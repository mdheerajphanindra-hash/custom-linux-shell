# Custom Linux Shell with Command Execution and Job Control

A custom Linux shell developed using C (C11) and POSIX interfaces as part of an Operating Systems project. DheerajShell demonstrates how a shell reads user commands, creates processes, executes programs, manages jobs, and handles input/output operations in Ubuntu Linux.

## Features

- **Custom Shell Prompt:** Interactive command-line interface with a custom DheerajShell prompt.
- **Command Execution:** Executes external Linux commands using `fork()` and `execvp()`.
- **Process Management:** Monitors child processes using `waitpid()`.
- **Job Control:** Supports foreground and background execution.
- **Job Management:** Includes `jobs`, `fg`, and `bg` commands.
- **Signal Handling:** Handles signals such as SIGINT, SIGTSTP, and SIGCONT.
- **Input Redirection:** Reads command input from files using `<`.
- **Output Redirection:** Saves command output using `>`.
- **Append Redirection:** Appends output to files using `>>`.
- **Pipelines:** Connects multiple commands using pipes (`|`).
- **Process Groups:** Uses process groups for job and terminal management.
- **Exit Cleanup:** Performs cleanup of active jobs when the shell exits.

## Technology Stack

- Language: C (C11)
- Operating System: Ubuntu Linux
- Compiler: GCC
- Interfaces: POSIX
- Development Environment: VS Code and Oracle VirtualBox

## Project Structure

```text
custom-linux-shell/
│
├── shell.c
├── README.md
└── .gitignore
```

## Compilation and Execution

### 1. Clone the repository

```bash
git clone https://github.com/mdheerajphanindra-hash/custom-linux-shell.git
cd custom-linux-shell
```

### 2. Compile the program

```bash
gcc -Wall -Wextra -std=c11 shell.c -o myshell
```

### 3. Run the shell

```bash
./myshell
```

## Supported Commands

### Basic Linux Commands

```bash
pwd
ls
ls -l
date
whoami
mkdir test
```

### Job Control

```bash
sleep 60 &
jobs
fg 1
bg 1
```

### Input and Output Redirection

```bash
cat < input.txt
echo hello > output.txt
echo hello >> output.txt
```

### Pipelines

```bash
ls | wc -l
ls | grep ".c" | wc -l
echo hello | wc -c
```

## Operating Systems Concepts Demonstrated

| Concept | Implementation |
|---|---|
| Process Creation | `fork()` |
| Program Execution | `execvp()` |
| Process Monitoring | `waitpid()` |
| Process Groups | `setpgid()` |
| Terminal Management | `tcsetpgrp()` |
| Signal Handling | `sigaction()` |
| Inter-Process Communication | `pipe()` |
| I/O Redirection | `dup2()` |
| File Operations | `open()`, `close()` |

## Limitations

The current implementation does not support:

- Shell scripting and complete variable expansion.
- Wildcard expansion such as `*.c`.
- Command history and aliases.
- Logical operators such as `&&` and `||`.
- Standard error redirection using `2>`.
- Built-in commands with pipelines, redirection, or background execution.

## Testing

The shell was tested with normal Linux commands, invalid commands, input/output redirection, pipelines, foreground/background execution, signal handling, and job cleanup.

## Contributors

Developed jointly by:

- **M. Dheeraj Phanindra**
- **B. Srikanth Reddy**

**Academic Project:** Operating Systems  
**Institution:** SRM University–AP  
**Faculty Guide:** Dr. Awadhesh Dixit
