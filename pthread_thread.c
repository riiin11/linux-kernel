#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

static int shared_counter = 0;

static void *thread_func(void *arg)
{
    printf("[pthread] TID=%d, counter=%d\n",
           syscall(SYS_gettid), shared_counter);
    shared_counter++;
    return NULL;
}

int main(void)
{
    shared_counter = 42;
    pthread_t th;
    pthread_create(&th, NULL, thread_func, NULL);
    pthread_join(th, NULL);
    printf("[main] counter=%d\n", shared_counter);
    return 0;
}
