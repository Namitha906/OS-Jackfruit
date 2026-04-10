#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int nice_value;
    int log_fd;
    int log_read_fd;
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_code;
    int exit_signal;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile sig_atomic_t should_stop;
    volatile sig_atomic_t sigchld_flag;
    pthread_t logger_thread;
    pthread_t pipe_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;

/* ---------------- utility ---------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int write_all(int fd, const void *buf, size_t count)
{
    const char *p = (const char *)buf;
    while (count > 0) {
        ssize_t n = write(fd, p, count);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        count -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t count)
{
    char *p = (char *)buf;
    while (count > 0) {
        ssize_t n = read(fd, p, count);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        count -= (size_t)n;
    }
    return 0;
}

static void ensure_zero_terminated(char *s, size_t n)
{
    if (n > 0)
        s[n - 1] = '\0';
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static void format_time(time_t t, char *buf, size_t sz)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* ---------------- bounded buffer ---------------- */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ---------------- metadata helpers ---------------- */

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strncmp(cur->id, id, CONTAINER_ID_LEN) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (cur->host_pid == pid)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static int rootfs_in_use_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if ((cur->state == CONTAINER_STARTING || cur->state == CONTAINER_RUNNING) &&
            strcmp(cur->rootfs, rootfs) == 0)
            return 1;
        cur = cur->next;
    }
    return 0;
}

/* ---------------- monitor hooks ---------------- */

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (monitor_fd < 0)
        return 0;

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (monitor_fd < 0)
        return 0;

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* ---------------- logger ---------------- */

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        pthread_mutex_lock(&ctx->metadata_lock);

        container_record_t *rec = find_container_locked(ctx, item.container_id);
        if (rec && rec->log_fd >= 0)
            write_all(rec->log_fd, item.data, item.length);

        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    return NULL;
}

static void *pipe_reader_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;

    while (!ctx->should_stop) {
        fd_set rfds;
        int maxfd = -1;
        int ready;
        struct timeval tv;

        FD_ZERO(&rfds);

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *cur = ctx->containers;
        while (cur) {
            if (cur->log_read_fd >= 0) {
                FD_SET(cur->log_read_fd, &rfds);
                if (cur->log_read_fd > maxfd)
                    maxfd = cur->log_read_fd;
            }
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (maxfd < 0) {
            usleep(100000);
            continue;
        }

        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            continue;
        }
        if (ready == 0)
            continue;

        pthread_mutex_lock(&ctx->metadata_lock);
        cur = ctx->containers;
        while (cur) {
            container_record_t *next = cur->next;
            if (cur->log_read_fd >= 0 && FD_ISSET(cur->log_read_fd, &rfds)) {
                char buf[LOG_CHUNK_SIZE];
                ssize_t n = read(cur->log_read_fd, buf, sizeof(buf));
                if (n > 0) {
                    log_item_t item;
                    memset(&item, 0, sizeof(item));
                    strncpy(item.container_id, cur->id, sizeof(item.container_id) - 1);
                    memcpy(item.data, buf, (size_t)n);
                    item.length = (size_t)n;
                    bounded_buffer_push(&ctx->log_buffer, &item);
                } else if (n == 0) {
                    close(cur->log_read_fd);
                    cur->log_read_fd = -1;
                } else if (errno != EINTR && errno != EAGAIN) {
                    close(cur->log_read_fd);
                    cur->log_read_fd = -1;
                }
            }
            cur = next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    return NULL;
}

/* ---------------- child / container ---------------- */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (sethostname(cfg->id, strlen(cfg->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount private");
        return 1;
    }

    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    if (cfg->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0) {
            perror("setpriority");
        }
    }

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }

    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }

    close(cfg->log_write_fd);

    execlp(cfg->command, cfg->command, (char *)NULL);
    perror("exec");
    return 127;
}

/* ---------------- signals / reap ---------------- */

static void handle_signal(int sig)
{
    if (!g_ctx)
        return;

    if (sig == SIGCHLD) {
        g_ctx->sigchld_flag = 1;
    } else if (sig == SIGINT || sig == SIGTERM) {
        g_ctx->should_stop = 1;
    }
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);

        container_record_t *rec = find_container_by_pid_locked(ctx, pid);
        if (rec) {
            if (WIFEXITED(status)) {
                rec->state = CONTAINER_EXITED;
                rec->exit_code = WEXITSTATUS(status);
                rec->exit_signal = 0;
            } else if (WIFSIGNALED(status)) {
                rec->state = CONTAINER_KILLED;
                rec->exit_code = -1;
                rec->exit_signal = WTERMSIG(status);
            } else {
                rec->state = CONTAINER_STOPPED;
            }

            if (rec->log_read_fd >= 0) {
                close(rec->log_read_fd);
                rec->log_read_fd = -1;
            }

            unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
        }

        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    ctx->sigchld_flag = 0;
}

/* ---------------- request handlers ---------------- */

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           control_response_t *resp)
{
    int pipefd[2] = {-1, -1};
    int log_fd = -1;
    void *stack = NULL;
    child_config_t *cfg = NULL;
    container_record_t *rec = NULL;
    pid_t pid;

    pthread_mutex_lock(&ctx->metadata_lock);

    if (find_container_locked(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Container id already exists: %s", req->container_id);
        return -1;
    }

    if (rootfs_in_use_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Rootfs already in use by another running container");
        return -1;
    }

    pthread_mutex_unlock(&ctx->metadata_lock);

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "mkdir logs failed: %s", strerror(errno));
        return -1;
    }

    if (pipe(pipefd) < 0) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "pipe failed: %s", strerror(errno));
        return -1;
    }

    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req->container_id);

    log_fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (log_fd < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "open log failed: %s", strerror(errno));
        return -1;
    }

    cfg = calloc(1, sizeof(*cfg));
    stack = malloc(STACK_SIZE);
    rec = calloc(1, sizeof(*rec));
    if (!cfg || !stack || !rec) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(log_fd);
        free(cfg);
        free(stack);
        free(rec);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "memory allocation failed");
        return -1;
    }

    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    pid = clone(child_fn,
                (char *)stack + STACK_SIZE,
                CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                cfg);
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(log_fd);
        free(cfg);
        free(stack);
        free(rec);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "clone failed: %s", strerror(errno));
        return -1;
    }

    close(pipefd[1]);

    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->nice_value = req->nice_value;
    rec->exit_code = -1;
    rec->exit_signal = 0;
    rec->log_fd = log_fd;
    rec->log_read_fd = pipefd[0];
    strncpy(rec->rootfs, req->rootfs, sizeof(rec->rootfs) - 1);
    strncpy(rec->command, req->command, sizeof(rec->command) - 1);
    strncpy(rec->log_path, log_path, sizeof(rec->log_path) - 1);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (register_with_monitor(ctx->monitor_fd,
                              req->container_id,
                              pid,
                              req->soft_limit_bytes,
                              req->hard_limit_bytes) < 0) {
        /* non-fatal for Task 1 */
    }

    resp->status = 0;
    resp->exit_code = 0;
    resp->exit_signal = 0;
    snprintf(resp->message, sizeof(resp->message),
             "Started container %s with PID %d", req->container_id, pid);
    return 0;
}

static int handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    char buf[CONTROL_MESSAGE_LEN];
    size_t used = 0;

    pthread_mutex_lock(&ctx->metadata_lock);

    used += snprintf(buf + used, sizeof(buf) - used, "ID PID STATE\n");
    container_record_t *cur = ctx->containers;
    while (cur && used < sizeof(buf) - 1) {
        used += snprintf(buf + used, sizeof(buf) - used, "%s %d %s\n",
                         cur->id, cur->host_pid, state_to_string(cur->state));
        cur = cur->next;
    }

    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    resp->exit_code = 0;
    resp->exit_signal = 0;
    strncpy(resp->message, buf, sizeof(resp->message) - 1);
    ensure_zero_terminated(resp->message, sizeof(resp->message));
    return 0;
}

static int handle_logs(supervisor_ctx_t *ctx, const char *id, control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *rec = find_container_locked(ctx, id);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "No such container: %s", id);
        return -1;
    }

    char path[PATH_MAX];
    strncpy(path, rec->log_path, sizeof(path) - 1);
    ensure_zero_terminated(path, sizeof(path));
    pthread_mutex_unlock(&ctx->metadata_lock);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "Cannot open log file");
        return -1;
    }

    size_t n = fread(resp->message, 1, sizeof(resp->message) - 1, fp);
    resp->message[n] = '\0';
    fclose(fp);

    resp->status = 0;
    resp->exit_code = 0;
    resp->exit_signal = 0;
    return 0;
}

static int handle_stop(supervisor_ctx_t *ctx, const char *id, control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *rec = find_container_locked(ctx, id);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "No such container: %s", id);
        return -1;
    }

    pid_t pid = rec->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (kill(pid, SIGTERM) < 0) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "Failed to stop %s: %s", id, strerror(errno));
        return -1;
    }

    resp->status = 0;
    resp->exit_code = 0;
    resp->exit_signal = 0;
    snprintf(resp->message, sizeof(resp->message), "Stop signal sent to %s", id);
    return 0;
}

/* ---------------- supervisor ---------------- */

static int setup_server_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    unlink(CONTROL_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        close(fd);
        unlink(CONTROL_PATH);
        return -1;
    }

    return fd;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sigaction sa;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.containers = NULL;
    pthread_mutex_init(&ctx.metadata_lock, NULL);

    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        fprintf(stderr, "Failed to initialize bounded buffer\n");
        return 1;
    }

    mkdir(LOG_DIR, 0755);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    /* not fatal if unavailable */

    ctx.server_fd = setup_server_socket();
    if (ctx.server_fd < 0) {
        perror("setup control socket");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    g_ctx = &ctx;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (pthread_create(&ctx.pipe_thread, NULL, pipe_reader_thread, &ctx) != 0) {
        perror("pthread_create pipe");
        ctx.should_stop = 1;
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    printf("Supervisor running on %s\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        int client_fd;
        control_request_t req;
        control_response_t resp;

        if (ctx.sigchld_flag)
            reap_children(&ctx);

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));

        if (read_all(client_fd, &req, sizeof(req)) < 0) {
            close(client_fd);
            continue;
        }

        ensure_zero_terminated(req.container_id, sizeof(req.container_id));
        ensure_zero_terminated(req.rootfs, sizeof(req.rootfs));
        ensure_zero_terminated(req.command, sizeof(req.command));

        switch (req.kind) {
        case CMD_START:
        case CMD_RUN:
            start_container(&ctx, &req, &resp);
            break;
        case CMD_PS:
            handle_ps(&ctx, &resp);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, req.container_id, &resp);
            break;
        case CMD_STOP:
            handle_stop(&ctx, req.container_id, &resp);
            break;
        default:
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Unknown command");
            break;
        }

        write_all(client_fd, &resp, sizeof(resp));
        close(client_fd);
    }

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING)
            kill(cur->host_pid, SIGTERM);
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(1);
    reap_children(&ctx);

    ctx.should_stop = 1;
    bounded_buffer_begin_shutdown(&ctx.log_buffer);

    pthread_join(ctx.pipe_thread, NULL);
    pthread_join(ctx.logger_thread, NULL);

    pthread_mutex_lock(&ctx.metadata_lock);
    cur = ctx.containers;
    while (cur) {
        container_record_t *next = cur->next;
        if (cur->log_fd >= 0)
            close(cur->log_fd);
        if (cur->log_read_fd >= 0)
            close(cur->log_read_fd);
        free(cur);
        cur = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    return 0;
}

/* ---------------- client side ---------------- */

static int send_control_request(const control_request_t *req, control_response_t *resp_out)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (write_all(fd, req, sizeof(*req)) < 0) {
        perror("write request");
        close(fd);
        return 1;
    }

    if (read_all(fd, &resp, sizeof(resp)) < 0) {
        perror("read response");
        close(fd);
        return 1;
    }

    close(fd);

    if (resp_out)
        *resp_out = resp;

    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    if (send_control_request(&req, &resp) != 0) {
        fprintf(stderr, "%s\n", resp.message);
        return 1;
    }

    printf("%s\n", resp.message);
    return 0;
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;
    int rc;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    rc = send_control_request(&req, &resp);
    if (rc != 0) {
        fprintf(stderr, "%s\n", resp.message);
        return 1;
    }

    printf("%s\n", resp.message);
    /*
     * For Task 1 we use the same start path.
     * This sends the request successfully; true blocking run semantics
     * are usually added in Task 2 with richer IPC/status tracking.
     */
    return 0;
}

static int cmd_ps(void)
{
    control_request_t req;
    control_response_t resp;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    if (send_control_request(&req, &resp) != 0) {
        fprintf(stderr, "%s\n", resp.message);
        return 1;
    }

    printf("%s", resp.message);
    return 0;
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (send_control_request(&req, &resp) != 0) {
        fprintf(stderr, "%s\n", resp.message);
        return 1;
    }

    printf("%s", resp.message);
    return 0;
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (send_control_request(&req, &resp) != 0) {
        fprintf(stderr, "%s\n", resp.message);
        return 1;
    }

    printf("%s\n", resp.message);
    return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}

 


