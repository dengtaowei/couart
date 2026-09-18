#include "couart.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_unix(const char *path, int timeout_ms)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    if (poll(&pfd, 1, timeout_ms) <= 0) {
        close(fd);
        return -1;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

int couart_ctl_request(const char *name, const char *cmd,
                       char *reply, size_t reply_n, int timeout_ms)
{
    char path[COUART_PATH_MAX];
    if (couart_control_path(path, sizeof(path), name) != 0)
        return -1;

    int fd = connect_unix(path, timeout_ms);
    if (fd < 0)
        return -1;

    dprintf(fd, "%s\n", cmd);

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) <= 0) {
        close(fd);
        return -1;
    }

    size_t off = 0;
    while (off + 1 < reply_n) {
        ssize_t n = read(fd, reply + off, reply_n - off - 1);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        break;
    }
    reply[off] = '\0';
    close(fd);
    return off > 0 ? 0 : -1;
}
