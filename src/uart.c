#include "couart.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
    case 460800:  return B460800;
    case 921600:  return B921600;
    default:      return 0;
    }
}

static void remember_by_id(couart_uart_t *u, const char *opened)
{
    if (!u || u->by_id[0] || !opened || !opened[0])
        return;
    if (strstr(opened, "/serial/by-id/")) {
        snprintf(u->by_id, sizeof(u->by_id), "%s", opened);
        return;
    }
    char real[PATH_MAX];
    if (!realpath(opened, real))
        return;
    DIR *d = opendir("/dev/serial/by-id");
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        char cand[COUART_PATH_MAX];
        if (snprintf(cand, sizeof(cand), "/dev/serial/by-id/%s", de->d_name) >=
            (int)sizeof(cand))
            continue;
        char tgt[PATH_MAX];
        if (realpath(cand, tgt) && strcmp(tgt, real) == 0) {
            snprintf(u->by_id, sizeof(u->by_id), "%s", cand);
            break;
        }
    }
    closedir(d);
}

int couart_uart_valid_baud(int baud)
{
    return baud_to_speed(baud) != 0;
}

int couart_uart_same_node(const couart_uart_t *u, const char *path)
{
    struct stat fs, ns;
    if (!u || u->fd < 0 || !path || !path[0])
        return 0;
    if (fstat(u->fd, &fs) < 0 || stat(path, &ns) < 0)
        return 0;
    return fs.st_dev == ns.st_dev && fs.st_ino == ns.st_ino;
}

int couart_uart_set_baud(couart_uart_t *u, int baud)
{
    speed_t sp = baud_to_speed(baud);
    if (!u || u->fd < 0 || sp == 0)
        return -1;
    struct termios tio;
    if (tcgetattr(u->fd, &tio) < 0)
        return -1;
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    if (tcsetattr(u->fd, TCSANOW, &tio) < 0)
        return -1;
    u->baud = baud;
    return 0;
}

int couart_uart_fd_current(const couart_uart_t *u)
{
    struct stat fs, ns;
    if (!u || u->fd < 0 || fstat(u->fd, &fs) < 0)
        return 0;
    if (u->by_id[0] && stat(u->by_id, &ns) == 0)
        return fs.st_dev == ns.st_dev && fs.st_ino == ns.st_ino;
    if (u->path[0] && stat(u->path, &ns) == 0)
        return fs.st_dev == ns.st_dev && fs.st_ino == ns.st_ino;
    return 0;
}

int couart_uart_open(couart_uart_t *u, const char *path, int baud)
{
    speed_t sp = baud_to_speed(baud);
    if (!u || !path || !path[0] || sp == 0)
        return -1;

    const char *try[2];
    int ntry = 0;
    try[ntry++] = path;
    if (u->by_id[0] && strcmp(u->by_id, path) != 0)
        try[ntry++] = u->by_id;

    int fd = -1;
    const char *used = NULL;
    for (int i = 0; i < ntry; i++) {
        fd = open(try[i], O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            used = try[i];
            break;
        }
    }
    if (fd < 0)
        return -1;

    if (ioctl(fd, TIOCEXCL) < 0) {
        close(fd);
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~HUPCL;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        close(fd);
        return -1;
    }

    u->fd = fd;
    u->baud = baud;
    char real[PATH_MAX];
    if (realpath(used, real))
        snprintf(u->path, sizeof(u->path), "%s", real);
    else
        snprintf(u->path, sizeof(u->path), "%s", used);
    remember_by_id(u, used);
    return 0;
}

void couart_uart_close(couart_uart_t *u)
{
    if (!u || u->fd < 0)
        return;
    ioctl(u->fd, TIOCNXCL);
    close(u->fd);
    u->fd = -1;
}

int couart_uart_write(couart_uart_t *u, const uint8_t *data, size_t len)
{
    if (!u || u->fd < 0 || !data || len == 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(u->fd, data + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            break;
        return -1;
    }
    return (int)off;
}
