#include "couart.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static couart_hub_t *g_hub;

static void on_signal(int sig)
{
    (void)sig;
    if (g_hub)
        couart_hub_request_stop(g_hub);
}

static int write_pid(const char *name, int *out_fd)
{
    char path[COUART_PATH_MAX];
    if (couart_pid_path(path, sizeof(path), name) != 0)
        return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        errno = EBUSY;
        return -1;
    }
    if (ftruncate(fd, 0) < 0) {
        close(fd);
        return -1;
    }
    dprintf(fd, "%d\n", (int)getpid());
    *out_fd = fd;
    return 0;
}

static void usage(FILE *fp)
{
    fprintf(fp,
            "usage: couart serve --name NAME --port DEVICE [--baud RATE] [--foreground]\n");
}

int couart_serve(int argc, char **argv)
{
    const char *name = NULL;
    const char *port = NULL;
    int baud = 115200;
    int foreground = 0;

    static const struct option opts[] = {
        {"name", required_argument, NULL, 'n'},
        {"port", required_argument, NULL, 'p'},
        {"baud", required_argument, NULL, 'b'},
        {"foreground", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "n:p:b:fh", opts, NULL)) != -1) {
        switch (c) {
        case 'n': name = optarg; break;
        case 'p': port = optarg; break;
        case 'b': baud = atoi(optarg); break;
        case 'f': foreground = 1; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (!name || !port) {
        usage(stderr);
        return 2;
    }

    int ready[2] = {-1, -1};
    if (!foreground) {
        if (pipe(ready) < 0)
            return 1;
        pid_t pid = fork();
        if (pid < 0)
            return 1;
        if (pid > 0) {
            close(ready[1]);
            char st = 1;
            if (read(ready[0], &st, 1) != 1)
                st = 1;
            close(ready[0]);
            return st == 0 ? 0 : 1;
        }
        close(ready[0]);
        if (setsid() < 0)
            _exit(1);
        signal(SIGHUP, SIG_IGN);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null > 2)
                close(null);
        }
    }

    int pid_fd = -1;
    {
        char root[COUART_PATH_MAX], inst[COUART_PATH_MAX];
        if (couart_runtime_dir(root, sizeof(root)) != 0 ||
            couart_instance_dir(inst, sizeof(inst), name) != 0 ||
            couart_ensure_dir(root, 0700) != 0 ||
            couart_ensure_dir(inst, 0700) != 0) {
            COUART_ERROR("cannot create instance dir: %s", strerror(errno));
            if (ready[1] >= 0) {
                char st = 1;
                (void)write(ready[1], &st, 1);
                close(ready[1]);
            }
            return 1;
        }
    }
    if (write_pid(name, &pid_fd) != 0) {
        COUART_ERROR("instance '%s' already running or cannot lock pid: %s",
                     name, strerror(errno));
        if (ready[1] >= 0) {
            char st = 1;
            (void)write(ready[1], &st, 1);
            close(ready[1]);
        }
        return 1;
    }

    couart_hub_t hub;
    if (couart_hub_init(&hub, name, port, baud) != 0) {
        if (pid_fd >= 0)
            close(pid_fd);
        if (ready[1] >= 0) {
            char st = 1;
            (void)write(ready[1], &st, 1);
            close(ready[1]);
        }
        return 1;
    }

    if (ready[1] >= 0) {
        char st = 0;
        (void)write(ready[1], &st, 1);
        close(ready[1]);
        ready[1] = -1;
    }

    g_hub = &hub;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int rc = couart_hub_run(&hub);
    g_hub = NULL;
    couart_hub_fini(&hub);
    if (pid_fd >= 0) {
        char path[COUART_PATH_MAX];
        if (couart_pid_path(path, sizeof(path), name) == 0)
            unlink(path);
        close(pid_fd);
    }
    return rc == 0 ? 0 : 1;
}
