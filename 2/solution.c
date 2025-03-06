#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>

#define ONEW 0
#define APP  1

static int xp(struct expr **cmds, int n, char *of, int ot, int bg) {
    if (n == 1) {
        char *c = cmds[0]->cmd.exe;
        if (!strcmp(c, "cd")) {
            char *d = cmds[0]->cmd.arg_count > 0 ? cmds[0]->cmd.args[0] : getenv("HOME");
            if (chdir(d) != 0) perror("cd");
            return 0;
        }
        if (!strcmp(c, "exit") && !of) {
            int ec = 0;
            if (cmds[0]->cmd.arg_count > 0) ec = atoi(cmds[0]->cmd.args[0]);
            exit(ec);
        }
    }
    int i, ret = 0;
    int pfd[2], prev = -1;
    pid_t *pids = malloc(n * sizeof(pid_t));
    for (i = 0; i < n; i++) {
        if (i < n - 1) {
            if (pipe(pfd) < 0) {
                perror("pipe");
                exit(1);
            }
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        if (pid == 0) {
            if (prev != -1) {
                dup2(prev, STDIN_FILENO);
                close(prev);
            }
            if (i < n - 1) {
                close(pfd[0]);
                dup2(pfd[1], STDOUT_FILENO);
                close(pfd[1]);
            } else if (of) {
                int fl = (ot == ONEW)
                    ? open(of, O_WRONLY | O_CREAT | O_TRUNC, 0644)
                    : open(of, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fl < 0) {
                    perror("open");
                    exit(1);
                }
                dup2(fl, STDOUT_FILENO);
                close(fl);
            }
            int total_args = cmds[i]->cmd.arg_count + 2;
            char **argv = malloc(sizeof(char*) * total_args);
            argv[0] = cmds[i]->cmd.exe;
            for (uint32_t j = 0; j < cmds[i]->cmd.arg_count; j++) {
                argv[j + 1] = cmds[i]->cmd.args[j];
            }
            argv[cmds[i]->cmd.arg_count + 1] = NULL;
            if (!strcmp(argv[0], "exit")) {
                int ec = 0;
                if (cmds[i]->cmd.arg_count > 0) ec = atoi(argv[1]);
                _exit(ec);
            }
            execvp(argv[0], argv);
            perror(argv[0]);
            _exit(1);
        } else {
            pids[i] = pid;
            if (prev != -1) close(prev);
            if (i < n - 1) {
                close(pfd[1]);
                prev = pfd[0];
            }
        }
    }
    if (!bg) {
        for (i = 0; i < n; i++) {
            int st;
            waitpid(pids[i], &st, 0);
            if (i == n - 1) ret = WEXITSTATUS(st);
        }
    }
    free(pids);
    return ret;
}

static int exec_line(struct command_line *cl) {
    int ret = 0;
    struct expr *e = cl->head;
    while (e) {
        int cnt = 0, cap = 4;
        struct expr **arr = malloc(cap * sizeof(struct expr*));
        while (e && (e->type == EXPR_TYPE_COMMAND || e->type == EXPR_TYPE_PIPE)) {
            if (e->type == EXPR_TYPE_COMMAND) {
                if (cnt >= cap) {
                    cap *= 2;
                    arr = realloc(arr, cap * sizeof(struct expr*));
                }
                arr[cnt++] = e;
            }
            e = e->next;
        }
        ret = xp(arr, cnt, cl->out_file,
                 (cl->out_type == OUTPUT_TYPE_FILE_NEW) ? ONEW : APP,
                 cl->is_background);
        free(arr);
        if (e && (e->type == EXPR_TYPE_AND || e->type == EXPR_TYPE_OR)) {
            int op = e->type;
            e = e->next;
            if (op == 1 && ret != 0)
                while (e && (e->type == EXPR_TYPE_COMMAND || e->type == 2))
                    e = e->next;
            else if (op == 2 && ret == 0)
                while (e && (e->type == 0 || e->type == 2))
                    e = e->next;
        }
    }
    return ret;
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);
    struct parser *p = parser_new();
    char buf[1024];
    int r;
    while ((r = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        parser_feed(p, buf, r);
        struct command_line *cl = NULL;
        while (1) {
            enum parser_error err = parser_pop_next(p, &cl);
            if (err == 0 && cl == NULL) break;
            if (err != 0) {
                fprintf(stderr, "Ошибка парсера: %d\n", (int)err);
                continue;
            }
            exec_line(cl);
            command_line_delete(cl);
        }
    }
    parser_delete(p);
    return 0;
}
