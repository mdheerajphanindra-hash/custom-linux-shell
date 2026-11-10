#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_TOKENS 128
#define MAX_ARGS 128
#define MAX_COMMANDS 16
#define MAX_JOBS 64
#define COMMAND_SIZE 512

typedef enum {
    PROC_RUNNING,
    PROC_STOPPED,
    PROC_DONE
} ProcessState;

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED
} JobState;

typedef struct {
    pid_t pid;
    ProcessState state;
} JobProcess;

typedef struct {
    int id;
    pid_t pgid;
    char command[COMMAND_SIZE];
    JobProcess processes[MAX_COMMANDS];
    int process_count;
    JobState state;
    int active;
    struct termios tmodes;
    int has_tmodes;
} Job;

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *input_file;
    char *output_file;
    int append;
} ParsedCommand;

static Job jobs[MAX_JOBS];
static int next_job_id = 1;
static int shell_interactive = 0;
static int shell_terminal = STDIN_FILENO;
static pid_t shell_pgid = -1;
static struct termios shell_tmodes;

static void set_signal(int sig, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, NULL) < 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

static void initialize_shell(void) {
    shell_interactive = isatty(shell_terminal);

    if (!shell_interactive) {
        shell_pgid = getpgrp();
    } else {
        while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp())) {
            if (kill(-shell_pgid, SIGTTIN) < 0) {
                perror("kill SIGTTIN");
                exit(EXIT_FAILURE);
            }
        }

        shell_pgid = getpid();
        if (setpgid(shell_pgid, shell_pgid) < 0 &&
            errno != EACCES && errno != EPERM) {
            perror("setpgid shell");
            exit(EXIT_FAILURE);
        }

        shell_pgid = getpgrp();
        if (tcsetpgrp(shell_terminal, shell_pgid) < 0) {
            perror("tcsetpgrp shell");
            exit(EXIT_FAILURE);
        }
        if (tcgetattr(shell_terminal, &shell_tmodes) < 0) {
            perror("tcgetattr");
            exit(EXIT_FAILURE);
        }
    }

    /* The shell must not be killed or stopped by terminal key signals. */
    set_signal(SIGINT, SIG_IGN);
    set_signal(SIGQUIT, SIG_IGN);
    set_signal(SIGTSTP, SIG_IGN);
    set_signal(SIGTTIN, SIG_IGN);
    set_signal(SIGTTOU, SIG_IGN);
}

static void restore_child_signals(void) {
    set_signal(SIGINT, SIG_DFL);
    set_signal(SIGQUIT, SIG_DFL);
    set_signal(SIGTSTP, SIG_DFL);
    set_signal(SIGTTIN, SIG_DFL);
    set_signal(SIGTTOU, SIG_DFL);
    set_signal(SIGCHLD, SIG_DFL);
}

static void display_prompt(void) {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("DheerajShell:%s$ ", cwd);
    } else {
        printf("DheerajShell$ ");
    }
    fflush(stdout);
}

static Job *find_free_job(void) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (!jobs[i].active) {
            return &jobs[i];
        }
    }
    return NULL;
}

static Job *find_job_by_id(int id) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].active && jobs[i].id == id) {
            return &jobs[i];
        }
    }
    return NULL;
}

static Job *add_job(const pid_t pids[], int process_count,
                    pid_t pgid, const char *command) {
    Job *job = find_free_job();
    if (job == NULL) {
        return NULL;
    }

    memset(job, 0, sizeof(*job));
    job->id = next_job_id++;
    job->pgid = pgid;
    job->process_count = process_count;
    job->state = JOB_RUNNING;
    job->active = 1;
    snprintf(job->command, sizeof(job->command), "%s", command);

    for (int i = 0; i < process_count; ++i) {
        job->processes[i].pid = pids[i];
        job->processes[i].state = PROC_RUNNING;
    }
    return job;
}

static void remove_job(Job *job) {
    job->active = 0;
}

static void refresh_job_state(Job *job) {
    int live = 0;
    int stopped = 0;

    for (int i = 0; i < job->process_count; ++i) {
        if (job->processes[i].state != PROC_DONE) {
            ++live;
            if (job->processes[i].state == PROC_STOPPED) {
                ++stopped;
            }
        }
    }

    if (live > 0 && stopped == live) {
        job->state = JOB_STOPPED;
    } else {
        job->state = JOB_RUNNING;
    }
}

static int job_is_done(const Job *job) {
    for (int i = 0; i < job->process_count; ++i) {
        if (job->processes[i].state != PROC_DONE) {
            return 0;
        }
    }
    return 1;
}

static int job_is_stopped(const Job *job) {
    int live = 0;
    int stopped = 0;
    for (int i = 0; i < job->process_count; ++i) {
        if (job->processes[i].state != PROC_DONE) {
            ++live;
            if (job->processes[i].state == PROC_STOPPED) {
                ++stopped;
            }
        }
    }
    return live > 0 && live == stopped;
}

static void build_tokens_string(char *tokens[], int count,
                                char *buffer, size_t size,
                                int background) {
    size_t used = 0;
    buffer[0] = '\0';

    for (int i = 0; i < count; ++i) {
        int written = snprintf(buffer + used, size - used,
                               "%s%s", i == 0 ? "" : " ", tokens[i]);
        if (written < 0 || (size_t)written >= size - used) {
            buffer[size - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }

    if (background && used < size - 1) {
        snprintf(buffer + used, size - used, " &");
    }
}

static int is_operator(const char *token) {
    return token != NULL &&
           (strcmp(token, "|") == 0 || strcmp(token, "<") == 0 ||
            strcmp(token, ">") == 0 || strcmp(token, ">>") == 0 ||
            strcmp(token, "&") == 0);
}

static void free_tokens(char *tokens[], int count) {
    for (int i = 0; i < count; ++i) {
        free(tokens[i]);
    }
}

/* Split operators even when adjacent to words; remove matching quotes. */
static int tokenize_line(const char *line, char *tokens[], int max_tokens) {
    const char *p = line;
    int count = 0;

    while (*p != '\0') {
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        if (count >= max_tokens) {
            fprintf(stderr, "Too many command tokens (maximum %d).\n", max_tokens);
            free_tokens(tokens, count);
            return -1;
        }

        if (*p == '|' || *p == '<' || *p == '>' || *p == '&') {
            size_t length = (*p == '>' && p[1] == '>') ? 2U : 1U;
            tokens[count] = malloc(length + 1U);
            if (tokens[count] == NULL) {
                perror("malloc");
                free_tokens(tokens, count);
                return -1;
            }
            memcpy(tokens[count], p, length);
            tokens[count][length] = '\0';
            ++count;
            p += length;
            continue;
        }

        size_t remaining = strlen(p);
        char *word = malloc(remaining + 1U);
        if (word == NULL) {
            perror("malloc");
            free_tokens(tokens, count);
            return -1;
        }

        size_t used = 0;
        int saw_character = 0;

        while (*p != '\0' && !isspace((unsigned char)*p) &&
               *p != '|' && *p != '<' && *p != '>' && *p != '&') {
            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                saw_character = 1;
                while (*p != '\0' && *p != quote) {
                    if (*p == '\\' && quote == '"' && p[1] != '\0') {
                        ++p;
                    }
                    word[used++] = *p++;
                }
                if (*p != quote) {
                    fprintf(stderr, "Syntax error: unmatched quote.\n");
                    free(word);
                    free_tokens(tokens, count);
                    return -1;
                }
                ++p;
            } else if (*p == '\\' && p[1] != '\0') {
                ++p;
                word[used++] = *p++;
                saw_character = 1;
            } else {
                word[used++] = *p++;
                saw_character = 1;
            }
        }

        word[used] = '\0';
        if (!saw_character) {
            free(word);
            continue;
        }
        tokens[count++] = word;
    }

    return count;
}

static int parse_pipeline(char *tokens[], int token_count,
                          ParsedCommand commands[], int *command_count) {
    memset(commands, 0, sizeof(ParsedCommand) * MAX_COMMANDS);
    *command_count = 1;
    ParsedCommand *current = &commands[0];

    for (int i = 0; i < token_count; ++i) {
        char *token = tokens[i];

        if (strcmp(token, "|") == 0) {
            if (current->argc == 0) {
                fprintf(stderr, "Syntax error: missing command before '|'.\n");
                return -1;
            }
            if (*command_count >= MAX_COMMANDS) {
                fprintf(stderr, "Maximum pipeline length is %d commands.\n", MAX_COMMANDS);
                return -1;
            }
            current->argv[current->argc] = NULL;
            current = &commands[(*command_count)++];
            continue;
        }

        if (strcmp(token, "<") == 0 || strcmp(token, ">") == 0 ||
            strcmp(token, ">>") == 0) {
            int is_input = strcmp(token, "<") == 0;
            if (i + 1 >= token_count || is_operator(tokens[i + 1])) {
                fprintf(stderr, "Syntax error: missing redirection filename.\n");
                return -1;
            }

            if (is_input) {
                if (current->input_file != NULL) {
                    fprintf(stderr, "Syntax error: duplicate input redirection.\n");
                    return -1;
                }
                current->input_file = tokens[++i];
            } else {
                if (current->output_file != NULL) {
                    fprintf(stderr, "Syntax error: duplicate output redirection.\n");
                    return -1;
                }
                current->append = strcmp(token, ">>") == 0;
                current->output_file = tokens[++i];
            }
            continue;
        }

        if (strcmp(token, "&") == 0) {
            fprintf(stderr, "Syntax error: '&' must be at the end of the command.\n");
            return -1;
        }

        if (current->argc >= MAX_ARGS - 1) {
            fprintf(stderr, "Too many arguments in a command.\n");
            return -1;
        }
        current->argv[current->argc++] = token;
    }

    for (int i = 0; i < *command_count; ++i) {
        if (commands[i].argc == 0) {
            fprintf(stderr, "Syntax error: missing command in pipeline.\n");
            return -1;
        }
        commands[i].argv[commands[i].argc] = NULL;
    }

    return 0;
}

static void close_pipes(int pipes[][2], int pipe_count) {
    for (int i = 0; i < pipe_count; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

static int apply_redirections(const ParsedCommand *command) {
    if (command->input_file != NULL) {
        int fd = open(command->input_file, O_RDONLY);
        if (fd < 0) {
            perror(command->input_file);
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("dup2 input");
            close(fd);
            return -1;
        }
        close(fd);
    }

    if (command->output_file != NULL) {
        int flags = O_WRONLY | O_CREAT;
        flags |= command->append ? O_APPEND : O_TRUNC;
        int fd = open(command->output_file, flags, 0644);
        if (fd < 0) {
            perror(command->output_file);
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("dup2 output");
            close(fd);
            return -1;
        }
        close(fd);
    }
    return 0;
}

static void give_terminal_to_shell(void) {
    if (!shell_interactive) {
        return;
    }
    if (tcsetpgrp(shell_terminal, shell_pgid) < 0) {
        perror("tcsetpgrp shell");
    }
    if (tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes) < 0) {
        perror("tcsetattr shell");
    }
}

static void wait_for_foreground_job(Job *job) {
    int status;
    int terminating_signal = 0;

    while (!job_is_done(job) && !job_is_stopped(job)) {
        pid_t result = waitpid(-job->pgid, &status, WUNTRACED);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                for (int i = 0; i < job->process_count; ++i) {
                    if (job->processes[i].state != PROC_DONE) {
                        job->processes[i].state = PROC_DONE;
                    }
                }
                break;
            }
            perror("waitpid foreground");
            break;
        }

        for (int i = 0; i < job->process_count; ++i) {
            if (job->processes[i].pid == result) {
                if (WIFSTOPPED(status)) {
                    job->processes[i].state = PROC_STOPPED;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    job->processes[i].state = PROC_DONE;
                    if (WIFSIGNALED(status)) {
                        terminating_signal = WTERMSIG(status);
                    }
                }
                break;
            }
        }
        refresh_job_state(job);
    }

    if (job_is_stopped(job)) {
        job->state = JOB_STOPPED;
        if (shell_interactive && tcgetattr(shell_terminal, &job->tmodes) == 0) {
            job->has_tmodes = 1;
        }
        printf("\n[%d] Stopped %s\n", job->id, job->command);
    } else if (job_is_done(job)) {
        if (terminating_signal != 0) {
            printf("\n[%d] Terminated by signal %d\n",
                   job->id, terminating_signal);
        }
        remove_job(job);
    }
}

static void foreground_job(Job *job, int continue_job) {
    if (shell_interactive && tcsetpgrp(shell_terminal, job->pgid) < 0) {
        perror("tcsetpgrp foreground");
        return;
    }

    if (continue_job && job->state == JOB_STOPPED) {
        if (shell_interactive && job->has_tmodes) {
            if (tcsetattr(shell_terminal, TCSADRAIN, &job->tmodes) < 0) {
                perror("tcsetattr job");
            }
        }

        if (kill(-job->pgid, SIGCONT) < 0) {
            perror("SIGCONT");
            give_terminal_to_shell();
            return;
        }

        for (int i = 0; i < job->process_count; ++i) {
            if (job->processes[i].state == PROC_STOPPED) {
                job->processes[i].state = PROC_RUNNING;
            }
        }
        job->state = JOB_RUNNING;
    }

    wait_for_foreground_job(job);
    give_terminal_to_shell();
}

static void background_job(Job *job) {
    if (job->state == JOB_STOPPED) {
        if (kill(-job->pgid, SIGCONT) < 0) {
            perror("SIGCONT");
            return;
        }
        for (int i = 0; i < job->process_count; ++i) {
            if (job->processes[i].state == PROC_STOPPED) {
                job->processes[i].state = PROC_RUNNING;
            }
        }
        job->state = JOB_RUNNING;
        printf("[%d] Running %s\n", job->id, job->command);
    } else {
        printf("[%d] Already running: %s\n", job->id, job->command);
    }
}

static void update_job_statuses(void) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        Job *job = &jobs[i];
        if (!job->active) {
            continue;
        }

        JobState old_state = job->state;

        for (int j = 0; j < job->process_count; ++j) {
            JobProcess *process = &job->processes[j];
            if (process->state == PROC_DONE) {
                continue;
            }

            while (1) {
                int status;
                pid_t result = waitpid(process->pid, &status,
                                       WNOHANG | WUNTRACED | WCONTINUED);
                if (result == 0) {
                    break;
                }
                if (result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == ECHILD) {
                        process->state = PROC_DONE;
                    } else {
                        perror("waitpid background");
                    }
                    break;
                }

                if (WIFSTOPPED(status)) {
                    process->state = PROC_STOPPED;
                } else if (WIFCONTINUED(status)) {
                    process->state = PROC_RUNNING;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    process->state = PROC_DONE;
                }
            }
        }

        if (job_is_done(job)) {
            printf("\n[%d] Done %s\n", job->id, job->command);
            remove_job(job);
            continue;
        }

        refresh_job_state(job);
        if (old_state != JOB_STOPPED && job->state == JOB_STOPPED) {
            printf("\n[%d] Stopped %s\n", job->id, job->command);
        } else if (old_state == JOB_STOPPED && job->state == JOB_RUNNING) {
            printf("\n[%d] Running %s\n", job->id, job->command);
        }
    }
}

static void list_jobs(void) {
    update_job_statuses();
    int found = 0;
    printf("\n========== JOB LIST ==========\n");

    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].active) {
            printf("[%d] %-8s %s (PGID: %d)\n",
                   jobs[i].id,
                   jobs[i].state == JOB_RUNNING ? "Running" : "Stopped",
                   jobs[i].command,
                   jobs[i].pgid);
            found = 1;
        }
    }

    if (!found) {
        printf("No active jobs.\n");
    }
    printf("==============================\n\n");
}

static int parse_job_id(const char *argument) {
    if (argument == NULL || *argument == '\0') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    long value = strtol(argument, &end, 10);
    if (errno != 0 || end == argument || *end != '\0' ||
        value <= 0 || value > 2147483647L) {
        return -1;
    }
    return (int)value;
}

static void cleanup_jobs(void) {
    update_job_statuses();

    for (int i = 0; i < MAX_JOBS; ++i) {
        Job *job = &jobs[i];
        if (!job->active) {
            continue;
        }

        (void)kill(-job->pgid, SIGHUP);
        (void)kill(-job->pgid, SIGCONT);
        (void)kill(-job->pgid, SIGTERM);

        /* Reap every child belonging to this pipeline. */
        for (int j = 0; j < job->process_count; ++j) {
            pid_t pid = job->processes[j].pid;
            if (job->processes[j].state == PROC_DONE) {
                continue;
            }
            while (waitpid(pid, NULL, 0) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
        }
        remove_job(job);
    }
}

static void show_help(void) {
    printf("\n======= DHEERAJ CUSTOM LINUX SHELL =======\n");
    printf("Basic commands:\n");
    printf("  cd <path>       Change directory\n");
    printf("  help            Display help\n");
    printf("  jobs            Display jobs\n");
    printf("  fg <job_id>     Bring job to foreground\n");
    printf("  bg <job_id>     Resume job in background\n");
    printf("  exit            Exit shell\n");
    printf("\nCommand features:\n");
    printf("  command &       Run in background\n");
    printf("  command > file  Redirect output\n");
    printf("  command >> file Append output\n");
    printf("  command < file  Redirect input\n");
    printf("  cmd1 | cmd2     Pipe commands\n");
    printf("  Ctrl+C          Terminate foreground job\n");
    printf("  Ctrl+Z          Suspend foreground job\n");
    printf("\nUse spaces around |, <, >, >> and &.\n");
    printf("Built-ins (cd, jobs, fg, bg, help, exit) must be used alone.\n");
    printf("==========================================\n\n");
}

static int is_builtin_command(const char *command) {
    return strcmp(command, "cd") == 0 ||
           strcmp(command, "help") == 0 ||
           strcmp(command, "jobs") == 0 ||
           strcmp(command, "fg") == 0 ||
           strcmp(command, "bg") == 0 ||
           strcmp(command, "exit") == 0;
}

static int execute_builtin(char *args[]) {
    if (strcmp(args[0], "exit") == 0) {
        cleanup_jobs();
        printf("Exiting Dheeraj Custom Linux Shell...\n");
        exit(EXIT_SUCCESS);
    }

    if (strcmp(args[0], "help") == 0) {
        show_help();
        return 1;
    }

    if (strcmp(args[0], "cd") == 0) {
        const char *path = args[1] != NULL ? args[1] : getenv("HOME");
        if (path == NULL || chdir(path) < 0) {
            perror("cd");
        }
        return 1;
    }

    if (strcmp(args[0], "jobs") == 0) {
        list_jobs();
        return 1;
    }

    if (strcmp(args[0], "fg") == 0 || strcmp(args[0], "bg") == 0) {
        update_job_statuses();
        int id = parse_job_id(args[1]);
        if (id < 0) {
            printf("Usage: %s <job_id>\n", args[0]);
            return 1;
        }

        Job *job = find_job_by_id(id);
        if (job == NULL) {
            printf("Job %d not found.\n", id);
            return 1;
        }

        if (strcmp(args[0], "fg") == 0) {
            foreground_job(job, 1);
        } else {
            background_job(job);
        }
        return 1;
    }

    return 0;
}

static int create_pipes(int pipes[][2], int pipe_count) {
    for (int i = 0; i < pipe_count; ++i) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (int j = 0; j < i; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return -1;
        }
    }
    return 0;
}

static void terminate_started_children(pid_t pids[], int count, pid_t pgid) {
    if (pgid > 0) {
        (void)kill(-pgid, SIGTERM);
    }
    for (int i = 0; i < count; ++i) {
        (void)kill(pids[i], SIGTERM);
    }
    for (int i = 0; i < count; ++i) {
        while (waitpid(pids[i], NULL, 0) < 0 && errno == EINTR) {
            /* Retry interrupted wait. */
        }
    }
}

static void execute_pipeline(ParsedCommand commands[], int command_count,
                             int background, const char *description) {
    int pipes[MAX_COMMANDS - 1][2];
    int pipe_count = command_count - 1;
    int gate[2] = {-1, -1};
    pid_t pids[MAX_COMMANDS];
    int started = 0;
    pid_t pgid = -1;

    if (find_free_job() == NULL) {
        fprintf(stderr, "Maximum job limit reached.\n");
        return;
    }

    if (pipe_count > 0 && create_pipes(pipes, pipe_count) < 0) {
        return;
    }

    /* Gate keeps children from exec'ing until the parent sets up the job. */
    if (pipe(gate) < 0) {
        perror("pipe start gate");
        if (pipe_count > 0) {
            close_pipes(pipes, pipe_count);
        }
        return;
    }

    for (int i = 0; i < command_count; ++i) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            close(gate[0]);
            close(gate[1]);
            if (pipe_count > 0) {
                close_pipes(pipes, pipe_count);
            }
            terminate_started_children(pids, started, pgid);
            return;
        }

        if (pid == 0) {
            close(gate[1]);

            if (i == 0) {
                if (setpgid(0, 0) < 0) {
                    perror("setpgid child");
                    _exit(126);
                }
            } else if (setpgid(0, pgid) < 0) {
                perror("setpgid child");
                _exit(126);
            }

            restore_child_signals();

            /* Connect previous pipeline output to this command's input. */
            if (i > 0 && dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                perror("dup2 pipe input");
                _exit(126);
            }

            /* Connect this command's output to the next command's input. */
            if (i < command_count - 1 &&
                dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                perror("dup2 pipe output");
                _exit(126);
            }

            if (pipe_count > 0) {
                close_pipes(pipes, pipe_count);
            }

            /* Explicit redirections override pipeline defaults. */
            if (apply_redirections(&commands[i]) < 0) {
                _exit(126);
            }

            /* Wait until parent closes the gate's write end. */
            char gate_byte;
            ssize_t gate_result;
            do {
                gate_result = read(gate[0], &gate_byte, 1);
            } while (gate_result < 0 && errno == EINTR);
            close(gate[0]);

            if (gate_result < 0) {
                perror("read start gate");
                _exit(126);
            }

            execvp(commands[i].argv[0], commands[i].argv);
            perror(commands[i].argv[0]);
            _exit(127);
        }

        pids[started++] = pid;
        if (i == 0) {
            pgid = pid;
        }

        if (setpgid(pid, pgid) < 0 && errno != EACCES && errno != ESRCH) {
            perror("setpgid parent");
        }
    }

    /* Parent no longer needs its copies of pipeline descriptors. */
    if (pipe_count > 0) {
        close_pipes(pipes, pipe_count);
    }
    close(gate[0]);

    Job *job = add_job(pids, started, pgid, description);
    if (job == NULL) {
        close(gate[1]);
        terminate_started_children(pids, started, pgid);
        fprintf(stderr, "Unable to create job.\n");
        return;
    }

    if (!background && shell_interactive) {
        if (tcsetpgrp(shell_terminal, pgid) < 0) {
            perror("tcsetpgrp foreground");
        }
    }

    /* EOF on the gate releases all child processes to execute. */
    close(gate[1]);

    if (background) {
        printf("[%d] %d\n", job->id, job->pgid);
    } else {
        wait_for_foreground_job(job);
        give_terminal_to_shell();
    }
}

int main(void) {
    char *line = NULL;
    size_t capacity = 0;

    initialize_shell();

    printf("\n========================================\n");
    printf("       DHEERAJ CUSTOM LINUX SHELL\n");
    printf("========================================\n");
    printf("Type 'help' to see available commands.\n");
    printf("Type 'exit' to quit.\n\n");

    while (1) {
        update_job_statuses();
        display_prompt();

        ssize_t length = getline(&line, &capacity, stdin);
        if (length < 0) {
            if (feof(stdin)) {
                printf("\nExiting shell...\n");
            } else {
                perror("getline");
            }
            break;
        }

        char *tokens[MAX_TOKENS];
        int allocated_tokens = tokenize_line(line, tokens, MAX_TOKENS);
        if (allocated_tokens < 0) {
            continue;
        }
        int token_count = allocated_tokens;
        if (token_count == 0) {
            free_tokens(tokens, allocated_tokens);
            continue;
        }

        int background = 0;
        if (strcmp(tokens[token_count - 1], "&") == 0) {
            background = 1;
            --token_count;
        }
        if (token_count == 0) {
            fprintf(stderr, "Syntax error: missing command before '&'.\n");
            free_tokens(tokens, allocated_tokens);
            continue;
        }

        char description[COMMAND_SIZE];
        build_tokens_string(tokens, token_count, description,
                            sizeof(description), background);

        ParsedCommand commands[MAX_COMMANDS];
        int command_count = 0;
        if (parse_pipeline(tokens, token_count, commands, &command_count) < 0) {
            free_tokens(tokens, allocated_tokens);
            continue;
        }

        /* Built-ins must run in the shell process and are supported standalone. */
        if (command_count == 1 && is_builtin_command(commands[0].argv[0])) {
            if (background || commands[0].input_file != NULL ||
                commands[0].output_file != NULL) {
                fprintf(stderr,
                        "Built-in commands do not support pipes, redirection, or '&' yet.\n");
                free_tokens(tokens, allocated_tokens);
                continue;
            }
            execute_builtin(commands[0].argv);
            free_tokens(tokens, allocated_tokens);
            continue;
        }

        /* Do not silently run built-ins in a child process or pipeline. */
        int invalid_builtin = 0;
        for (int i = 0; i < command_count; ++i) {
            if (is_builtin_command(commands[i].argv[0])) {
                fprintf(stderr,
                        "Built-in '%s' must be used as a standalone command.\n",
                        commands[i].argv[0]);
                invalid_builtin = 1;
                break;
            }
        }
        if (invalid_builtin) {
            free_tokens(tokens, allocated_tokens);
            continue;
        }

        execute_pipeline(commands, command_count, background, description);
        free_tokens(tokens, allocated_tokens);
    }

    cleanup_jobs();
    free(line);
    return 0;
}
