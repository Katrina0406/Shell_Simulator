/**
 * @file tsh.c
 * @brief A tiny shell program with job control
 *
 * - eval function is the core function that evalutates all commands
 * - all signals are blocked before fork() child process and unblocked
 * before exceve or a parent process finishes
 * - parent should add job first and wait for child to finish (and delete job).
 * This is done by sigsuspend.
 * - A job list is maintained throughout the process. Its access requires all
 * signals to be blocked.
 * - built-in commands:
 *  - The quit command terminates the shell.
 *  - The jobs command lists all background jobs.
 *  - The bg job command resumes job by sending it a SIGCONT signal, and then runs it in the background. The job argument can be either a PID or a JID.
 *  - The fg job command resumes job by sending it a SIGCONT signal, and then runs it in the foreground. The job argument can be either a PID or a JID.
 *
 * - sigchld handler is the main handler. It deals with actions after receiving other 
 * signals (SIGINT, SIGTSTP, SIGCONT)
 * - I/O redirection gives only write permission to the owner for output redirection
 * 
 * @author Yuqiao Hu <yuqiaohu@andrew.cmu.edu>
 */

#include "csapp.h"
#include "tsh_helper.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#define MODE S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH
#endif

/* Function prototypes */
void eval(const char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);
void cleanup(void);

/**
 * @brief the main routine for a shell program
 *
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE_TSH]; // Cmdline for fgets
    bool emit_prompt = true;   // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        perror("dup2 error");
        exit(1);
    }

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h': // Prints help message
            usage();
            break;
        case 'v': // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p': // Disables prompt printing
            emit_prompt = false;
            break;
        default:
            usage();
        }
    }

    // Create environment variable
    if (putenv("MY_ENV=42") < 0) {
        perror("putenv error");
        exit(1);
    }

    // Set buffering mode of stdout to line buffering.
    // This prevents lines from being printed in the wrong order.
    if (setvbuf(stdout, NULL, _IOLBF, 0) < 0) {
        perror("setvbuf error");
        exit(1);
    }

    // Initialize the job list
    init_job_list();

    // Register a function to clean up the job list on program termination.
    // The function may not run in the case of abnormal termination (e.g. when
    // using exit or terminating due to a signal handler), so in those cases,
    // we trust that the OS will clean up any remaining resources.
    if (atexit(cleanup) < 0) {
        perror("atexit error");
        exit(1);
    }

    // Install the signal handlers
    Signal(SIGINT, sigint_handler);   // Handles Ctrl-C
    Signal(SIGTSTP, sigtstp_handler); // Handles Ctrl-Z
    Signal(SIGCHLD, sigchld_handler); // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler);

    // Execute the shell's read/eval loop
    while (true) {
        if (emit_prompt) {
            printf("%s", prompt);

            // We must flush stdout since we are not printing a full line.
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin)) {
            perror("fgets error");
            exit(1);
        }

        if (feof(stdin)) {
            // End of file (Ctrl-D)
            printf("\n");
            return 0;
        }

        // Remove any trailing newline
        char *newline = strchr(cmdline, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        // Evaluate the command line
        eval(cmdline);
    }

    return -1; // control never reaches here
}

/**
 * @brief Main routine that parses, interprets, and executes the command line.
 *
 * calls the builtin_command function and checks
 * if first cmd-line arg is a built-in shell command -> interprets & returns 1
 * if not -> returns 0
 *
 * if returns 0 -> shell creates a child process & exec program inside child
 * if user asked to run in bg, shell returns to top of loop & wait for next
 * cmd line
 * otherwise, shell uses waitpid func to wait for job to terminate & goes on
 * yo next iteration
 *
 * NOTE: The shell is supposed to be a long-running process, so this function
 *       (and its helpers) should avoid exiting on error.  This is not to say
 *       they shouldn't detect and print (or otherwise handle) errors!
 */
void eval(const char *cmdline) {
    parseline_return parse_result;
    struct cmdline_tokens token;
    pid_t pid;
    sigset_t mask_all, prev_all;
    jid_t jid;
    char *num;

    // Parse command line
    parse_result = parseline(cmdline, &token);

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY) {
        return;
    }

    sigfillset(&mask_all);
    // not a built-in command
    if (token.builtin == BUILTIN_NONE) {
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all); // block four signals
        if ((pid = fork()) == 0) {                    // child runs user job
            // input redirection
            if (token.infile != NULL) {
                int fd = open(token.infile, O_RDONLY, 0);
                if (fd != -1) {
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                } else {
                    if (errno == 2) {
                        sio_printf("%s: No such file or directory\n",
                                   token.infile);
                    } else if (errno == 13) {
                        sio_printf("%s: Permission denied\n", token.infile);
                    }
                    _exit(0);
                }
            }
            // output redirection
            if (token.outfile != NULL) {
                int fd =
                    open(token.outfile, O_CREAT | O_TRUNC | O_WRONLY, MODE);
                if (fd != -1) {
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                } else {
                    if (errno == 2) {
                        sio_printf("%s: No such file or directory\n",
                                   token.outfile);
                    } else if (errno == 13) {
                        sio_printf("%s: Permission denied\n", token.outfile);
                    }
                    _exit(0);
                }
            }
            sigprocmask(SIG_SETMASK, &prev_all, NULL); // unblock all signals
            setpgid(0, 0);
            if (execve(token.argv[0], token.argv, environ) < 0) {
                if (errno == 2) {
                    sio_printf("%s: No such file or directory\n",
                               token.argv[0]);
                } else if (errno == 13) {
                    sio_printf("%s: Permission denied\n", token.argv[0]);
                }
                _exit(0);
            }
        }

        // foreground job
        if (parse_result == PARSELINE_FG) {
            add_job(pid, FG, cmdline);
            while ((fg_job() != 0) && (pid == job_get_pid(fg_job()))) {
                // wait for child process to terminate
                sigsuspend(&prev_all);
            }
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
        }
        // background job
        if (parse_result == PARSELINE_BG) {
            sigprocmask(SIG_BLOCK, &mask_all, NULL);
            add_job(pid, BG, cmdline);
            jid = job_from_pid(pid);
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            sio_printf("[%d] (%d) %s\n", jid, pid, cmdline);
        }
        return;
    }

    // if built-in command
    if (token.builtin == BUILTIN_QUIT) { // quit command
        _exit(0);
    }
    if (token.builtin == BUILTIN_JOBS) { // lists all background jobs
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
        if (token.outfile != NULL) {
            int fd = open(token.outfile, O_CREAT | O_TRUNC | O_WRONLY, MODE);
            if (fd != -1) {
                if (!list_jobs(fd)) {
                    sio_printf("Fails to write into job list.\n");
                }
                close(fd);
            } else {
                if (errno == 2) {
                    sio_printf("%s: No such file or directory\n",
                               token.outfile);
                } else if (errno == 13) {
                    sio_printf("%s: Permission denied\n", token.outfile);
                }
            }
        } else {
            if (!list_jobs(STDOUT_FILENO)) {
                sio_printf("Fails to write into job list.\n");
            }
        }
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }
    if (token.builtin == BUILTIN_BG) { // bg command
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
        if (token.argc == 1) {
            sio_printf("bg command requires PID or %%jobid argument\n");
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            return;
        }
        char *arg = token.argv[1];
        if (arg[0] != '%' && (!isdigit(arg[0]))) {
            sio_printf("bg: argument must be a PID or %%jobid\n");
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            return;
        }
        if (arg[0] == '%') {
            num = (arg + 1);
            jid = strtol(num, NULL, 10);
            if (!job_exists(jid)) {
                sio_printf("%s: No such job\n", arg);
                sigprocmask(SIG_SETMASK, &prev_all, NULL);
                return;
            }
            pid = job_get_pid(jid);
        } else {
            num = arg;
            pid = strtol(num, NULL, 10);
            jid = job_from_pid(pid);
            if (!job_exists(jid)) {
                sio_printf("%s: No such job\n", arg);
                sigprocmask(SIG_SETMASK, &prev_all, NULL);
                return;
            }
        }
        sio_printf("[%d] (%d) %s\n", jid, pid, job_get_cmdline(jid));
        kill(-pid, SIGCONT);
        job_set_state(jid, BG);
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }
    if (token.builtin == BUILTIN_FG) { // fg command
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
        if (token.argc == 1) {
            sio_printf("fg command requires PID or %%jobid argument\n");
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            return;
        }
        char *arg = token.argv[1];
        if (arg[0] != '%' && (!isdigit(arg[0]))) {
            sio_printf("fg: argument must be a PID or %%jobid\n");
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            return;
        }
        if (arg[0] == '%') {
            num = (arg + 1);
            jid = strtol(num, NULL, 10);
            if (!job_exists(jid)) {
                sio_printf("%s: No such job\n", arg);
                sigprocmask(SIG_SETMASK, &prev_all, NULL);
                return;
            }
            pid = job_get_pid(jid);
        } else {
            num = arg;
            pid = strtol(num, NULL, 10);
            jid = job_from_pid(pid);
            if (!job_exists(jid)) {
                sio_printf("%s: No such job\n", arg);
                sigprocmask(SIG_SETMASK, &prev_all, NULL);
                return;
            }
        }
        kill(-pid, SIGCONT);
        job_set_state(jid, FG);
        while ((fg_job() != 0) && (pid == job_get_pid(fg_job()))) {
            sigsuspend(&prev_all);
        }
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }
}

/*****************
 * Signal handlers
 *****************/

/**
 * @brief deal with event when a child process has stopped or terminated
 * @default action: Ignore
 *
 */
void sigchld_handler(int sig) {
    int olderrno = errno;
    pid_t pid;
    jid_t jid;
    int status;
    sigset_t mask_all, prev_all;

    sigfillset(&mask_all);
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) >
           0) { // reap zombie children
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
        jid = job_from_pid(pid);
        if (WIFSTOPPED(status)) {
            job_set_state(jid, ST);
            sio_printf("Job [%d] (%d) stopped by signal %d\n", jid, pid,
                       WSTOPSIG(status));
        } else if (WIFSIGNALED(status)) {
            sio_printf("Job [%d] (%d) terminated by signal %d\n", jid, pid,
                       WTERMSIG(status));
            delete_job(jid);
        } else if (WIFCONTINUED(status)) {
            job_set_state(jid, FG);
        } else {
            delete_job(jid);
        }
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }
    errno = olderrno;
}

/**
 * @brief Terminate - interrupt from keyboard
 *
 */
void sigint_handler(int sig) {
    int olderrno = errno;
    pid_t pid;
    jid_t jid;
    sigset_t mask_all, prev_all;

    sigfillset(&mask_all);
    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
    jid = fg_job();
    if (jid) {
        pid = job_get_pid(jid);
        // send this sigint to foreground process
        kill(-pid, SIGINT);
    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL);
    errno = olderrno;
}

/**
 * @brief stop until next SIGCONT (from terminal)
 *
 */
void sigtstp_handler(int sig) {
    int olderrno = errno;
    pid_t pid;
    jid_t jid;
    sigset_t mask_all, prev_all;

    sigfillset(&mask_all);
    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
    jid = fg_job();
    if (jid) {
        pid = job_get_pid(jid);
        // send this sigtstp to foreground process
        // -pid to send to all processes within the group
        kill(-pid, SIGTSTP);
    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL);
    errno = olderrno;
}

/**
 * @brief Attempt to clean up global resources when the program exits.
 *
 * In particular, the job list must be freed at this time, since it may
 * contain leftover buffers from existing or even deleted jobs.
 */
void cleanup(void) {
    // Signals handlers need to be removed before destroying the joblist
    Signal(SIGINT, SIG_DFL);  // Handles Ctrl-C
    Signal(SIGTSTP, SIG_DFL); // Handles Ctrl-Z
    Signal(SIGCHLD, SIG_DFL); // Handles terminated or stopped child

    destroy_job_list();
}
