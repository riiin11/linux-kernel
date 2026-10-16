#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_ARGS   64
#define MAX_CMDS   16

typedef enum { MODE_FORK, MODE_PTHREAD } pipe_mode_t;
static pipe_mode_t pipe_mode = MODE_FORK;

/* =========================================================
 *  Парсинг конвейера
 * ========================================================= */
static int parse_pipeline(char *line, char **cmds, int max)
{
    int n = 0;
    char *save1, *tok = strtok_r(line, "|", &save1);
    while (tok && n < max) {
        while (*tok == ' ') tok++;
        cmds[n++] = tok;
        tok = strtok_r(NULL, "|", &save1);
    }
    return n;
}

static int split_args(char *cmd, char **argv, int max)
{
    int n = 0;
    char *save, *tok = strtok_r(cmd, " \t\n", &save);
    while (tok && n < max - 1) {
        argv[n++] = tok;
        tok = strtok_r(NULL, " \t\n", &save);
    }
    argv[n] = NULL;
    return n;
}

/* =========================================================
 *  PTHREAD-режим: каждый этап — поток, данные между этапами
 *  идут через pipe(), синхронизируемый ядром.
 * ========================================================= */
typedef struct {
    char *cmdline;
    int   in_fd;   /* -1 для первого этапа */
    int   out_fd;  /* -1 для последнего этапа */
} stage_arg_t;

static void *pthread_stage(void *arg)
{
    stage_arg_t *a = (stage_arg_t *)arg;
    char *argv[MAX_ARGS];
    split_args(a->cmdline, argv, MAX_ARGS);
    if (!argv[0]) return NULL;

    pid_t pid = fork();
    if (pid == 0) {
        if (a->in_fd  >= 0) dup2(a->in_fd,  0);
        if (a->out_fd >= 0) dup2(a->out_fd, 1);

        /* КРИТИЧНО: закрываем ВСЕ унаследованные fd (3..1023),
           чтобы не держать write-концы pipe и получать EOF */
        for (int fd = 3; fd < 1024; fd++)
            close(fd);

        execvp(argv[0], argv);
        _exit(127);
    }

    if (a->in_fd  >= 0) close(a->in_fd);
    if (a->out_fd >= 0) close(a->out_fd);

    waitpid(pid, NULL, 0);
    return NULL;
}

static void run_pipeline_pthread(char **cmds, int n)
{
    int   (*pipes)[2] = calloc(n > 1 ? n - 1 : 1, sizeof(int[2]));
    pthread_t *ths    = calloc(n, sizeof(pthread_t));
    stage_arg_t *args = calloc(n, sizeof(stage_arg_t));

    if (!pipes || !ths || !args) { perror("calloc"); exit(1); }

    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) { perror("pipe"); exit(1); }
    }

    /* Готовим аргументы ДО запуска потоков */
    for (int i = 0; i < n; i++) {
        args[i].cmdline = cmds[i];
        args[i].in_fd   = (i == 0)     ? -1 : pipes[i - 1][0];
        args[i].out_fd  = (i == n - 1) ? -1 : pipes[i][1];
        pthread_create(&ths[i], NULL, pthread_stage, &args[i]);
    }

    /* Ждём завершения всех потоков, потом закрываем ВСЕ копии
       (потоки уже сделали свои fork и закрыли внутри) */
    for (int i = 0; i < n; i++)
        pthread_join(ths[i], NULL);

    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    free(pipes); free(ths); free(args);
}
/* =========================================================
 *  FORK-режим: классический fork+pipe
 * ========================================================= */
static void run_pipeline_fork(char **cmds, int n)
{
    int prev_pipe = -1;
    for (int i = 0; i < n; i++) {
        int fds[2];
        if (i < n - 1) {
            if (pipe(fds) < 0) { perror("pipe"); return; }
        }

        pid_t pid = fork();
        if (pid == 0) {
            if (prev_pipe != -1) { dup2(prev_pipe, 0); close(prev_pipe); }
            if (i < n - 1) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); }
            char *argv[MAX_ARGS];
            split_args(cmds[i], argv, MAX_ARGS);
            execvp(argv[0], argv);
            _exit(127);
        }
        if (prev_pipe != -1) close(prev_pipe);
        if (i < n - 1) { close(fds[1]); prev_pipe = fds[0]; }
    }
    for (int i = 0; i < n; i++) wait(NULL);
}

/* =========================================================
 *  main
 * ========================================================= */
int main(void)
{
    char line[4096];
    printf("minishell (режим: fork). Команды: set pipeline-mode fork|pthread, exit\n");

    while (1) {
        printf("> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0) continue;
        if (!strcmp(line, "exit")) break;

        if (!strncmp(line, "set pipeline-mode ", 18)) {
            const char *m = line + 18;
            if (!strcmp(m, "fork"))         { pipe_mode = MODE_FORK;    printf("режим: fork\n"); }
            else if (!strcmp(m, "pthread")) { pipe_mode = MODE_PTHREAD; printf("режим: pthread\n"); }
            else printf("usage: set pipeline-mode fork|pthread\n");
            continue;
        }

        char *copy = strdup(line);
        char *cmds[MAX_CMDS];
        int n = parse_pipeline(copy, cmds, MAX_CMDS);

        if (n == 1) {
            char *argv[MAX_ARGS];
            split_args(cmds[0], argv, MAX_ARGS);
            if (!argv[0]) { free(copy); continue; }
            pid_t pid = fork();
            if (pid == 0) { execvp(argv[0], argv); _exit(127); }
            waitpid(pid, NULL, 0);
        } else if (pipe_mode == MODE_FORK) {
            run_pipeline_fork(cmds, n);
        } else {
            run_pipeline_pthread(cmds, n);
        }
        free(copy);
    }
    return 0;
}
