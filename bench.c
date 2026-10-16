#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define STACK_SIZE (256 * 1024)
#define WORK_ITERS 10000

/* Локальная работа: каждый поток/процесс крутит свой счётчик.
   Никакой общей памяти, никакой синхронизации — измеряем
   чистую стоимость создания задачи. */
static void do_work(void)
{
    volatile long local = 0;
    for (int i = 0; i < WORK_ITERS; i++)
        local += i;
    asm volatile("" : : "r"(local) : "memory");
}

/* ---------- fork ---------- */
static void run_fork(int n)
{
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) {
            do_work();
            _exit(0);
        }
    }
    for (int i = 0; i < n; i++)
        wait(NULL);
}

/* ---------- pthread ---------- */
static void *pthread_entry(void *a)
{
    do_work();
    return NULL;
}

static void run_pthread(int n)
{
    pthread_t *ths = calloc(n, sizeof(*ths));
    if (!ths) { perror("calloc"); exit(1); }
    for (int i = 0; i < n; i++)
        pthread_create(&ths[i], NULL, pthread_entry, NULL);
    for (int i = 0; i < n; i++)
        pthread_join(ths[i], NULL);
    free(ths);
}

/* ---------- clone ---------- */
static int clone_entry(void *a)
{
    do_work();
    return 0;
}

static void run_clone(int n)
{
    char **stacks = calloc(n, sizeof(char *));
    if (!stacks) { perror("calloc"); exit(1); }
    for (int i = 0; i < n; i++) {
        stacks[i] = malloc(STACK_SIZE);
        if (!stacks[i]) { perror("malloc"); exit(1); }
        clone(clone_entry, stacks[i] + STACK_SIZE,
              CLONE_VM | CLONE_FS | CLONE_FILES |
              CLONE_SIGHAND | CLONE_THREAD | SIGCHLD,
              NULL);
    }
    /* Ждём завершения всех clone-потоков */
    while (waitpid(-1, NULL, __WCLONE) > 0) {}
    for (int i = 0; i < n; i++)
        free(stacks[i]);
    free(stacks);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s {fork|pthread|clone} N\n", argv[0]);
        return 1;
    }
    const char *mode = argv[1];
    int n = atoi(argv[2]);
    if (n <= 0) { fprintf(stderr, "N must be > 0\n"); return 1; }

    if (!strcmp(mode, "fork"))         run_fork(n);
    else if (!strcmp(mode, "pthread")) run_pthread(n);
    else if (!strcmp(mode, "clone"))   run_clone(n);
    else { fprintf(stderr, "unknown mode: %s\n", mode); return 1; }

    return 0;
}
