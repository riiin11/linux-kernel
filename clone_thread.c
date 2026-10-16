#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <string.h>

#define STACK_SIZE (1024 * 1024)

static int shared_counter = 0;

static int thread_func(void *arg)
{
    pid_t tid = syscall(SYS_gettid);
    printf("[clone-thread] TID=%d, counter before=%d\n", tid, shared_counter);
    shared_counter++;
    printf("[clone-thread] TID=%d, counter after =%d\n", tid, shared_counter);
    return 0;
}

int main(void)
{
    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc"); return 1; }
    char *stack_top = stack + STACK_SIZE;

    shared_counter = 42;
    printf("[main] PID=%d, TID=%d, counter=%d\n",
           getpid(), syscall(SYS_gettid), shared_counter);

    pid_t tid = clone(thread_func, stack_top,
                      CLONE_VM | CLONE_FS | CLONE_FILES |
                      CLONE_SIGHAND | CLONE_THREAD | SIGCHLD,
                      NULL);
    if (tid == -1) { perror("clone"); return 1; }

    sleep(1);

    printf("[main] after clone: counter=%d (ожидаем 43)\n", shared_counter);
    free(stack);
    return 0;
}
