#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>

#define CONTAINER_ID_LEN 32
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define MAX_CONTAINERS 10
#define LOG_DIR "logs"

/* ================= DATA STRUCTURES ================= */

typedef struct {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    int running;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    int head, tail, count;
    int shutdown;

    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} bounded_buffer_t;

typedef struct {
    bounded_buffer_t buffer;
    pthread_t logger_thread;
} supervisor_ctx_t;

typedef struct {
    int pipe_fd;
    char container_id[CONTAINER_ID_LEN];
    supervisor_ctx_t *ctx;
} producer_arg_t;

/* ================= BUFFER ================= */

void buffer_init(bounded_buffer_t *b) {
    b->head = b->tail = b->count = 0;
    b->shutdown = 0;
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_full, NULL);
    pthread_cond_init(&b->not_empty, NULL);
}

void buffer_push(bounded_buffer_t *b, log_item_t *item) {
    pthread_mutex_lock(&b->mutex);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutdown) {
        pthread_mutex_unlock(&b->mutex);
        return;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
}

int buffer_pop(bounded_buffer_t *b, log_item_t *item) {
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0 && b->shutdown) {
        pthread_mutex_unlock(&b->mutex);
        return 0;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 1;
}

void buffer_shutdown(bounded_buffer_t *b) {
    pthread_mutex_lock(&b->mutex);
    b->shutdown = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/* ================= LOGGER ================= */

void *logger_thread(void *arg) {
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    mkdir(LOG_DIR, 0777);

    while (buffer_pop(&ctx->buffer, &item)) {

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0) continue;

        write(fd, item.data, item.length);
        close(fd);
    }

    return NULL;
}

/* ================= PRODUCER ================= */

void *producer_thread(void *arg) {
    producer_arg_t *p = arg;

    log_item_t item;
    strncpy(item.container_id, p->container_id, CONTAINER_ID_LEN);

    while (1) {
        ssize_t n = read(p->pipe_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) break;

        item.length = n;
        buffer_push(&p->ctx->buffer, &item);
    }

    close(p->pipe_fd);
    free(p);
    return NULL;
}

/* ================= RUN CONTAINER ================= */

int run_container(supervisor_ctx_t *ctx, char *id, char *rootfs, char *cmd) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid == 0) {
        /* CHILD */

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        close(pipefd[0]);
        close(pipefd[1]);

        if (chroot(rootfs) < 0) {
            perror("chroot");
            exit(1);
        }

        chdir("/");
        mount("proc", "/proc", "proc", 0, NULL);

        execl(cmd, cmd, NULL);

        perror("exec");
        exit(1);
    }

    /* PARENT */
    close(pipefd[1]);

    producer_arg_t *p = malloc(sizeof(producer_arg_t));
    p->pipe_fd = pipefd[0];
    strncpy(p->container_id, id, CONTAINER_ID_LEN);
    p->ctx = ctx;

    pthread_t t;
    pthread_create(&t, NULL, producer_thread, p);

    printf("Started container %s (PID %d)\n", id, pid);

    containers[container_count].pid = pid;
    strcpy(containers[container_count].id, id);
    containers[container_count].running = 1;
    container_count++;

    return pid;
}

/* ================= SUPERVISOR ================= */

void supervisor() {
    supervisor_ctx_t ctx;
    buffer_init(&ctx.buffer);

    pthread_create(&ctx.logger_thread, NULL, logger_thread, &ctx);

    char input[256];

    while (1) {
        printf("engine> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) continue;
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "exit") == 0) break;

        if (strncmp(input, "run", 3) == 0) {
            char id[32], root[128], cmd[128];

            if (sscanf(input, "run %s %s %s", id, root, cmd) != 3) {
                printf("Invalid command\n");
                continue;
            }

            run_container(&ctx, id, root, cmd);
        }

        else if (strcmp(input, "ps") == 0) {
            printf("ID\tPID\tSTATUS\n");
            for (int i = 0; i < container_count; i++) {
                printf("%s\t%d\t%s\n",
                       containers[i].id,
                       containers[i].pid,
                       containers[i].running ? "running" : "stopped");
            }
        }

        /* cleanup exited containers */
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < container_count; i++) {
                if (containers[i].pid == pid)
                    containers[i].running = 0;
            }
        }
    }

    buffer_shutdown(&ctx.buffer);
    pthread_join(ctx.logger_thread, NULL);
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: ./engine supervisor\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        supervisor();
    }

    return 0;
}
