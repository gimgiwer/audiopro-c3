/*
 * Minimal static telnetd with full PTY (pseudoterminal) support for MIPS Linux
 * Spawns an interactive shell on a real PTY master/slave pair (supports top, vi, Ctrl+C).
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <pty.h>
#include <fcntl.h>

#define IAC  255
#define WILL 251
#define WONT 252
#define DO   253
#define DONT 254
#define SB   250
#define SE   240

static void reap(int s) {
    (void)s;
    while (waitpid(-1, 0, WNOHANG) > 0);
}

/* Filter telnet IAC commands and respond with WONT/DONT */
static int filter_telnet_iac(int sock, unsigned char *buf, int len, int pty_fd) {
    unsigned char out[256];
    unsigned char clean[1024];
    int oi = 0, ci = 0;

    for (int i = 0; i < len; ) {
        if (buf[i] == IAC && i + 1 < len) {
            unsigned char cmd = buf[i+1];
            if (cmd == DO || cmd == DONT || cmd == WILL || cmd == WONT) {
                if (i + 2 < len) {
                    unsigned char opt = buf[i+2];
                    if (cmd == DO && oi + 3 <= (int)sizeof(out)) {
                        out[oi++] = IAC; out[oi++] = WONT; out[oi++] = opt;
                    } else if (cmd == WILL && oi + 3 <= (int)sizeof(out)) {
                        out[oi++] = IAC; out[oi++] = DONT; out[oi++] = opt;
                    }
                    i += 3;
                } else {
                    break;
                }
            } else if (cmd == SB) {
                i += 2;
                while (i < len - 1 && !(buf[i] == IAC && buf[i+1] == SE)) i++;
                i += 2;
            } else {
                i += 2;
            }
        } else if (buf[i] == '\0') {
            i++; /* skip null bytes sent after CR */
        } else {
            if (ci < (int)sizeof(clean)) {
                clean[ci++] = buf[i++];
            } else {
                i++;
            }
        }
    }

    if (oi > 0) write(sock, out, oi);
    if (ci > 0) write(pty_fd, clean, ci);
    return 0;
}

/* Session handler: bridges TCP client and PTY master */
static void handle_session(int cli) {
    int master, slave;
    pid_t pid;

    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        close(cli);
        _exit(1);
    }

    pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        close(cli);
        _exit(1);
    }

    if (pid == 0) {
        /* Child process: setup controlling tty and launch shell */
        close(cli);
        close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);

        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);

        setenv("TERM", "vt100", 1);
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/system/workdir/bin", 1);
        execl("/bin/sh", "sh", "-i", (char *)0);
        _exit(1);
    }

    /* Parent session bridge */
    close(slave);

    /* Initial Telnet options: Suppress Go Ahead (SGA) and Echo */
    unsigned char init_iac[] = {
        IAC, WILL, 1,   /* WILL ECHO */
        IAC, WILL, 3,   /* WILL SUPPRESS GO AHEAD */
        IAC, DO,   3    /* DO SUPPRESS GO AHEAD */
    };
    write(cli, init_iac, sizeof(init_iac));

    struct pollfd fds[2];
    fds[0].fd = cli;
    fds[0].events = POLLIN;
    fds[1].fd = master;
    fds[1].events = POLLIN;

    unsigned char buf[1024];

    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret <= 0) break;

        /* Client -> PTY */
        if (fds[0].revents & POLLIN) {
            int n = read(cli, buf, sizeof(buf));
            if (n <= 0) break;
            filter_telnet_iac(cli, buf, n, master);
        }

        /* PTY -> Client */
        if (fds[1].revents & POLLIN) {
            int n = read(master, buf, sizeof(buf));
            if (n <= 0) break;
            if (write(cli, buf, n) != n) break;
        }

        if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
            (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            break;
        }
    }

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    close(master);
    close(cli);
    _exit(0);
}

int main(void) {
    signal(SIGCHLD, reap);
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return 1;

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(23);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv);
        return 1;
    }
    if (listen(srv, 8) < 0) {
        close(srv);
        return 1;
    }

    while (1) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) continue;

        if (fork() == 0) {
            close(srv);
            handle_session(cli);
        }
        close(cli);
    }
}
