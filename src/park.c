#include "couart.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

enum { SEAT_FDS = COUART_SEAT_COUNT * 2 };

static const char *k_roles[COUART_SEAT_COUNT] = { "console", "agent", "watch" };
static const int k_writable[COUART_SEAT_COUNT] = { 1, 1, 0 };

int couart_pts_foreign_openers(const char *pts_name, pid_t skip)
{
    struct stat want;
    if (!pts_name || !pts_name[0] || stat(pts_name, &want) != 0)
        return 0;

    DIR *proc = opendir("/proc");
    if (!proc)
        return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(proc))) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 0 || pid == skip)
            continue;
        char fdpath[64];
        snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd", (int)pid);
        DIR *fds = opendir(fdpath);
        if (!fds)
            continue;
        struct dirent *fe;
        while ((fe = readdir(fds))) {
            if (!isdigit((unsigned char)fe->d_name[0]))
                continue;
            char p[96];
            snprintf(p, sizeof(p), "%s/%s", fdpath, fe->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && st.st_dev == want.st_dev &&
                st.st_ino == want.st_ino)
                count++;
        }
        closedir(fds);
    }
    closedir(proc);
    return count;
}

static int any_foreign(couart_seat_t seats[COUART_SEAT_COUNT], pid_t skip)
{
    for (int i = 0; i < COUART_SEAT_COUNT; i++) {
        if (seats[i].pts_name[0] &&
            couart_pts_foreign_openers(seats[i].pts_name, skip) > 0)
            return 1;
    }
    return 0;
}

static void collect_fds(couart_seat_t seats[COUART_SEAT_COUNT], int fds[SEAT_FDS])
{
    for (int i = 0; i < COUART_SEAT_COUNT; i++) {
        fds[i * 2] = seats[i].master_fd;
        fds[i * 2 + 1] = seats[i].slave_fd;
    }
}

static int send_fds(int sock, int *fds, int n, const char *msg)
{
    struct iovec iov = { .iov_base = (void *)msg, .iov_len = strlen(msg) };
    union {
        struct cmsghdr hdr;
        char buf[CMSG_SPACE(sizeof(int) * SEAT_FDS)];
    } cmsg;
    memset(&cmsg, 0, sizeof(cmsg));
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsg.buf;
    mh.msg_controllen = CMSG_SPACE(sizeof(int) * n);
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (!c)
        return -1;
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)n);
    return sendmsg(sock, &mh, 0) < 0 ? -1 : 0;
}

static int recv_fds(int sock, int *fds, int n, char *msg, size_t msg_n)
{
    struct iovec iov = { .iov_base = msg, .iov_len = msg_n - 1 };
    union {
        struct cmsghdr hdr;
        char buf[CMSG_SPACE(sizeof(int) * SEAT_FDS)];
    } cmsg;
    memset(&cmsg, 0, sizeof(cmsg));
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsg.buf;
    mh.msg_controllen = sizeof(cmsg.buf);
    ssize_t r = recvmsg(sock, &mh, 0);
    if (r <= 0)
        return -1;
    msg[r] = '\0';
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
        return -1;
    size_t got = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    if ((int)got != n)
        return -1;
    memcpy(fds, CMSG_DATA(c), sizeof(int) * (size_t)n);
    return 0;
}

static int bind_park(const char *path)
{
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    return fd;
}

static void drain_master(int fd)
{
    uint8_t buf[256];
    while (read(fd, buf, sizeof(buf)) > 0)
        ;
}

static void close_inherited_except(couart_seat_t seats[COUART_SEAT_COUNT], int listen_fd)
{
    int keep[16];
    int nk = 0;
    keep[nk++] = listen_fd;
    for (int i = 0; i < COUART_SEAT_COUNT; i++) {
        if (seats[i].master_fd >= 0)
            keep[nk++] = seats[i].master_fd;
        if (seats[i].slave_fd >= 0)
            keep[nk++] = seats[i].slave_fd;
    }
    int maxfd = (int)sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 1024)
        maxfd = 1024;
    for (int fd = 0; fd < maxfd; fd++) {
        int hold = 0;
        for (int j = 0; j < nk; j++) {
            if (keep[j] == fd) {
                hold = 1;
                break;
            }
        }
        if (!hold)
            close(fd);
    }
}

static void park_child(couart_seat_t seats[COUART_SEAT_COUNT], int listen_fd,
                       const char *name)
{
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    (void)setsid();
    close_inherited_except(seats, listen_fd);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        _exit(1);
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 100 };
    epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev);
    for (int i = 0; i < COUART_SEAT_COUNT; i++) {
        if (seats[i].master_fd < 0)
            continue;
        ev.data.u32 = (uint32_t)i;
        epoll_ctl(ep, EPOLL_CTL_ADD, seats[i].master_fd, &ev);
    }

    const char *banner = "\r\n[couart] hub parked — PTY kept for the terminal\r\n";
    for (int i = 0; i < COUART_SEAT_COUNT; i++)
        (void)couart_seat_write(&seats[i], (const uint8_t *)banner, strlen(banner));

    struct epoll_event evs[8];
    for (;;) {
        int n = epoll_wait(ep, evs, 8, 1000);
        if (n < 0 && errno == EINTR)
            continue;
        for (int i = 0; i < n; i++) {
            uint32_t tag = evs[i].data.u32;
            if (tag == 100) {
                int cfd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
                if (cfd < 0)
                    continue;
                char dummy[16];
                (void)read(cfd, dummy, sizeof(dummy));
                int fds[SEAT_FDS];
                collect_fds(seats, fds);
                char msg[512];
                size_t off = 0;
                off += (size_t)snprintf(msg + off, sizeof(msg) - off, "OK\n");
                for (int s = 0; s < COUART_SEAT_COUNT; s++)
                    off += (size_t)snprintf(msg + off, sizeof(msg) - off, "%s\n",
                                            seats[s].pts_name);
                (void)send_fds(cfd, fds, SEAT_FDS, msg);
                close(cfd);
                close(listen_fd);
                char path[COUART_PATH_MAX];
                if (couart_park_path(path, sizeof(path), name) == 0)
                    unlink(path);
                _exit(0);
            }
            if (tag < (uint32_t)COUART_SEAT_COUNT)
                drain_master(seats[tag].master_fd);
        }
        if (!any_foreign(seats, getpid()))
            break;
    }

    close(listen_fd);
    char path[COUART_PATH_MAX];
    if (couart_park_path(path, sizeof(path), name) == 0)
        unlink(path);
    for (int i = 0; i < COUART_SEAT_COUNT; i++)
        couart_seat_close(&seats[i]);
    char dir[COUART_PATH_MAX];
    if (couart_instance_dir(dir, sizeof(dir), name) == 0)
        rmdir(dir);
    _exit(0);
}

int couart_seats_adopt(couart_seat_t seats[COUART_SEAT_COUNT], const char *name)
{
    char path[COUART_PATH_MAX];
    if (couart_park_path(path, sizeof(path), name) != 0)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    (void)write(fd, "ADOPT\n", 6);
    int fds[SEAT_FDS];
    char msg[512];
    if (recv_fds(fd, fds, SEAT_FDS, msg, sizeof(msg)) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    int ok = (strncmp(msg, "OK\n", 3) == 0);
    char *p = msg + 3;
    char pts[COUART_SEAT_COUNT][128];
    memset(pts, 0, sizeof(pts));
    for (int i = 0; ok && i < COUART_SEAT_COUNT; i++) {
        char *nl = strchr(p, '\n');
        if (!nl) {
            ok = 0;
            break;
        }
        *nl = '\0';
        snprintf(pts[i], sizeof(pts[i]), "%s", p);
        p = nl + 1;
    }
    if (!ok) {
        for (int i = 0; i < SEAT_FDS; i++)
            close(fds[i]);
        return -1;
    }

    for (int i = 0; i < COUART_SEAT_COUNT; i++) {
        memset(&seats[i], 0, sizeof(seats[i]));
        seats[i].master_fd = fds[i * 2];
        seats[i].slave_fd = fds[i * 2 + 1];
        seats[i].role = k_roles[i];
        seats[i].writable = k_writable[i];
        snprintf(seats[i].pts_name, sizeof(seats[i].pts_name), "%s", pts[i]);
        if (couart_seat_path(seats[i].link_path, sizeof(seats[i].link_path),
                             name, k_roles[i]) != 0) {
            for (int j = i; j < COUART_SEAT_COUNT; j++) {
                close(fds[j * 2]);
                close(fds[j * 2 + 1]);
            }
            return -1;
        }
        seats[i].link_owned = 1;
        fcntl(seats[i].master_fd, F_SETFD, FD_CLOEXEC);
        fcntl(seats[i].slave_fd, F_SETFD, FD_CLOEXEC);
        int fl = fcntl(seats[i].master_fd, F_GETFL, 0);
        if (fl >= 0)
            fcntl(seats[i].master_fd, F_SETFL, fl | O_NONBLOCK);
    }
    COUART_INFO("adopted parked seats for %s", name);
    return 0;
}

int couart_seats_park(couart_seat_t seats[COUART_SEAT_COUNT], const char *name)
{
    if (!any_foreign(seats, getpid()))
        return 0;
    char path[COUART_PATH_MAX];
    if (couart_park_path(path, sizeof(path), name) != 0)
        return -1;
    int listen_fd = bind_park(path);
    if (listen_fd < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(listen_fd);
        unlink(path);
        return -1;
    }
    if (pid == 0)
        park_child(seats, listen_fd, name);

    close(listen_fd);
    COUART_INFO("parked seats (pty held until terminal closes or next attach)");
    return 1;
}
