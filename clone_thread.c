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

        pid_t tid = clone(
        thread_func,   /* функция, с которой стартует новый поток */
        stack_top,     /* верхушка стека нового потока */

        /* Флаги разделения ресурсов — делают clone() похожим на pthread_create(): */
        CLONE_VM        |  /* общее адресное пространство (mm_struct):
                              изменения памяти видны всем потокам */
        CLONE_FS        |  /* общий cwd, root, umask (fs_struct):
                              chdir() в одном потоке виден остальным */
        CLONE_FILES     |  /* общая таблица файловых дескрипторов (files_struct):
                              открытие/закрытие fd видно всем потокам */
        CLONE_SIGHAND   |  /* общие обработчики сигналов (sighand_struct):
                              sigaction() меняет поведение для всех */
        CLONE_THREAD    |  /* поток в той же thread group:
                              общий TGID, getpid() одинаков у всех */
        SIGCHLD,           /* сигнал родителю при завершении потока */

        NULL               /* аргумент для thread_func (не используется) */
    );
    if (tid == -1) { perror("clone"); return 1; }

    sleep(1);

    printf("[main] after clone: counter=%d (ожидаем 43)\n", shared_counter);
    free(stack);
    return 0;
}
