# Лабораторная работа: Потоки, clone(), kprobe, pthread-конвейер

**Автор:** Раджабов Р.  
**Дата:** 16 октября 2026  
**Ядро хоста:** Linux 7.0.0-38-generic (aarch64, Ubuntu 26.04)  
**Ядро гостя (QEMU):** Linux 7.0.14 (собрано вручную с CONFIG_FTRACE, CONFIG_SCHEDSTATS)  
**Платформа:** Apple Silicon M3, UTM (ARM64 виртуализация)

---

## Содержание

1. [Часть 1. Ручная реализация потока через clone()](#часть-1)
2. [Часть 2. Бенчмарк fork/pthread/clone](#часть-2)
3. [Часть 3. Тестирование в кастомном ядре](#часть-3)
4. [Часть 4. kprobe на kernel_clone](#часть-4)
5. [Часть 5. pthread-конвейер в minishell](#часть-5)
6. [Заключение](#заключение)

---

<a name="часть-1"></a>
## Часть 1. Ручная реализация потока через clone()

### 1.1. Флаги clone() и их назначение

Вызов `clone()` создаёт новую задачу в ядре Linux. Поведение определяется набором флагов `CLONE_*`. Чтобы получить поведение, эквивалентное `pthread_create()`, нужна следующая комбинация:

| Флаг | Что разделяет | Что даёт |
|---|---|---|
| `CLONE_VM` | `mm_struct` (адресное пространство) | Изменения памяти видны всем потокам |
| `CLONE_FS` | `fs_struct` (cwd, root, umask) | `chdir()` в одном потоке виден остальным |
| `CLONE_FILES` | `files_struct` (таблица fd) | `open()`/`close()` видны всем потокам |
| `CLONE_SIGHAND` | `sighand_struct` (обработчики сигналов) | Один `sigaction()` меняет для всех |
| `CLONE_THREAD` | `signal_struct` + TGID | Поток в той же thread group, общий `getpid()` |
| `SIGCHLD` | — | Сигнал родителю при завершении задачи |

**Почему именно так:** без `CLONE_VM` задача получила бы **копию** адресного пространства (как fork). Без `CLONE_THREAD` — стала бы отдельным процессом (свой TGID). Без `CLONE_SIGHAND` — обработчики сигналов были бы независимы.

### 1.2. Исходный код clone_thread.c

```c
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

    /* Флаги разделения ресурсов — делают clone() похожим на pthread_create():
       CLONE_VM       — общее адресное пространство (mm_struct)
       CLONE_FS       — общий cwd, root, umask (fs_struct)
       CLONE_FILES    — общая таблица файловых дескрипторов (files_struct)
       CLONE_SIGHAND  — общие обработчики сигналов (sighand_struct)
       CLONE_THREAD   — поток в той же thread group (общий TGID)
       SIGCHLD        — сигнал родителю при завершении потока */
    pid_t tid = clone(
        thread_func,       /* точка входа нового потока */
        stack_top,         /* верхушка стека (clone требует явно) */
        CLONE_VM | CLONE_FS | CLONE_FILES |
        CLONE_SIGHAND | CLONE_THREAD | SIGCHLD,
        NULL
    );
    if (tid == -1) { perror("clone"); return 1; }

    sleep(1);  /* грубая синхронизация */

    printf("[main] after clone: counter=%d (ожидаем 43)\n", shared_counter);
    free(stack);
    return 0;
}
```

### 1.3. Эквивалент на pthread

```c
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

    /* Внутри pthread_create() glibc вызывает clone() с флагами:
       CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
       CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID */
    pthread_create(&th, NULL, thread_func, NULL);
    pthread_join(th, NULL);
    printf("[main] counter=%d\n", shared_counter);
    return 0;
}
```

### 1.4. Демонстрация разделения памяти

Вывод `./clone_thread` (файл `logs/part1_clone.txt`):

```
[main] PID=11800, TID=11800, counter=42
[clone-thread] TID=11801, counter before=42
[clone-thread] TID=11801, counter after =43
[main] after clone: counter=43 (ожидаем 43)
```

**Анализ:**

- **PID = 11800** у main и **TID = 11801** у потока — разные TID, но один PID (это потоки одного процесса, а не разные процессы).
- **counter = 42 → 43**: clone-поток изменил переменную, и main это увидел → `CLONE_VM` действительно работает, память общая.

Вывод `./pthread_thread` (`logs/part1_pthread.txt`):

```
[pthread] TID=11835, counter=42
[main] counter=43
```

**Вывод:** clone-поток и pthread-поток ведут себя **идентично** — оба видят общую память, оба имеют свой TID в рамках одного PID.

---

<a name="часть-2"></a>
## Часть 2. Бенчмарк fork/pthread/clone

### 2.1. Исходный код bench.c

```c
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

/* Локальная работа: измеряем чистую стоимость создания задачи,
   без синхронизации на общей переменной. */
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
        if (p == 0) { do_work(); _exit(0); }
    }
    for (int i = 0; i < n; i++) wait(NULL);
}

/* ---------- pthread ---------- */
static void *pthread_entry(void *a) { do_work(); return NULL; }

static void run_pthread(int n)
{
    pthread_t *ths = calloc(n, sizeof(*ths));
    for (int i = 0; i < n; i++)
        pthread_create(&ths[i], NULL, pthread_entry, NULL);
    for (int i = 0; i < n; i++)
        pthread_join(ths[i], NULL);
    free(ths);
}

/* ---------- clone ---------- */
static int clone_entry(void *a) { do_work(); return 0; }

static void run_clone(int n)
{
    char **stacks = calloc(n, sizeof(char *));
    pid_t *tids   = calloc(n, sizeof(pid_t));
    for (int i = 0; i < n; i++) {
        stacks[i] = malloc(STACK_SIZE);
        tids[i] = clone(clone_entry, stacks[i] + STACK_SIZE,
                        CLONE_VM | CLONE_FS | CLONE_FILES |
                        CLONE_SIGHAND | CLONE_THREAD | SIGCHLD,
                        NULL);
    }
    for (int i = 0; i < n; i++)
        waitpid(tids[i], NULL, __WCLONE);
    for (int i = 0; i < n; i++) free(stacks[i]);
    free(stacks); free(tids);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s {fork|pthread|clone} N\n", argv[0]); return 1; }
    const char *mode = argv[1];
    int n = atoi(argv[2]);

    if (!strcmp(mode, "fork"))         run_fork(n);
    else if (!strcmp(mode, "pthread")) run_pthread(n);
    else if (!strcmp(mode, "clone"))   run_clone(n);
    else { fprintf(stderr, "unknown mode\n"); return 1; }
    return 0;
}
```

### 2.2. Результаты perf stat (среднее по 5 запускам, N=1000)

| Режим | task-clock, мс | context-switches | time elapsed, с |
|---|---|---|---|
| **fork** | 41.39 | **17.6** | 0.0234 |
| **pthread** | 23.03 | 287.6 | 0.0147 |
| **clone** | **26.33** | 383.4 | 0.0145 |

Полные логи: `logs/part2_perf_fork.txt`, `logs/part2_perf_pthread.txt`, `logs/part2_perf_clone.txt`.

### 2.3. Анализ

**Почему fork показывает мало context-switches (17):**

`perf stat` без флага `-a` считает счётчики только того процесса, который был запущен напрямую — то есть `./bench`. Когда `fork()` создаёт 1000 дочерних процессов, **их работа не попадает в perf**. Это классическая ловушка измерения.

**Почему fork тратит больше task-clock (41 мс):**

Даже с COW, `fork()` обязан:

- создать новую `task_struct`,
- создать новую `mm_struct`,
- **скопировать таблицу страниц** родителя,
- сбросить TLB при первом переключении на новый процесс (меняется `CR3`).

**Почему pthread/clone быстрее:**

Они шарят `mm_struct` (`CLONE_VM`), поэтому:

- не нужно копировать таблицу страниц,
- не сбрасывается TLB,
- переключение контекста дешевле (CR3 не меняется).

**Почему context-switches у pthread/clone в 15–20 раз больше:**

Потому что perf видит работу всех 1000 потоков внутри одного процесса. Каждое переключение между потоками считается.

### 2.4. Дополнительный эксперимент: с общей переменной

Первый вариант `bench.c` (файл `wallclock.log`) использовал общий атомарный счётчик `__sync_fetch_and_add`. Результат **перевернулся**:

```
=== fork ===
0.01 sec  1124 KB
0.02 sec  1144 KB
...
=== pthread ===
0.09 sec  9336 KB
0.10 sec  9336 KB
...
=== clone ===
0.10 sec  12004 KB
...
```

**Fork стал быстрее**, потому что его дети работают с **приватными копиями** переменной (COW). А pthread/clone стучатся в **одну cache line**, вызывая cache line bouncing и сериализацию.

**Вывод:** узкое место смещается со **создания задачи** (когда работа изолирована) на **синхронизацию** (когда память общая). Это классический trade-off.

---

<a name="часть-3"></a>
## Часть 3. Тестирование в кастомном ядре

### 3.1. Конфигурация кастомного ядра

Из `logs/part3_kernel_config.txt`:

```
CONFIG_SCHEDSTATS=y
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
```

Опции включены через `make menuconfig`:

- `Kernel hacking → Tracers → [*] Tracers`
- `Kernel hacking → Tracers → [*] Kernel Function Tracer`
- `Kernel hacking → Collect scheduler statistics`

Дополнительно очищены Ubuntu-специфичные пути сертификатов:

```
CONFIG_SYSTEM_TRUSTED_KEYS=""
CONFIG_MODULE_SIG_KEY=""
CONFIG_SYSTEM_REVOCATION_KEYS=""
```

### 3.2. Сборка и упаковка

```bash
# Собрали ядро
make -j4
# Получили arch/arm64/boot/Image

# Собрали initramfs с busybox и нашими бинарниками
mkdir -p ~/initramfs/{bin,sbin,etc,proc,sys,dev,tmp,root}
cp /usr/bin/busybox ~/initramfs/bin/busybox
cp ~/lab/bench_static ~/lab/clone_thread_static ~/initramfs/root/
cd ~/initramfs && find . -print0 | cpio --null -ov --format=newc | gzip -9 > ~/initramfs.img
```

Информация о собранном ядре (`logs/part3_image.txt`):

```
/home/rin4/linux-source-7.0.0/arch/arm64/boot/Image: Linux kernel ARM64 boot executable Image, little-endian, 4K pages
```

### 3.3. Лог загрузки кастомного ядра

Из `logs/part3_qemu.txt`:

```
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x411fd070]
[    0.000000] Linux version 7.0.14 (rin4@rin) (gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0,
               GNU ld (GNU Binutils for Ubuntu) 2.46) #1 SMP PREEMPT_DYNAMIC
               Fri Oct 16 19:03:19 UTC 2026
[    0.000000] KASLR enabled
[    0.000000] Machine model: linux,dummy-virt
[    0.000000] psci: PSCIv1.1 detected in firmware.
[    0.000000] Detected PIPT I-cache on CPU0
[    0.000000] CPU features: detected: Spectre-v2
[    0.000000] CPU features: detected: Spectre-BHB
[    0.000000] CPU features: kernel page table isolation forced ON by KASLR
...
===================================
 Кастомное ядро загружено!
===================================

Доступные бинарники в /root:
-rwxrwxr-x 1 1000 1000 804560 bench_static
-rwxrwxr-x 1 1000 1000 716720 clone_thread_static

~ # uname -a
Linux (none) 7.0.14 #1 SMP PREEMPT_DYNAMIC Fri Oct 16 19:03:19 UTC 2026 aarch64 GNU/Linux
```

**Что подтверждает лог:**

- Загрузилось **именно собранное ядро** (`7.0.14` вместо стокового `7.0.0-38-generic`).
- `uname` внутри гостя подтверждает версию и дату сборки.
- Ядро стартует, монтирует `/proc` и `/sys`, запускает busybox shell через `rdinit=/init`.

### 3.4. Результаты bench внутри QEMU

```
~ # time /root/bench_static pthread 1000
real    0m 0.90s
user    0m 0.21s
sys     0m 0.64s

~ # time /root/bench_static clone   1000
real    0m 0.52s
user    0m 0.10s
sys     0m 0.38s

~ # time /root/bench_static fork    500
real    0m 0.74s
user    0m 0.21s
sys     0m 0.51s
```

### 3.5. Сравнение с хост-системой

| Режим | Хост (M3 + Ubuntu) | QEMU (кастомное ядро) | Во сколько раз медленнее |
|---|---|---|---|
| pthread 1000 | ~0.010 с | 0.90 с | **~90×** |
| clone 1000 | <0.005 с | 0.52 с | **~100×** |
| fork 1000 | ~0.018 с | ~1.48 с (экстрапол. с 500) | **~80×** |

**Почему такая разница:**

1. **QEMU использует TCG** (полная эмуляция ARM), а не KVM-ускорение. Каждая инструкция проходит через трансляцию.
2. В QEMU **1 vCPU** против 4 ядер на хосте.
3. Планировщик кастомного ядра работает в другой конфигурации.

**Что сохраняется:** относительный порядок **clone < pthread < fork** — как и предсказывает теория.

---

<a name="часть-4"></a>
## Часть 4. kprobe на kernel_clone

### 4.1. Исходный код clone_probe.c

```c
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sched.h>

static struct kprobe kp = {
    .symbol_name = "kernel_clone",
};

static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    pr_info("clone_probe: pid=%d tgid=%d comm=%s\n",
            current->pid, current->tgid, current->comm);
    return 0;
}

static int __init probe_init(void)
{
    kp.pre_handler = handler_pre;
    int ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("clone_probe: register_kprobe failed: %d\n", ret);
        return ret;
    }
    pr_info("clone_probe: registered at %pS\n", kp.addr);
    return 0;
}

static void __exit probe_exit(void)
{
    unregister_kprobe(&kp);
    pr_info("clone_probe: unregistered\n");
}

module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kprobe on kernel_clone()");
```

### 4.2. Makefile

```makefile
obj-m += clone_probe.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
```

Сборка: `make` — модуль собирается против заголовков **текущего** (хостового) ядра.

Загрузка:

```bash
sudo insmod clone_probe.ko
```

### 4.3. Логи dmesg для трёх режимов

**pthread 3** (`logs/part4_pthread.txt`):

```
[10646.677705] clone_probe: pid=112783 tgid=112783 comm=bench
[10646.677753] clone_probe: pid=112783 tgid=112783 comm=bench
[10646.677835] clone_probe: pid=112783 tgid=112783 comm=bench
```

**clone 3** (`logs/part4_clone.txt`):

```
[10646.693453] clone_probe: pid=112812 tgid=112812 comm=bench
[10646.693483] clone_probe: pid=112812 tgid=112812 comm=bench
[10646.693500] clone_probe: pid=112812 tgid=112812 comm=bench
```

**fork 3** (`logs/part4_fork.txt`):

```
[10646.706524] clone_probe: pid=112841 tgid=112841 comm=bench
[10646.706559] clone_probe: pid=112841 tgid=112841 comm=bench
[10646.706603] clone_probe: pid=112841 tgid=112841 comm=bench
```

### 4.4. Вывод

1. **kprobe перехватывает все три режима создания задач** — `pthread_create`, прямой `clone()` и `fork()` — потому что все они в ядре проходят через `kernel_clone()`.
2. **Ровно 3 срабатывания на каждый режим при N=3** — kprobe ловит каждую созданную задачу, ничего не пропускает.
3. **`pid == tgid` во всех строках** — потому что `handler_pre` срабатывает **до** создания потомка, когда `current` — это родитель (main-поток `bench`).
4. **`comm=bench`** подтверждает, что перехваты — наши.
5. Это **прямое подтверждение на уровне ядра** того, что измерялось в userspace через `perf stat` (Часть 2).

---

<a name="часть-5"></a>
## Часть 5. pthread-конвейер в minishell

### 5.1. Исходный код minishell.c

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#define MAX_ARGS 64
#define MAX_CMDS 16

typedef enum { MODE_FORK, MODE_PTHREAD } pipe_mode_t;
static pipe_mode_t pipe_mode = MODE_FORK;

/* ... парсинг (parse_pipeline, split_args) ... */

/* ---------- PTHREAD-режим ---------- */
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

        /* КРИТИЧНО: закрываем все унаследованные fd (3..1023),
           чтобы не держать чужие write-концы pipe и получать EOF */
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

    for (int i = 0; i < n - 1; i++)
        if (pipe(pipes[i]) < 0) { perror("pipe"); exit(1); }

    for (int i = 0; i < n; i++) {
        args[i].cmdline = cmds[i];
        args[i].in_fd   = (i == 0)     ? -1 : pipes[i - 1][0];
        args[i].out_fd  = (i == n - 1) ? -1 : pipes[i][1];
        pthread_create(&ths[i], NULL, pthread_stage, &args[i]);
    }

    for (int i = 0; i < n; i++)
        pthread_join(ths[i], NULL);

    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    free(pipes); free(ths); free(args);
}

/* ---------- FORK-режим ---------- */
static void run_pipeline_fork(char **cmds, int n)
{
    int prev_pipe = -1;
    for (int i = 0; i < n; i++) {
        int fds[2];
        if (i < n - 1) pipe(fds);

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

/* main — цикл shell с командой "set pipeline-mode fork|pthread" */
```

### 5.2. Результаты perf stat (N=100000, конвейер `seq | grep | wc`)

| Режим | task-clock, мс | context-switches | time elapsed, с |
|---|---|---|---|
| **fork** | 9.83 | 74 | 0.0060 |
| **pthread** | **6.02** | 78 | **0.0038** |

Логи: `logs/part5_fork.txt`, `logs/part5_pthread.txt`. Оба режима дали правильный результат `40951`.

### 5.3. Анализ

**Pthread-конвейер быстрее в ~1.6 раза** по task-clock (6.02 vs 9.83 мс) и по time elapsed (3.8 vs 6.0 мс).

**Почему:** в pthread-режиме три этапа конвейера стартуют параллельно, каждый в своём потоке. В fork-режиме главный процесс форкает этапы последовательно — сначала `seq`, потом `grep`, потом `wc`.

**Почему выигрыш не радикальный:** в нашей реализации каждый поток-этап всё равно делает `fork()` для запуска внешней команды. Если бы этапы были встроенными вычислениями (без exec), pthread-конвейер дал бы гораздо больший выигрыш — все три этапа работали бы параллельно в одном адресном пространстве.

**Ключевое техническое препятствие:** при `fork()` внутри потока дочерний процесс наследует все открытые fd, включая pipe-концы других потоков. Это блокирует EOF и приводит к deadlock. Решение — цикл `close(3..1023)` в ребёнке после `dup2`.

---

<a name="заключение"></a>
## Заключение

В ходе работы были изучены механизмы создания задач в ядре Linux и их различия на уровне userspace:

1. **`clone()` — универсальный системный вызов** для создания и процессов, и потоков. Поведение определяется флагами `CLONE_*`. `pthread_create()` — это обёртка над `clone()` с фиксированным набором флагов.

2. **Потоки vs процессы на уровне ядра** — разницы нет: и то, и другое `task_struct`. Разница — в разделяемых ресурсах (`mm_struct`, `files_struct`, `sighand_struct`) и общем TGID.

3. **Fork дороже** в CPU-времени (копирование `mm_struct`, COW, TLB), но его дети не видны `perf stat` без `-a`. Pthread/clone шарят память, поэтому создаются дешевле, но требуют синхронизации при работе с общими данными (cache line bouncing).

4. **В кастомном ядре** относительный порядок (clone < pthread < fork) сохраняется, но абсолютные времена в 50–100 раз больше из-за TCG-эмуляции и одного vCPU.

5. **kprobe на `kernel_clone()`** перехватывает все три режима создания задач — `pthread_create`, `clone`, `fork` — потому что все они в ядре проходят через одну точку входа. Число срабатываний точно совпадает с числом созданных задач.

6. **Pthread-конвейер** в minishell работает параллельно и быстрее fork-конвейера в 1.6 раза, но требует аккуратной работы с fd (`close(3..1023)` в ребёнке), иначе deadlock на pipe.

**Основной практический вывод:** fork/pthread/clone — это три способа создания задачи с разной стоимостью и разными разделяемыми ресурсами. Выбор зависит от задачи: изоляция (fork), общая память (pthread), или ручное управление (clone).

---

## Приложение. Структура репозитория

```
os-lab-threads/
├── README.md                       (этот файл)
├── clone_thread.c                  (Часть 1)
├── pthread_thread.c                (Часть 1)
├── bench.c                         (Часть 2)
├── minishell.c                     (Часть 5)
├── clone_probe/
│   ├── clone_probe.c               (Часть 4)
│   └── Makefile
└── logs/
    ├── part1_clone.txt
    ├── part1_pthread.txt
    ├── part2_perf_fork.txt
    ├── part2_perf_pthread.txt
    ├── part2_perf_clone.txt
    ├── part3_qemu.txt
    ├── part3_kernel_config.txt
    ├── part3_image.txt
    ├── part4_pthread.txt
    ├── part4_clone.txt
    ├── part4_fork.txt
    ├── part5_fork.txt
    └── part5_pthread.txt
```
