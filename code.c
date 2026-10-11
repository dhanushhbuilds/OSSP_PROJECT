#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

/* ============================================================
   LINUX PROCESS COMMAND CENTER
   A /proc based process monitoring and management system
   ============================================================ */

#define MAX_PROCESSES       8192
#define MAX_CPU_HISTORY    16384
#define NAME_SIZE           256
#define PATH_SIZE          4096
#define LINE_SIZE          2048

/* ANSI colors */
#define RESET   "\033[0m"
#define RED     "\033[1;31m"
#define GREEN   "\033[1;32m"
#define YELLOW  "\033[1;33m"
#define BLUE    "\033[1;34m"
#define CYAN    "\033[1;36m"
#define WHITE   "\033[1;37m"
#define MAGENTA "\033[1;35m"

/* ============================================================
   DATA STRUCTURES
   ============================================================ */

typedef struct
{
    int pid;
    int ppid;

    char name[NAME_SIZE];
    char state;

    long threads;
    long priority;
    long nice_value;

    unsigned long long utime;
    unsigned long long stime;
    unsigned long long total_cpu_time;

    unsigned long long vmrss;
    unsigned long long vmsize;

    double cpu_percent;
    double ram_percent;

} Process;

typedef struct
{
    int pid;
    unsigned long long cpu_time;
} CpuHistory;

/* ============================================================
   GLOBAL VARIABLES
   ============================================================ */

static CpuHistory cpu_history[MAX_CPU_HISTORY];
static int cpu_history_count = 0;

static unsigned long long previous_total_cpu = 0;

static volatile sig_atomic_t live_running = 1;

/* ============================================================
   UTILITY FUNCTIONS
   ============================================================ */

void clear_screen(void)
{
    printf("\033[2J\033[H");
}

void clear_input_buffer(void)
{
    int c;

    while ((c = getchar()) != '\n' && c != EOF)
    {
        /* discard input */
    }
}

void pause_screen(void)
{
    printf("\nPress ENTER to continue...");
    fflush(stdout);

    clear_input_buffer();
}

int is_pid_directory(const char *name)
{
    size_t i;

    if (name == NULL || *name == '\0')
        return 0;

    for (i = 0; name[i] != '\0'; i++)
    {
        if (!isdigit((unsigned char)name[i]))
            return 0;
    }

    return 1;
}

/* ============================================================
   CPU HISTORY
   ============================================================ */

unsigned long long get_previous_process_cpu(int pid)
{
    int i;

    for (i = 0; i < cpu_history_count; i++)
    {
        if (cpu_history[i].pid == pid)
            return cpu_history[i].cpu_time;
    }

    return 0;
}

void update_process_cpu_history(int pid, unsigned long long cpu_time)
{
    int i;

    for (i = 0; i < cpu_history_count; i++)
    {
        if (cpu_history[i].pid == pid)
        {
            cpu_history[i].cpu_time = cpu_time;
            return;
        }
    }

    if (cpu_history_count < MAX_CPU_HISTORY)
    {
        cpu_history[cpu_history_count].pid = pid;
        cpu_history[cpu_history_count].cpu_time = cpu_time;
        cpu_history_count++;
    }
}

/* ============================================================
   /proc/PID/stat
   ============================================================ */

/*
   Linux /proc/[pid]/stat fields:

   1  pid
   2  comm
   3  state
   4  ppid
   5  pgrp
   6  session
   7  tty_nr
   8  tpgid
   9  flags
   10 minflt
   11 cminflt
   12 majflt
   13 cmajflt
   14 utime
   15 stime
   16 cutime
   17 cstime
   18 priority
   19 nice
   20 num_threads
*/

int read_process_stat(int pid, Process *p)
{
    char path[PATH_SIZE];
    char line[LINE_SIZE];

    char *left_paren;
    char *right_paren;

    FILE *fp;

    int parsed_pid;
    int ppid;

    char state;

    unsigned long long utime = 0;
    unsigned long long stime = 0;

    long priority = 0;
    long nice_value = 0;
    long threads = 0;

    /*
       Fields 5-13:
       pgrp, session, tty_nr, tpgid, flags,
       minflt, cminflt, majflt, cmajflt
    */

    unsigned long long dummy_ull;

    /*
       Fields 16-17:
       cutime, cstime
    */
    long long dummy_ll_1;
    long long dummy_ll_2;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    fp = fopen(path, "r");

    if (fp == NULL)
        return 0;

    if (fgets(line, sizeof(line), fp) == NULL)
    {
        fclose(fp);
        return 0;
    }

    fclose(fp);

    /*
       Process names can contain ')' characters.
       Therefore use the LAST ')' before the state field.
    */
    left_paren = strchr(line, '(');
    right_paren = strrchr(line, ')');

    if (left_paren == NULL || right_paren == NULL)
        return 0;

    if (right_paren <= left_paren)
        return 0;

    /*
       Read PID and process name.
    */
    if (sscanf(
            line,
            "%d (%255[^)])",
            &parsed_pid,
            p->name
        ) != 2)
    {
        return 0;
    }

    /*
       After ") ":
       field 3 = state
       field 4 = ppid
       fields 5-13 skipped
       field 14 = utime
       field 15 = stime
       fields 16-17 skipped
       field 18 = priority
       field 19 = nice
       field 20 = threads
    */
    if (sscanf(
            right_paren + 2,
            "%c %d "
            "%*d %*d %*d %*d %*d %*u %*u %*u %*u "
            "%llu %llu "
            "%lld %lld "
            "%ld %ld %ld",
            &state,
            &ppid,
            &dummy_ull,
            &dummy_ull,
            &dummy_ll_1,
            &dummy_ll_2,
            &priority,
            &nice_value,
            &threads
        ) != 9)
    {
        return 0;
    }

    p->pid = parsed_pid;
    p->ppid = ppid;

    p->state = state;

    p->utime = utime;
    p->stime = stime;

    p->priority = priority;
    p->nice_value = nice_value;
    p->threads = threads;

    p->total_cpu_time = p->utime + p->stime;

    return 1;
}

/* ============================================================
   /proc/PID/status
   ============================================================ */

int read_process_status(int pid, Process *p)
{
    char path[PATH_SIZE];
    char line[LINE_SIZE];

    FILE *fp;

    p->vmrss = 0;
    p->vmsize = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    fp = fopen(path, "r");

    if (fp == NULL)
        return 0;

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        unsigned long long value;

        if (sscanf(line, "VmRSS: %llu kB", &value) == 1)
        {
            p->vmrss = value;
        }
        else if (sscanf(line, "VmSize: %llu kB", &value) == 1)
        {
            p->vmsize = value;
        }
    }

    fclose(fp);

    return 1;
}

/* ============================================================
   COMMAND LINE
   ============================================================ */

int get_process_cmdline(int pid, char *buffer, size_t size)
{
    char path[PATH_SIZE];

    FILE *fp;

    size_t len;

    if (buffer == NULL || size == 0)
        return 0;

    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    fp = fopen(path, "r");

    if (fp == NULL)
    {
        snprintf(buffer, size, "[unknown]");
        return 0;
    }

    len = fread(buffer, 1, size - 1, fp);

    fclose(fp);

    if (len == 0)
    {
        snprintf(buffer, size, "[kernel/empty]");
        return 0;
    }

    buffer[len] = '\0';

    /*
       /proc/PID/cmdline separates arguments with '\0'.
       Convert them into spaces for display.
    */
    for (size_t i = 0; i < len; i++)
    {
        if (buffer[i] == '\0')
            buffer[i] = ' ';
    }

    return 1;
}

/* ============================================================
   EXECUTABLE PATH
   ============================================================ */

int get_process_executable(int pid, char *buffer, size_t size)
{
    char path[PATH_SIZE];

    ssize_t len;

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);

    len = readlink(path, buffer, size - 1);

    if (len < 0)
    {
        snprintf(buffer, size, "[unavailable]");
        return 0;
    }

    buffer[len] = '\0';

    return 1;
}

/* ============================================================
   SYSTEM CPU
   ============================================================ */

unsigned long long get_total_cpu_time(void)
{
    FILE *fp;

    char line[LINE_SIZE];

    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;

    fp = fopen("/proc/stat", "r");

    if (fp == NULL)
        return 0;

    if (fgets(line, sizeof(line), fp) == NULL)
    {
        fclose(fp);
        return 0;
    }

    fclose(fp);

    user = 0;
    nice = 0;
    system = 0;
    idle = 0;
    iowait = 0;
    irq = 0;
    softirq = 0;
    steal = 0;

    if (sscanf(
            line,
            "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
            &user,
            &nice,
            &system,
            &idle,
            &iowait,
            &irq,
            &softirq,
            &steal
        ) < 4)
    {
        return 0;
    }

    return user + nice + system + idle +
           iowait + irq + softirq + steal;
}

/* ============================================================
   SYSTEM MEMORY
   ============================================================ */

void get_system_memory(
    unsigned long long *total_memory,
    unsigned long long *free_memory,
    unsigned long long *available_memory,
    unsigned long long *swap_total,
    unsigned long long *swap_free
)
{
    struct sysinfo info;

    if (sysinfo(&info) != 0)
    {
        *total_memory = 0;
        *free_memory = 0;
        *available_memory = 0;
        *swap_total = 0;
        *swap_free = 0;
        return;
    }

    *total_memory =
        (unsigned long long)info.totalram * info.mem_unit;

    *free_memory =
        (unsigned long long)info.freeram * info.mem_unit;

    *available_memory =
        (unsigned long long)info.freeram * info.mem_unit;

    *swap_total =
        (unsigned long long)info.totalswap * info.mem_unit;

    *swap_free =
        (unsigned long long)info.freeswap * info.mem_unit;
}

/* ============================================================
   CPU PERCENTAGE
   ============================================================ */

double calculate_cpu_percentage(
    unsigned long long process_previous,
    unsigned long long process_current,
    unsigned long long total_previous,
    unsigned long long total_current
)
{
    unsigned long long process_delta;
    unsigned long long total_delta;

    if (total_current <= total_previous)
        return 0.0;

    if (process_current < process_previous)
        return 0.0;

    process_delta = process_current - process_previous;
    total_delta = total_current - total_previous;

    if (total_delta == 0)
        return 0.0;

    /*
       Multiply by number of online CPUs so that:
       one fully busy CPU on a multicore machine ≈ 100%.
    */
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count < 1)
        cpu_count = 1;

    return ((double)process_delta / (double)total_delta)
           * 100.0
           * (double)cpu_count;
}

/* ============================================================
   PROCESS SCANNER
   ============================================================ */

int scan_processes(Process processes[], int *count)
{
    DIR *dir;

    struct dirent *entry;

    unsigned long long total_cpu;

    unsigned long long total_memory;
    unsigned long long free_memory;
    unsigned long long available_memory;
    unsigned long long swap_total;
    unsigned long long swap_free;

    int i;

    (void)free_memory;
    (void)swap_total;
    (void)swap_free;

    *count = 0;

    dir = opendir("/proc");

    if (dir == NULL)
    {
        perror("opendir /proc");
        return 0;
    }

    total_cpu = get_total_cpu_time();

    get_system_memory(
        &total_memory,
        &free_memory,
        &available_memory,
        &swap_total,
        &swap_free
    );

    /*
       First pass: collect processes.
    */
    while ((entry = readdir(dir)) != NULL)
    {
        int pid;

        Process p;

        if (!is_pid_directory(entry->d_name))
            continue;

        pid = atoi(entry->d_name);

        if (pid <= 0)
            continue;

        if (*count >= MAX_PROCESSES)
            break;

        memset(&p, 0, sizeof(p));

        if (!read_process_stat(pid, &p))
            continue;

        read_process_status(pid, &p);

        p.cpu_percent = calculate_cpu_percentage(
            get_previous_process_cpu(pid),
            p.total_cpu_time,
            previous_total_cpu,
            total_cpu
        );

        if (total_memory > 0)
        {
            p.ram_percent =
                ((double)p.vmrss * 1024.0 /
                 (double)total_memory) * 100.0;
        }
        else
        {
            p.ram_percent = 0.0;
        }

        processes[*count] = p;
        (*count)++;
    }

    closedir(dir);

    /*
       Second pass: update CPU history.
    */
    for (i = 0; i < *count; i++)
    {
        update_process_cpu_history(
            processes[i].pid,
            processes[i].total_cpu_time
        );
    }

    previous_total_cpu = total_cpu;

    return 1;
}

/* ============================================================
   BANNER
   ============================================================ */

void print_banner(void)
{
    printf(
        CYAN
        "============================================================\n"
        "              LINUX PROCESS COMMAND CENTER\n"
        "============================================================\n"
        RESET
    );

    printf(
        WHITE
        "        /proc Based Process Monitoring System\n"
        RESET
    );

    printf(
        "------------------------------------------------------------\n"
    );
}

/* ============================================================
   DASHBOARD
   ============================================================ */

void print_dashboard(Process processes[], int count)
{
    struct sysinfo info;

    struct utsname system_info;

    unsigned long long total_memory;
    unsigned long long free_memory;
    unsigned long long available_memory;
    unsigned long long swap_total;
    unsigned long long swap_free;

    unsigned long long used_memory;
    unsigned long long used_swap;

    double load1;
    double load5;
    double load15;

    int cpu_count;

    int i;

    int running = 0;
    int sleeping = 0;
    int stopped = 0;
    int zombie = 0;

    (void)processes;

    if (sysinfo(&info) != 0)
        return;

    uname(&system_info);

    get_system_memory(
        &total_memory,
        &free_memory,
        &available_memory,
        &swap_total,
        &swap_free
    );

    used_memory = total_memory - available_memory;

    if (swap_total >= swap_free)
        used_swap = swap_total - swap_free;
    else
        used_swap = 0;

    cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count < 1)
        cpu_count = 1;

    load1 =
        (double)info.loads[0] / 65536.0;

    load5 =
        (double)info.loads[1] / 65536.0;

    load15 =
        (double)info.loads[2] / 65536.0;

    for (i = 0; i < count; i++)
    {
        switch (processes[i].state)
        {
            case 'R':
                running++;
                break;

            case 'S':
            case 'D':
            case 'I':
                sleeping++;
                break;

            case 'T':
            case 't':
                stopped++;
                break;

            case 'Z':
                zombie++;
                break;

            default:
                break;
        }
    }

    printf(
        "\n"
        BLUE "SYSTEM DASHBOARD" RESET "\n"
        "------------------------------------------------------------\n"
    );

    printf(
        "Hostname       : %s\n"
        "Kernel         : %s\n"
        "Architecture   : %s\n"
        "CPU Cores      : %d\n"
        "Processes      : %d\n",
        system_info.nodename,
        system_info.release,
        system_info.machine,
        cpu_count,
        count
    );

    printf(
        "\n"
        "Process States  : "
        GREEN "Running=%d " RESET
        YELLOW "Sleeping=%d " RESET
        RED "Stopped=%d " RESET
        MAGENTA "Zombie=%d" RESET "\n",
        running,
        sleeping,
        stopped,
        zombie
    );

    printf(
        "\n"
        "RAM Used        : %.2f GB / %.2f GB (%.1f%%)\n",
        used_memory / (1024.0 * 1024.0 * 1024.0),
        total_memory / (1024.0 * 1024.0 * 1024.0),
        total_memory > 0
            ? ((double)used_memory / total_memory) * 100.0
            : 0.0
    );

    printf(
        "RAM Available   : %.2f GB\n",
        available_memory /
        (1024.0 * 1024.0 * 1024.0)
    );

    printf(
        "Swap Used       : %.2f GB / %.2f GB\n",
        used_swap /
        (1024.0 * 1024.0 * 1024.0),
        swap_total /
        (1024.0 * 1024.0 * 1024.0)
    );

    printf(
        "\n"
        "Load Average    : %.2f  %.2f  %.2f\n",
        load1,
        load5,
        load15
    );

    printf(
        "Uptime          : %ld seconds\n",
        info.uptime
    );

    printf(
        "------------------------------------------------------------\n"
    );
}

/* ============================================================
   PROCESS TABLE
   ============================================================ */

void display_processes(Process processes[], int count)
{
    int i;

    printf(
        "\n"
        CYAN
        "%-7s %-24s %-5s %-7s %-8s %-8s %-9s %-9s\n"
        RESET,
        "PID",
        "NAME",
        "STATE",
        "PPID",
        "THREADS",
        "RAM",
        "CPU%",
        "RSS"
    );

    printf(
        "--------------------------------------------------------------------------------\n"
    );

    for (i = 0; i < count; i++)
    {
        char ram_text[32];

        if (processes[i].vmrss >= 1024ULL * 1024ULL)
        {
            snprintf(
                ram_text,
                sizeof(ram_text),
                "%.2fG",
                processes[i].vmrss /
                (1024.0 * 1024.0)
            );
        }
        else if (processes[i].vmrss >= 1024ULL)
        {
            snprintf(
                ram_text,
                sizeof(ram_text),
                "%.1fM",
                processes[i].vmrss / 1024.0
            );
        }
        else
        {
            snprintf(
                ram_text,
                sizeof(ram_text),
                "%lluK",
                processes[i].vmrss
            );
        }

        printf(
            "%-7d %-24.24s %-5c %-7d %-8ld %-8.2f %-9.2f %-9s\n",
            processes[i].pid,
            processes[i].name,
            processes[i].state,
            processes[i].ppid,
            processes[i].threads,
            processes[i].ram_percent,
            processes[i].cpu_percent,
            ram_text
        );
    }

    printf(
        "\nTotal processes displayed: %d\n",
        count
    );
}

/* ============================================================
   PROCESS DETAILS
   ============================================================ */

void display_process_details(int pid)
{
    Process p;

    char cmdline[PATH_SIZE];
    char executable[PATH_SIZE];

    memset(&p, 0, sizeof(p));

    if (!read_process_stat(pid, &p))
    {
        printf(
            RED
            "\nProcess PID %d no longer exists or cannot be read.\n"
            RESET,
            pid
        );

        return;
    }

    read_process_status(pid, &p);
    get_process_cmdline(pid, cmdline, sizeof(cmdline));
    get_process_executable(pid, executable, sizeof(executable));

    printf(
        "\n"
        CYAN
        "================ PROCESS DETAILS ================\n"
        RESET
    );

    printf("PID             : %d\n", p.pid);
    printf("Name            : %s\n", p.name);
    printf("State           : %c\n", p.state);
    printf("Parent PID      : %d\n", p.ppid);
    printf("Threads         : %ld\n", p.threads);

    printf(
        "Priority        : %ld\n",
        p.priority
    );

    printf(
        "Nice Value      : %ld\n",
        p.nice_value
    );

    printf(
        "User CPU Time   : %llu jiffies\n",
        p.utime
    );

    printf(
        "System CPU Time : %llu jiffies\n",
        p.stime
    );

    printf(
        "Total CPU Time  : %llu jiffies\n",
        p.total_cpu_time
    );

    printf(
        "Resident RAM    : %llu KB\n",
        p.vmrss
    );

    printf(
        "Virtual Memory  : %llu KB\n",
        p.vmsize
    );

    printf(
        "Command Line    : %s\n",
        cmdline
    );

    printf(
        "Executable      : %s\n",
        executable
    );

    printf(
        "=================================================\n"
    );
}

/* ============================================================
   SEARCH
   ============================================================ */

void search_processes(Process processes[], int count)
{
    char query[256];

    int found = 0;

    int i;

    printf(
        "\nEnter PID or process name: "
    );

    if (scanf("%255s", query) != 1)
    {
        printf(
            RED
            "Invalid search input.\n"
            RESET
        );

        clear_input_buffer();

        return;
    }

    clear_input_buffer();

    printf(
        "\n"
        CYAN
        "SEARCH RESULTS\n"
        RESET
    );

    printf(
        "------------------------------------------------------------\n"
    );

    for (i = 0; i < count; i++)
    {
        char pid_text[32];

        snprintf(
            pid_text,
            sizeof(pid_text),
            "%d",
            processes[i].pid
        );

        if (
            strstr(processes[i].name, query) != NULL ||
            strstr(pid_text, query) != NULL
        )
        {
            printf(
                "PID=%d  NAME=%s  STATE=%c  CPU=%.2f%%  RAM=%.2f%%\n",
                processes[i].pid,
                processes[i].name,
                processes[i].state,
                processes[i].cpu_percent,
                processes[i].ram_percent
            );

            found++;
        }
    }

    if (!found)
    {
        printf(
            YELLOW
            "No matching processes found.\n"
            RESET
        );
    }
}

/* ============================================================
   SORTING
   ============================================================ */

int compare_pid(const void *a, const void *b)
{
    const Process *p1 = (const Process *)a;
    const Process *p2 = (const Process *)b;

    if (p1->pid < p2->pid)
        return -1;

    if (p1->pid > p2->pid)
        return 1;

    return 0;
}

int compare_name(const void *a, const void *b)
{
    const Process *p1 = (const Process *)a;
    const Process *p2 = (const Process *)b;

    return strcmp(p1->name, p2->name);
}

int compare_cpu(const void *a, const void *b)
{
    const Process *p1 = (const Process *)a;
    const Process *p2 = (const Process *)b;

    if (p1->cpu_percent < p2->cpu_percent)
        return 1;

    if (p1->cpu_percent > p2->cpu_percent)
        return -1;

    return 0;
}

int compare_ram(const void *a, const void *b)
{
    const Process *p1 = (const Process *)a;
    const Process *p2 = (const Process *)b;

    if (p1->vmrss < p2->vmrss)
        return 1;

    if (p1->vmrss > p2->vmrss)
        return -1;

    return 0;
}

void sort_processes(Process processes[], int count)
{
    int choice;

    printf(
        "\n"
        "1. Sort by PID\n"
        "2. Sort by Name\n"
        "3. Sort by CPU Usage\n"
        "4. Sort by RAM Usage\n"
        "\nEnter choice: "
    );

    if (scanf("%d", &choice) != 1)
    {
        printf(
            RED
            "Invalid input.\n"
            RESET
        );

        clear_input_buffer();

        return;
    }

    clear_input_buffer();

    switch (choice)
    {
        case 1:
            qsort(
                processes,
                (size_t)count,
                sizeof(Process),
                compare_pid
            );
            break;

        case 2:
            qsort(
                processes,
                (size_t)count,
                sizeof(Process),
                compare_name
            );
            break;

        case 3:
            qsort(
                processes,
                (size_t)count,
                sizeof(Process),
                compare_cpu
            );
            break;

        case 4:
            qsort(
                processes,
                (size_t)count,
                sizeof(Process),
                compare_ram
            );
            break;

        default:
            printf(
                RED
                "Invalid sorting option.\n"
                RESET
            );
            return;
    }

    printf(
        GREEN
        "Processes sorted successfully.\n"
        RESET
    );
}

/* ============================================================
   PROCESS TREE
   ============================================================ */

void print_tree_node(
    Process processes[],
    int count,
    int parent_pid,
    int level
)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (processes[i].ppid != parent_pid)
            continue;

        for (int j = 0; j < level; j++)
            printf("    ");

        printf(
            "|-- %d [%s] (%c)\n",
            processes[i].pid,
            processes[i].name,
            processes[i].state
        );

        if (level < 32)
        {
            print_tree_node(
                processes,
                count,
                processes[i].pid,
                level + 1
            );
        }
    }
}

void display_process_tree(Process processes[], int count)
{
    printf(
        "\n"
        CYAN
        "================ PROCESS TREE ================\n"
        RESET
    );

    print_tree_node(
        processes,
        count,
        1,
        0
    );

    printf(
        "================================================\n"
    );
}

/* ============================================================
   PROCESS CONTROL
   ============================================================ */

int confirm_action(const char *action, int pid)
{
    char answer[16];

    printf(
        YELLOW
        "\nAre you sure you want to %s PID %d? [y/N]: "
        RESET,
        action,
        pid
    );

    if (fgets(answer, sizeof(answer), stdin) == NULL)
        return 0;

    return answer[0] == 'y' || answer[0] == 'Y';
}

void process_control(void)
{
    int pid;
    int choice;
    int signal_number;

    printf(
        "\nEnter PID: "
    );

    if (scanf("%d", &pid) != 1)
    {
        printf(
            RED
            "Invalid PID.\n"
            RESET
        );

        clear_input_buffer();

        return;
    }

    clear_input_buffer();

    if (pid <= 0)
    {
        printf(
            RED
            "PID must be greater than zero.\n"
            RESET
        );

        return;
    }

    printf(
        "\n"
        "1. SIGTERM - Graceful termination\n"
        "2. SIGKILL - Force termination\n"
        "3. SIGSTOP - Stop process\n"
        "4. SIGCONT - Continue process\n"
        "\nEnter choice: "
    );

    if (scanf("%d", &choice) != 1)
    {
        printf(
            RED
            "Invalid choice.\n"
            RESET
        );

        clear_input_buffer();

        return;
    }

    clear_input_buffer();

    switch (choice)
    {
        case 1:
            signal_number = SIGTERM;

            if (!confirm_action("terminate", pid))
                return;

            break;

        case 2:
            signal_number = SIGKILL;

            if (!confirm_action("force kill", pid))
                return;

            break;

        case 3:
            signal_number = SIGSTOP;

            if (!confirm_action("stop", pid))
                return;

            break;

        case 4:
            signal_number = SIGCONT;

            if (!confirm_action("continue", pid))
                return;

            break;

        default:
            printf(
                RED
                "Invalid process-control option.\n"
                RESET
            );

            return;
    }

    if (kill((pid_t)pid, signal_number) == 0)
    {
        printf(
            GREEN
            "\nSignal sent successfully to PID %d.\n"
            RESET,
            pid
        );
    }
    else
    {
        printf(
            RED
            "\nUnable to send signal to PID %d: %s\n"
            RESET,
            pid,
            strerror(errno)
        );
    }
}

/* ============================================================
   SYSTEM INFORMATION
   ============================================================ */

void system_information(void)
{
    struct utsname info;

    struct sysinfo system;

    if (uname(&info) != 0)
    {
        perror("uname");
        return;
    }

    if (sysinfo(&system) != 0)
    {
        perror("sysinfo");
        return;
    }

    printf(
        "\n"
        CYAN
        "================ SYSTEM INFORMATION ================\n"
        RESET
    );

    printf(
        "Operating System : Linux\n"
    );

    printf(
        "Hostname         : %s\n",
        info.nodename
    );

    printf(
        "Kernel Release   : %s\n",
        info.release
    );

    printf(
        "Kernel Version   : %s\n",
        info.version
    );

    printf(
        "Machine          : %s\n",
        info.machine
    );

    printf(
        "CPU Cores        : %ld\n",
        sysconf(_SC_NPROCESSORS_ONLN)
    );

    printf(
        "Page Size        : %ld bytes\n",
        sysconf(_SC_PAGESIZE)
    );

    printf(
        "Uptime           : %ld seconds\n",
        system.uptime
    );

    printf(
        "Total RAM        : %.2f GB\n",
        ((double)system.totalram * system.mem_unit) /
        (1024.0 * 1024.0 * 1024.0)
    );

    printf(
        "Free RAM         : %.2f GB\n",
        ((double)system.freeram * system.mem_unit) /
        (1024.0 * 1024.0 * 1024.0)
    );

    printf(
        "Total Swap       : %.2f GB\n",
        ((double)system.totalswap * system.mem_unit) /
        (1024.0 * 1024.0 * 1024.0)
    );

    printf(
        "Free Swap        : %.2f GB\n",
        ((double)system.freeswap * system.mem_unit) /
        (1024.0 * 1024.0 * 1024.0)
    );

    printf(
        "=====================================================\n"
    );
}

/* ============================================================
   LIVE MONITOR
   ============================================================ */

void live_signal_handler(int signal_number)
{
    if (signal_number == SIGINT)
    {
        live_running = 0;
    }
}

void live_monitor(void)
{
    Process processes[MAX_PROCESSES];

    int count;

    void (*old_handler)(int);

    old_handler = signal(SIGINT, live_signal_handler);

    live_running = 1;

    /*
       Clear old CPU history so the first refresh doesn't
       show meaningless CPU percentages.
    */
    previous_total_cpu = 0;
    cpu_history_count = 0;

    while (live_running)
    {
        clear_screen();

        print_banner();

        if (!scan_processes(processes, &count))
        {
            printf(
                RED
                "\nUnable to scan /proc.\n"
                RESET
            );

            break;
        }

        print_dashboard(processes, count);

        /*
           Sort a copy-like array in-place by CPU.
           This is okay because live mode rescans every cycle.
        */
        qsort(
            processes,
            (size_t)count,
            sizeof(Process),
            compare_cpu
        );

        printf(
            "\n"
            BLUE
            "TOP CPU PROCESSES\n"
            RESET
        );

        printf(
            "------------------------------------------------------------\n"
        );

        int display_count =
            count < 15 ? count : 15;

        for (int i = 0; i < display_count; i++)
        {
            printf(
                "%-7d %-25.25s CPU=%7.2f%% RAM=%7.2f%% STATE=%c\n",
                processes[i].pid,
                processes[i].name,
                processes[i].cpu_percent,
                processes[i].ram_percent,
                processes[i].state
            );
        }

        printf(
            "\n"
            YELLOW
            "Live monitoring active. Press Ctrl+C to return to menu.\n"
            RESET
        );

        sleep(2);
    }

    signal(SIGINT, old_handler);

    printf(
        "\n"
        GREEN
        "Live monitor stopped.\n"
        RESET
    );

    sleep(1);
}

/* ============================================================
   MAIN MENU
   ============================================================ */

void main_menu(void)
{
    Process processes[MAX_PROCESSES];

    int count = 0;

    int choice;

    while (1)
    {
        clear_screen();

        print_banner();

        printf(
            "\n"
            "1. View Process Dashboard\n"
            "2. View Process Details\n"
            "3. Search Processes\n"
            "4. Sort Processes\n"
            "5. Process Tree\n"
            "6. Process Control\n"
            "7. System Information\n"
            "8. Live Monitor\n"
            "9. Exit\n"
            "\n"
            "Enter choice: "
        );

        if (scanf("%d", &choice) != 1)
        {
            printf(
                RED
                "\nInvalid input. Enter a number.\n"
                RESET
            );

            clear_input_buffer();
            sleep(1);

            continue;
        }

        clear_input_buffer();

        switch (choice)
        {
            case 1:

                clear_screen();

                print_banner();

                if (scan_processes(processes, &count))
                {
                    print_dashboard(
                        processes,
                        count
                    );

                    display_processes(
                        processes,
                        count
                    );
                }

                pause_screen();

                break;

            case 2:
            {
                int pid;

                printf(
                    "\nEnter PID: "
                );

                if (scanf("%d", &pid) != 1)
                {
                    printf(
                        RED
                        "Invalid PID.\n"
                        RESET
                    );

                    clear_input_buffer();

                    pause_screen();

                    break;
                }

                clear_input_buffer();

                display_process_details(pid);

                pause_screen();

                break;
            }

            case 3:

                if (scan_processes(processes, &count))
                {
                    search_processes(
                        processes,
                        count
                    );
                }

                pause_screen();

                break;

            case 4:

                if (scan_processes(processes, &count))
                {
                    sort_processes(
                        processes,
                        count
                    );

                    printf("\n");

                    display_processes(
                        processes,
                        count
                    );
                }

                pause_screen();

                break;

            case 5:

                if (scan_processes(processes, &count))
                {
                    display_process_tree(
                        processes,
                        count
                    );
                }

                pause_screen();

                break;

            case 6:

                process_control();

                pause_screen();

                break;

            case 7:

                system_information();

                pause_screen();

                break;

            case 8:

                live_monitor();

                break;

            case 9:

                printf(
                    GREEN
                    "\nExiting Linux Process Command Center...\n"
                    RESET
                );

                return;

            default:

                printf(
                    RED
                    "\nInvalid menu choice.\n"
                    RESET
                );

                sleep(1);

                break;
        }
    }
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void)
{
    main_menu();

    return 0;
}
