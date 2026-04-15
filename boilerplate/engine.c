#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ================= TASK 3: CONFIG ================= */

#define LOG_DIR "logs"
#define LOG_CHUNK 256
#define BUFFER_CAP 16

/* ================= TASK 3: DATA ================= */

typedef struct {
    char container_id[32];
    size_t length;
    char data[LOG_CHUNK];
} log_item_t;

typedef struct {
    log_item_t items[BUFFER_CAP];
    int head, tail, count;
    int shutting_down;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    bounded_buffer_t log_buffer;
    pthread_t logger_thread;
    int should_stop;
} supervisor_ctx_t;

static supervisor_ctx_t *global_ctx = NULL;

/* ================= TASK 3: BUFFER ================= */

void buffer_init(bounded_buffer_t *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

void buffer_shutdown(bounded_buffer_t *b) {
    pthread_mutex_lock(&b->lock);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->lock);
}

void buffer_push(bounded_buffer_t *b, const log_item_t *item) {
    pthread_mutex_lock(&b->lock);

    /* TASK 3: block if full */
    while (b->count == BUFFER_CAP && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->lock);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->lock);
        return;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % BUFFER_CAP;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->lock);
}

int buffer_pop(bounded_buffer_t *b, log_item_t *item) {
    pthread_mutex_lock(&b->lock);

    /* TASK 3: block if empty */
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->lock);

    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->lock);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % BUFFER_CAP;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->lock);
    return 0;
}

/* ================= TASK 3: LOGGER ================= */

void *logger_thread(void *arg) {
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.length, f);
            fclose(f);
        }
    }

    printf("[Logger] Clean exit\n");
    return NULL;
}

/* ================= TASK 3: PRODUCER ================= */

typedef struct {
    int fd;
    supervisor_ctx_t *ctx;
    char id[32];
} producer_arg_t;

void *producer_thread(void *arg) {
    producer_arg_t *p = arg;
    char buf[LOG_CHUNK];

    while (1) {
        int n = read(p->fd, buf, sizeof(buf));
        if (n <= 0) break;

        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, p->id, sizeof(item.container_id)-1);
        item.length = n;
        memcpy(item.data, buf, n);

        buffer_push(&p->ctx->log_buffer, &item);
    }

    close(p->fd);
    free(p);
    return NULL;
}

/* ================= TASK 3: CONTAINER (DEMO) ================= */

void start_demo_container(supervisor_ctx_t *ctx, const char *id) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return;
    }

    pid_t pid = fork();

    if (pid == 0) {
        close(pipefd[0]);

        /* TASK 3: redirect stdout/stderr */
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        for (int i = 0; i < 5; i++) {
            printf("[%s] message %d\n", id, i);
            sleep(1);
        }

        close(pipefd[1]);
        exit(0);
    } else {
        close(pipefd[1]);

        producer_arg_t *p = malloc(sizeof(*p));
        p->fd = pipefd[0];
        p->ctx = ctx;
        strncpy(p->id, id, sizeof(p->id)-1);

        pthread_t tid;
        pthread_create(&tid, NULL, producer_thread, p);
        pthread_detach(tid);

        waitpid(pid, NULL, 0);
    }
}

/* ================= SIGNAL ================= */

void handle_signal(int sig) {
    (void)sig;
    if (!global_ctx) return;

    printf("[Supervisor] Signal received\n");

    global_ctx->should_stop = 1;
    buffer_shutdown(&global_ctx->log_buffer);
}

/* ================= SUPERVISOR ================= */

int run_supervisor(const char *rootfs) {
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    global_ctx = &ctx;

    buffer_init(&ctx.log_buffer);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    pthread_create(&ctx.logger_thread, NULL, logger_thread, &ctx);

    printf("Supervisor running (rootfs: %s)\n", rootfs);

    /* TASK 3: run multiple containers */
    start_demo_container(&ctx, "alpha");
    start_demo_container(&ctx, "beta");

    while (!ctx.should_stop) {
        sleep(1);
        break; /* demo exit */
    }

    printf("[Supervisor] Cleaning up...\n");

    buffer_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    printf("[Supervisor] Done\n");
    return 0;
}

/* ================= SIMPLE START (OPTIONAL) ================= */

int cmd_start(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <cmd>\n", argv[0]);
        return 1;
    }

    const char *id = argv[2];
    const char *rootfs = argv[3];
    const char *cmd = argv[4];

    pid_t pid = fork();

    if (pid == 0) {
        chroot(rootfs);
        chdir("/");
        execlp(cmd, cmd, NULL);
        perror("exec");
        exit(1);
    }

    printf("Started %s (PID %d)\n", id, pid);
    return 0;
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s supervisor <rootfs>\n", argv[0]);
        printf("  %s start <id> <rootfs> <cmd>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) {
        return cmd_start(argc, argv);
    }

    printf("Unknown command\n");
    return 1;
}
