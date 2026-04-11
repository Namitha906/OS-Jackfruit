#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <sched.h>
#include <sys/mount.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>

#define MAX_CONTAINERS 20
#define FIFO_PATH "/tmp/cmd_pipe"
#define BUFFER_SIZE 10

/* ================= STRUCT ================= */
struct container {
    char id[32];
    pid_t pid;
    char state[16];
};

struct container containers[MAX_CONTAINERS];
int count = 0;

/* ================= BUFFER ================= */
char buffer_log[BUFFER_SIZE][256];
int in = 0, out = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

/* ================= PRODUCER ================= */
void *producer(void *arg)
{
    int fd = *(int *)arg;
    char temp[256];

    while (read(fd, temp, sizeof(temp)) > 0) {

        pthread_mutex_lock(&lock);

        while ((in + 1) % BUFFER_SIZE == out)
            pthread_cond_wait(&not_full, &lock);

        strcpy(buffer_log[in], temp);
        in = (in + 1) % BUFFER_SIZE;

        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&lock);
    }

    return NULL;
}

/* ================= CONSUMER ================= */
void *consumer(void *arg)
{
    FILE *fp = fopen("container.log", "a");

    while (1) {
        pthread_mutex_lock(&lock);

        while (in == out)
            pthread_cond_wait(&not_empty, &lock);

        fprintf(fp, "%s", buffer_log[out]);
        fflush(fp);

        out = (out + 1) % BUFFER_SIZE;

        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&lock);
    }

    fclose(fp);
    return NULL;
}

/* ================= FIND ================= */
int find_container(char *id)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(containers[i].id, id) == 0)
            return i;
    }
    return -1;
}

/* ================= SIGCHLD ================= */
void handle_sigchld(int sig)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < count; i++) {
            if (containers[i].pid == pid) {
                strcpy(containers[i].state, "stopped");
                printf("Container %s exited\n", containers[i].id);
            }
        }
    }
}

/* ================= START ================= */
void start_container(char *id, char *rootfs, char **args)
{
    int pipefd[2];
    pipe(pipefd);

    pid_t pid = fork();

    if (pid == 0) {
        close(pipefd[0]);

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS);

        if (chroot(rootfs) != 0) {
            perror("chroot failed");
            exit(1);
        }

        chdir("/");
        mount("proc", "/proc", "proc", 0, NULL);

        execv(args[0], args);

        perror("exec failed");
        exit(1);
    }
    else {
        close(pipefd[1]);

        pthread_t prod, cons;
        pthread_create(&prod, NULL, producer, &pipefd[0]);
        pthread_create(&cons, NULL, consumer, NULL);

        strcpy(containers[count].id, id);
        containers[count].pid = pid;
        strcpy(containers[count].state, "running");
        count++;

        printf("Started container %s (PID %d)\n", id, pid);
    }
}

/* ================= STOP ================= */
void stop_container(char *id)
{
    int idx = find_container(id);

    if (idx == -1) {
        printf("Container not found\n");
        return;
    }

    kill(containers[idx].pid, SIGTERM);
    strcpy(containers[idx].state, "stopped");

    printf("Stopped %s\n", id);
}

/* ================= PS ================= */
void list_containers()
{
    printf("\nID\tPID\tSTATE\n");

    for (int i = 0; i < count; i++) {
        printf("%s\t%d\t%s\n",
               containers[i].id,
               containers[i].pid,
               containers[i].state);
    }

    printf("\n");
}

/* ================= MAIN ================= */
int main()
{
    signal(SIGCHLD, handle_sigchld);

    printf("Supervisor started...\n");

    mkfifo(FIFO_PATH, 0666);

    while (1) {

        FILE *fp = fopen(FIFO_PATH, "r");
        if (!fp) continue;

        char buffer[256];

        if (fgets(buffer, sizeof(buffer), fp) != NULL) {

            char *token;
            char cmd[32], id[32], rootfs[64];

            token = strtok(buffer, " ");
            if (!token) continue;
            strcpy(cmd, token);

            token = strtok(NULL, " ");
            if (!token) continue;
            strcpy(id, token);

            token = strtok(NULL, " ");
            if (!token) continue;
            strcpy(rootfs, token);

            char *args[10];
            int i = 0;

            while ((token = strtok(NULL, " \n")) != NULL) {
                args[i++] = token;
            }
            args[i] = NULL;

            if (strcmp(cmd, "start") == 0) {
                start_container(id, rootfs, args);
            }
            else if (strcmp(cmd, "ps") == 0) {
                list_containers();
            }
            else if (strcmp(cmd, "stop") == 0) {
                stop_container(id);
            }
        }

        fclose(fp);
    }

    return 0;
}
