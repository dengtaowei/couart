#include "couart.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static int set_raw_nonblock(int fd)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~HUPCL;
        if (tcsetattr(fd, TCSANOW, &tio) < 0)
            return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void unlink_if_ours(couart_seat_t *s)
{
    if (!s->link_owned || !s->link_path[0])
        return;
    char buf[128];
    ssize_t n = readlink(s->link_path, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (strcmp(buf, s->pts_name) == 0)
            unlink(s->link_path);
    }
    s->link_owned = 0;
}

int couart_seat_open(couart_seat_t *s, const char *link_path,
                     const char *role, int writable)
{
    if (!s || !link_path || !role)
        return -1;

    memset(s, 0, sizeof(*s));
    s->master_fd = -1;
    s->slave_fd = -1;
    s->role = role;
    s->writable = writable;
    snprintf(s->link_path, sizeof(s->link_path), "%s", link_path);

    if (openpty(&s->master_fd, &s->slave_fd, s->pts_name, NULL, NULL) < 0)
        return -1;

    if (set_raw_nonblock(s->master_fd) < 0 || set_raw_nonblock(s->slave_fd) < 0) {
        close(s->master_fd);
        close(s->slave_fd);
        s->master_fd = s->slave_fd = -1;
        return -1;
    }
    fcntl(s->master_fd, F_SETFD, FD_CLOEXEC);
    fcntl(s->slave_fd, F_SETFD, FD_CLOEXEC);

    struct stat st;
    if (lstat(link_path, &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            COUART_ERROR("%s exists and is not a symlink", link_path);
            close(s->master_fd);
            close(s->slave_fd);
            s->master_fd = s->slave_fd = -1;
            return -1;
        }
        unlink(link_path);
    }
    if (symlink(s->pts_name, link_path) < 0) {
        COUART_ERROR("symlink %s -> %s: %s", link_path, s->pts_name, strerror(errno));
        close(s->master_fd);
        close(s->slave_fd);
        s->master_fd = s->slave_fd = -1;
        return -1;
    }
    s->link_owned = 1;
    return 0;
}

void couart_seat_close(couart_seat_t *s)
{
    if (!s)
        return;
    unlink_if_ours(s);
    if (s->master_fd >= 0) {
        close(s->master_fd);
        s->master_fd = -1;
    }
    if (s->slave_fd >= 0) {
        close(s->slave_fd);
        s->slave_fd = -1;
    }
}

static int slave_echo_on(const couart_seat_t *s)
{
    if (!s || s->slave_fd < 0)
        return 0;
    struct termios tio;
    if (tcgetattr(s->slave_fd, &tio) < 0)
        return 0;
    return (tio.c_lflag & (ECHO | ECHONL)) != 0;
}

static void echo_note(couart_seat_t *s, const uint8_t *data, size_t len)
{
    if (!s || !data || len == 0)
        return;
    /* With ECHO off (normal serial), RX we write to the master never
     * comes back. Remembering it would eat the next real CR/keys. */
    if (!slave_echo_on(s)) {
        s->echo_len = 0;
        return;
    }
    if (len >= sizeof(s->echo)) {
        memcpy(s->echo, data + (len - sizeof(s->echo)), sizeof(s->echo));
        s->echo_len = sizeof(s->echo);
        return;
    }
    if (s->echo_len + len > sizeof(s->echo)) {
        size_t drop = s->echo_len + len - sizeof(s->echo);
        memmove(s->echo, s->echo + drop, s->echo_len - drop);
        s->echo_len -= drop;
    }
    memcpy(s->echo + s->echo_len, data, len);
    s->echo_len += len;
}

int couart_seat_write(couart_seat_t *s, const uint8_t *data, size_t len)
{
    if (!s || s->master_fd < 0 || !data || len == 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(s->master_fd, data + off, len - off);
        if (n > 0) {
            echo_note(s, data + off, (size_t)n);
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EIO || errno == EINTR))
            break;
        break;
    }
    return (int)off;
}

size_t couart_seat_filter_tx(couart_seat_t *s, uint8_t *data, size_t len)
{
    if (!s || !data || len == 0)
        return 0;
    if (!slave_echo_on(s)) {
        s->echo_len = 0;
        return len;
    }
    size_t i = 0;
    while (i < len && s->echo_len > 0) {
        if (data[i] == s->echo[0]) {
            memmove(s->echo, s->echo + 1, s->echo_len - 1);
            s->echo_len--;
            i++;
            continue;
        }
        /* Desync: this is real typing, not an echo of our RX. */
        s->echo_len = 0;
        break;
    }
    if (i == 0)
        return len;
    if (i >= len)
        return 0;
    memmove(data, data + i, len - i);
    return len - i;
}

void couart_seat_keep_raw(couart_seat_t *s)
{
    if (!s || s->slave_fd < 0)
        return;
    struct termios tio;
    if (tcgetattr(s->slave_fd, &tio) < 0)
        return;
    if ((tio.c_lflag & (ECHO | ECHOE | ECHOK | ECHONL | ICANON | ISIG | IEXTEN)) == 0 &&
        (tio.c_oflag & OPOST) == 0) {
        s->echo_len = 0;
        return;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~HUPCL;
    (void)tcsetattr(s->slave_fd, TCSANOW, &tio);
    s->echo_len = 0;
}

void couart_seat_drop_fds(couart_seat_t *s)
{
    if (!s)
        return;
    if (s->master_fd >= 0) {
        close(s->master_fd);
        s->master_fd = -1;
    }
    if (s->slave_fd >= 0) {
        close(s->slave_fd);
        s->slave_fd = -1;
    }
    s->link_owned = 0;
}
