```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>

#define CONTAINER_ID_LEN 32
#define LOG_BUFFER_CAPACITY 16
#define LOG_CHUNK_SIZE 4096
#define MAX_CONTAINERS 10

// ===================== DATA STRUCTURES =====================

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
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    bounded_buffer_t log_buffer;
    pthread_t logger_thread;
} supervisor_ctx_t;

supervisor_ctx_t *GLOBAL_CTX = NULL;

typedef struct {
    int pipe_fd;
    char container_id[CONTAINER_ID_LEN];
    supervisor_ctx_t *ctx;
} producer_arg_t;


// ===================== BUFFER =====================

int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
    return 0;
}

void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}


// ===================== LOGGING THREAD =====================

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        char path[128];
        snprintf(path, sizeof(path), "logs/%s.log", item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) continue;

        write(fd, item.data, item.length);
        close(fd);
    }
    return NULL;
}


// ===================== PRODUCER =====================

void *producer_thread(void *arg)
{
    producer_arg_t *p = arg;
    log_item_t item;

    strncpy(item.container_id, p->container_id, CONTAINER_ID_LEN);

    while (1) {
        ssize_t n = read(p->pipe_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) break;

        item.length = n;
        bounded_buffer_push(&p->ctx->log_buffer, &item);
    }

    close(p->pipe_fd);
    free(p);
    return NULL;
}


// ===================== CMD RUN =====================

int cmd_run(char *id, char *rootfs, char *cmd)
{
    int pipefd[2];
    pipe(pipefd);

    pid_t pid = fork();

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        close(pipefd[0]);
        close(pipefd[1]);

        chroot(rootfs);
        chdir("/");

        execlp(cmd, cmd, NULL);
        perror("exec");
        exit(1);
    }

    close(pipefd[1]);

    producer_arg_t *p = malloc(sizeof(producer_arg_t));
    p->pipe_fd = pipefd[0];
    strcpy(p->container_id, id);
    p->ctx = GLOBAL_CTX;

    pthread_t t;
    pthread_create(&t, NULL, producer_thread, p);

    containers[container_count++] = (container_t){0};
    strcpy(containers[container_count-1].id, id);
    containers[container_count-1].pid = pid;
    containers[container_count-1].running = 1;

    printf("Started container %s with PID %d\n", id, pid);
    return pid;
}


// ===================== SUPERVISOR =====================

int run_supervisor()
{
    supervisor_ctx_t ctx;
    GLOBAL_CTX = &ctx;

    bounded_buffer_init(&ctx.log_buffer);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    char input[256];

    while (1) {
        printf("engine> ");
        fflush(stdout);

        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "exit") == 0)
            break;

        if (strncmp(input, "run", 3) == 0) {
            char id[32], root[128], cmd[128];
            sscanf(input, "run %s %s %s", id, root, cmd);
            cmd_run(id, root, cmd);
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

        // reap children
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < container_count; i++) {
                if (containers[i].pid == pid)
                    containers[i].running = 0;
            }
        }
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    return 0;
}


// ===================== MAIN =====================

int main(int argc, char *argv[])
{
    mkdir("logs", 0755);

    if (argc < 2) {
        printf("Usage: ./engine supervisor\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        return run_supervisor();
    }

    printf("Invalid command\n");
    return 1;
}
```
