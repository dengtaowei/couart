#include "couart.h"
#include "hex.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const struct {
    const char *role;
    int writable;
} k_roles[COUART_SEAT_COUNT] = {
    {"console", 1},
    {"agent",   1},
    {"watch",   0},
};

enum {
    TAG_UART = 1,
    TAG_LISTEN = 2,
    TAG_WAKE = 3,
    TAG_SEAT0 = 10,
    TAG_CTL0 = 20
};

static void announce(couart_hub_t *h, const char *msg)
{
    size_t n = strlen(msg);
    for (int i = 0; i < COUART_SEAT_COUNT; i++)
        couart_seat_write(&h->seats[i], (const uint8_t *)msg, n);
}

static void fanout_rx(couart_hub_t *h, const uint8_t *data, size_t len, int skip_seat)
{
    for (int i = 0; i < COUART_SEAT_COUNT; i++) {
        if (i == skip_seat)
            continue;
        couart_seat_write(&h->seats[i], data, len);
    }
}

static void hist_add(couart_hub_t *h, const uint8_t *data, size_t len)
{
    if (!data || len == 0)
        return;
    for (size_t i = 0; i < len; i++) {
        h->hist[h->hist_head] = data[i];
        h->hist_head = (h->hist_head + 1) % COUART_HIST_SIZE;
        if (h->hist_len < COUART_HIST_SIZE)
            h->hist_len++;
        h->hist_seq++;
    }
}

static size_t hist_copy_last(const couart_hub_t *h, uint8_t *dst, size_t max)
{
    size_t n = h->hist_len < max ? h->hist_len : max;
    if (n == 0)
        return 0;
    size_t start = (h->hist_head + COUART_HIST_SIZE - n) % COUART_HIST_SIZE;
    for (size_t i = 0; i < n; i++)
        dst[i] = h->hist[(start + i) % COUART_HIST_SIZE];
    return n;
}

static int uart_online(couart_hub_t *h);
static int uart_go_online(couart_hub_t *h, int announce_up);

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void seat_hold_flush(couart_hub_t *h, int idx);

static void tx_unlock(couart_hub_t *h)
{
    if (!h->tx_locked)
        return;
    h->tx_locked = 0;
    for (int i = 0; i < COUART_SEAT_COUNT; i++)
        seat_hold_flush(h, i);
}

static void tx_lock_expire(couart_hub_t *h)
{
    if (h->tx_locked && mono_now() >= h->tx_lock_until) {
        COUART_WARN("tx lock expired");
        tx_unlock(h);
    }
}

static void tx_drain(couart_hub_t *h)
{
    while (h->txq_n > 0 && uart_online(h)) {
        couart_txf_t *f = &h->txq[h->txq_rd];
        int n = couart_uart_write(&h->uart, f->data + f->off, f->len - f->off);
        if (n < 0)
            break;
        if (n > 0) {
            /* Do not mirror TX to seats. A raw PTY displays LF as
             * "next line, same column", and ECHO would loop the bytes
             * back onto the UART — that is how `md.b 0x0 0x20` split. */
            hist_add(h, f->data + f->off, (size_t)n);
            f->off += (size_t)n;
        }
        if (f->off < f->len)
            break;
        h->txq_rd = (h->txq_rd + 1) % COUART_TXQ_MAX;
        h->txq_n--;
    }
}

static int tx_enqueue(couart_hub_t *h, const uint8_t *data, size_t len, int skip_seat)
{
    if (!data || len == 0)
        return 0;
    if (!uart_online(h))
        return -1;
    while (len > 0) {
        if (h->txq_n >= COUART_TXQ_MAX) {
            tx_drain(h);
            if (h->txq_n >= COUART_TXQ_MAX)
                return -1;
        }
        size_t chunk = len < COUART_TXFRAME ? len : COUART_TXFRAME;
        int wr = (h->txq_rd + h->txq_n) % COUART_TXQ_MAX;
        couart_txf_t *f = &h->txq[wr];
        memcpy(f->data, data, chunk);
        f->len = chunk;
        f->off = 0;
        f->skip_seat = skip_seat;
        h->txq_n++;
        data += chunk;
        len -= chunk;
    }
    tx_drain(h);
    return 0;
}

static int add_epoll(couart_hub_t *h, int fd, int tag)
{
    uint32_t events = EPOLLIN;
    if (tag == TAG_UART)
        events |= EPOLLET;
    if (tag == TAG_LISTEN || (tag >= TAG_CTL0 && tag < TAG_CTL0 + COUART_CTL_MAX))
        events |= EPOLLRDHUP;
    struct epoll_event ev = {.events = events, .data.u32 = (uint32_t)tag};
    return epoll_ctl(h->epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void del_epoll(couart_hub_t *h, int fd)
{
    if (fd >= 0)
        epoll_ctl(h->epfd, EPOLL_CTL_DEL, fd, NULL);
}

static int bind_control(couart_hub_t *h)
{
    unlink(h->control_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(h->control_path) >= sizeof(addr.sun_path)) {
        COUART_ERROR("control path too long for AF_UNIX");
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, h->control_path, strlen(h->control_path) + 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    chmod(h->control_path, 0600);
    h->listen_fd = fd;
    return add_epoll(h, fd, TAG_LISTEN);
}

static void close_ctl(couart_hub_t *h, int i)
{
    if (h->ctl[i].fd < 0)
        return;
    del_epoll(h, h->ctl[i].fd);
    close(h->ctl[i].fd);
    h->ctl[i].fd = -1;
    h->ctl[i].in_len = 0;
}

static void reply(int fd, const char *text)
{
    dprintf(fd, "%s", text);
}

static void handle_status(couart_hub_t *h, int fd)
{
    dprintf(fd,
            "OK\n"
            "name=%s\n"
            "port=%s\n"
            "by_id=%s\n"
            "baud=%d\n"
            "suspended=%d\n"
            "uart_online=%d\n"
            "txq=%d\n"
            "tx_locked=%d\n"
            "hist_seq=%llu\n",
            h->name, h->uart.path, h->uart.by_id[0] ? h->uart.by_id : "-",
            h->uart.baud, h->suspended, h->uart_online,
            h->txq_n, h->tx_locked, (unsigned long long)h->hist_seq);
    for (int i = 0; i < COUART_SEAT_COUNT; i++)
        dprintf(fd, "seat.%s=%s\n", h->seats[i].role, h->seats[i].link_path);
}

static int uart_online(couart_hub_t *h)
{
    return h->uart_online && h->uart.fd >= 0 && !h->suspended;
}

static int do_suspend(couart_hub_t *h)
{
    if (h->suspended)
        return 0;
    if (h->uart.fd >= 0) {
        del_epoll(h, h->uart.fd);
        couart_uart_close(&h->uart);
    }
    h->uart_online = 0;
    h->suspended = 1;
    announce(h, "\r\n[couart] port suspended — physical UART released\r\n");
    COUART_INFO("suspended %s", h->uart.path);
    return 0;
}

static int do_resume(couart_hub_t *h)
{
    if (h->uart_online && !h->suspended)
        return 0;
    return uart_go_online(h, 1);
}

static int do_bind(couart_hub_t *h, const char *path, int baud)
{
    if (!path || !path[0] || !couart_uart_valid_baud(baud)) {
        errno = EINVAL;
        return -1;
    }

    /* Same node: TIOCEXCL would block a second open. Change baud in place. */
    if (h->uart.fd >= 0 && couart_uart_same_node(&h->uart, path)) {
        if (couart_uart_set_baud(&h->uart, baud) != 0)
            return -1;
        h->uart_online = 1;
        h->suspended = 0;
        char msg[COUART_PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "\r\n[couart] bound %s @ %d\r\n",
                 h->uart.path, h->uart.baud);
        announce(h, msg);
        COUART_INFO("bound %s @ %d", h->uart.path, h->uart.baud);
        return 0;
    }

    couart_uart_t neu;
    memset(&neu, 0, sizeof(neu));
    neu.fd = -1;
    if (couart_uart_open(&neu, path, baud) != 0)
        return -1;

    if (h->uart.fd >= 0) {
        del_epoll(h, h->uart.fd);
        couart_uart_close(&h->uart);
    }
    h->uart = neu;
    if (add_epoll(h, h->uart.fd, TAG_UART) != 0) {
        couart_uart_close(&h->uart);
        h->uart_online = 0;
        return -1;
    }
    h->uart_online = 1;
    h->suspended = 0;
    char msg[COUART_PATH_MAX + 64];
    snprintf(msg, sizeof(msg), "\r\n[couart] bound %s @ %d\r\n",
             h->uart.path, h->uart.baud);
    announce(h, msg);
    COUART_INFO("bound %s @ %d", h->uart.path, h->uart.baud);
    return 0;
}

static void handle_line(couart_hub_t *h, int cidx, const char *line)
{
    int fd = h->ctl[cidx].fd;
    if (strcmp(line, "PING") == 0) {
        reply(fd, "OK pong\n");
    } else if (strcmp(line, "STATUS") == 0) {
        handle_status(h, fd);
    } else if (strcmp(line, "SUSPEND") == 0) {
        if (do_suspend(h) == 0)
            reply(fd, "OK suspended\n");
        else
            reply(fd, "ERR suspend failed\n");
    } else if (strcmp(line, "RESUME") == 0) {
        if (do_resume(h) == 0)
            reply(fd, "OK resumed\n");
        else
            reply(fd, "ERR resume failed\n");
    } else if (strcmp(line, "SHUTDOWN") == 0) {
        reply(fd, "OK shutting down\n");
        couart_hub_request_stop(h);
    } else if (strncmp(line, "PORT ", 5) == 0) {
        char path[COUART_PATH_MAX];
        int baud = 0;
        int n = sscanf(line + 5, "%255s %d", path, &baud);
        if (n < 1 || !path[0]) {
            reply(fd, "ERR usage: PORT DEVICE [BAUD]\n");
        } else {
            if (n < 2 || baud <= 0)
                baud = h->uart.baud;
            if (!couart_uart_valid_baud(baud)) {
                reply(fd, "ERR bad baud\n");
            } else if (do_bind(h, path, baud) != 0) {
                dprintf(fd, "ERR bind failed: %s\n", strerror(errno));
            } else {
                dprintf(fd, "OK\nport=%s\nby_id=%s\nbaud=%d\n",
                        h->uart.path, h->uart.by_id[0] ? h->uart.by_id : "-",
                        h->uart.baud);
            }
        }
    } else if (strncmp(line, "WRITE ", 6) == 0) {
        uint8_t raw[4096];
        size_t n = 0;
        if (couart_hex_decode(line + 6, raw, sizeof(raw), &n) != 0 || n == 0) {
            reply(fd, "ERR bad hex\n");
        } else if (!uart_online(h)) {
            reply(fd, "ERR uart offline\n");
        } else if (tx_enqueue(h, raw, n, -1) != 0) {
            reply(fd, "ERR tx queue full\n");
        } else {
            dprintf(fd, "OK wrote=%zu\n", n);
        }
    } else if (strcmp(line, "TXLOCK") == 0) {
        tx_lock_expire(h);
        if (h->tx_locked) {
            reply(fd, "ERR busy\n");
        } else {
            h->tx_locked = 1;
            h->tx_lock_until = mono_now() + COUART_TXLOCK_S;
            reply(fd, "OK locked\n");
        }
    } else if (strcmp(line, "TXUNLOCK") == 0) {
        tx_unlock(h);
        reply(fd, "OK unlocked\n");
    } else if (strcmp(line, "HISTORY") == 0 || strncmp(line, "HISTORY ", 8) == 0) {
        size_t want = 8192;
        if (strncmp(line, "HISTORY ", 8) == 0) {
            long v = strtol(line + 8, NULL, 10);
            if (v > 0 && v <= (long)COUART_HIST_SIZE)
                want = (size_t)v;
        }
        uint8_t raw[COUART_HIST_SIZE];
        size_t n = hist_copy_last(h, raw, want);
        char *hex = malloc(n * 2 + 1);
        if (!hex) {
            reply(fd, "ERR oom\n");
        } else {
            couart_hex_encode(raw, n, hex, n * 2 + 1);
            dprintf(fd, "OK bytes=%zu seq=%llu\n%s\n", n,
                    (unsigned long long)h->hist_seq, hex);
            free(hex);
        }
    } else {
        reply(fd, "ERR unknown command\n");
    }
}

static void on_ctl(couart_hub_t *h, int cidx)
{
    couart_client_t *c = &h->ctl[cidx];
    for (;;) {
        if (c->in_len >= sizeof(c->in) - 1) {
            c->in_len = 0;
            reply(c->fd, "ERR line too long\n");
            close_ctl(h, cidx);
            return;
        }
        ssize_t n = read(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            close_ctl(h, cidx);
            return;
        }
        if (n == 0) {
            close_ctl(h, cidx);
            return;
        }
        c->in_len += (size_t)n;
        char *nl;
        while ((nl = memchr(c->in, '\n', c->in_len))) {
            *nl = '\0';
            if (nl > c->in && nl[-1] == '\r')
                nl[-1] = '\0';
            handle_line(h, cidx, c->in);
            size_t rest = c->in_len - (size_t)(nl + 1 - c->in);
            memmove(c->in, nl + 1, rest);
            c->in_len = rest;
            close_ctl(h, cidx);
            return;
        }
    }
}

static void accept_ctl(couart_hub_t *h)
{
    for (;;) {
        int fd = accept4(h->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
            return;
        int slot = -1;
        for (int i = 0; i < COUART_CTL_MAX; i++) {
            if (h->ctl[i].fd < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            dprintf(fd, "ERR too many clients\n");
            close(fd);
            continue;
        }
        h->ctl[slot].fd = fd;
        h->ctl[slot].in_len = 0;
        if (add_epoll(h, fd, TAG_CTL0 + slot) != 0)
            close_ctl(h, slot);
    }
}

static int uart_node_present(const couart_uart_t *u)
{
    struct stat st;
    if (u->by_id[0] && stat(u->by_id, &st) == 0)
        return 1;
    return u->path[0] && stat(u->path, &st) == 0;
}

static void uart_go_offline(couart_hub_t *h, const char *why)
{
    COUART_WARN("uart offline (%s)", why);
    if (h->uart.fd >= 0) {
        del_epoll(h, h->uart.fd);
        couart_uart_close(&h->uart);
    }
    h->uart_online = 0;
    announce(h, "\r\n[couart] UART disconnected\r\n");
}

static int uart_go_online(couart_hub_t *h, int announce_up)
{
    if (h->uart.fd >= 0) {
        del_epoll(h, h->uart.fd);
        couart_uart_close(&h->uart);
    }
    const char *open_path = h->uart.by_id[0] ? h->uart.by_id : h->uart.path;
    if (couart_uart_open(&h->uart, open_path, h->uart.baud) != 0)
        return -1;
    if (add_epoll(h, h->uart.fd, TAG_UART) != 0) {
        couart_uart_close(&h->uart);
        return -1;
    }
    h->uart_online = 1;
    h->suspended = 0;
    if (announce_up)
        announce(h, "\r\n[couart] port resumed\r\n");
    COUART_INFO("uart online %s", h->uart.path);
    return 0;
}

static void on_uart(couart_hub_t *h)
{
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(h->uart.fd, buf, sizeof(buf));
        if (n > 0) {
            fanout_rx(h, buf, (size_t)n, -1);
            hist_add(h, buf, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        /* Phantom HUP: same inode, node still ours — ignore.
         * Replug: same name, new inode — reopen without dropping the seat. */
        if (n == 0 || (n < 0 && (errno == EIO || errno == ENXIO))) {
            if (couart_uart_fd_current(&h->uart))
                return;
            if (uart_node_present(&h->uart)) {
                if (uart_go_online(h, 0) != 0) {
                    h->uart_online = 0;
                    announce(h, "\r\n[couart] UART disconnected\r\n");
                }
                return;
            }
        }
        uart_go_offline(h, n == 0 ? "EOF" : strerror(errno));
        return;
    }
}

static void seat_hold_append(couart_seat_t *s, const uint8_t *data, size_t len)
{
    size_t room = sizeof(s->hold) - s->hold_len;
    if (len > room)
        len = room;
    if (len == 0)
        return;
    memcpy(s->hold + s->hold_len, data, len);
    s->hold_len += len;
}

static void seat_hold_flush(couart_hub_t *h, int idx)
{
    couart_seat_t *s = &h->seats[idx];
    if (!s->writable || s->hold_len == 0)
        return;
    (void)tx_enqueue(h, s->hold, s->hold_len, idx);
    s->hold_len = 0;
}

static void on_seat(couart_hub_t *h, int idx)
{
    couart_seat_t *s = &h->seats[idx];
    uint8_t acc[COUART_TXFRAME];
    size_t alen = 0;
    for (;;) {
        ssize_t n = read(s->master_fd, acc + alen, sizeof(acc) - alen);
        if (n > 0) {
            alen += (size_t)n;
            if (alen >= sizeof(acc))
                break;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR || errno == EIO))
            break;
        break;
    }
    if (alen == 0 || !s->writable)
        return;
    alen = couart_seat_filter_tx(s, acc, alen);
    if (alen == 0)
        return;
    tx_lock_expire(h);
    if (h->tx_locked)
        seat_hold_append(s, acc, alen);
    else
        (void)tx_enqueue(h, acc, alen, idx);
}

int couart_hub_init(couart_hub_t *h, const char *name, const char *port, int baud)
{
    memset(h, 0, sizeof(*h));
    h->epfd = h->listen_fd = h->wake_rd = h->wake_wr = -1;
    h->uart.fd = -1;
    for (int i = 0; i < COUART_CTL_MAX; i++)
        h->ctl[i].fd = -1;
    for (int i = 0; i < COUART_SEAT_COUNT; i++) {
        h->seats[i].master_fd = -1;
        h->seats[i].slave_fd = -1;
    }

    if (!couart_valid_name(name)) {
        COUART_ERROR("invalid instance name '%s'", name ? name : "");
        return -1;
    }
    snprintf(h->name, sizeof(h->name), "%s", name);
    if (couart_instance_dir(h->instance_dir, sizeof(h->instance_dir), name) != 0 ||
        couart_control_path(h->control_path, sizeof(h->control_path), name) != 0)
        return -1;

    char root[COUART_PATH_MAX];
    if (couart_runtime_dir(root, sizeof(root)) != 0)
        return -1;
    if (couart_ensure_dir(root, 0700) != 0 ||
        couart_ensure_dir(h->instance_dir, 0700) != 0) {
        COUART_ERROR("mkdir %s: %s", h->instance_dir, strerror(errno));
        return -1;
    }

    int wake[2];
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) < 0)
        return -1;
    h->wake_rd = wake[0];
    h->wake_wr = wake[1];

    h->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (h->epfd < 0)
        goto fail;
    if (add_epoll(h, h->wake_rd, TAG_WAKE) != 0)
        goto fail;

    if (couart_uart_open(&h->uart, port, baud) != 0) {
        COUART_ERROR("open %s @ %d: %s", port, baud, strerror(errno));
        goto fail;
    }
    h->uart_online = 1;
    if (add_epoll(h, h->uart.fd, TAG_UART) != 0)
        goto fail;

    for (int i = 0; i < COUART_SEAT_COUNT; i++) {
        char link[COUART_PATH_MAX];
        if (couart_seat_path(link, sizeof(link), name, k_roles[i].role) != 0)
            goto fail;
        if (i == 0 && couart_seats_adopt(h->seats, name) == 0) {
            /* All three seats reused; still need epoll on each master. */
        } else if (h->seats[i].master_fd < 0) {
            if (couart_seat_open(&h->seats[i], link, k_roles[i].role, k_roles[i].writable) != 0)
                goto fail;
        }
        h->seats[i].role = k_roles[i].role;
        h->seats[i].writable = k_roles[i].writable;
        if (add_epoll(h, h->seats[i].master_fd, TAG_SEAT0 + i) != 0)
            goto fail;
        COUART_INFO("seat %s -> %s (%s)", k_roles[i].role, link,
                    k_roles[i].writable ? "rw" : "ro");
    }

    if (bind_control(h) != 0) {
        COUART_ERROR("control socket: %s", strerror(errno));
        goto fail;
    }

    COUART_INFO("hub %s holding %s @ %d", name, port, baud);
    return 0;

fail:
    couart_hub_fini(h);
    return -1;
}

void couart_hub_request_stop(couart_hub_t *h)
{
    h->stop = 1;
    if (h->wake_wr >= 0) {
        char x = 1;
        (void)write(h->wake_wr, &x, 1);
    }
}

int couart_hub_run(couart_hub_t *h)
{
    struct epoll_event evs[32];
    while (!h->stop) {
        int n = epoll_wait(h->epfd, evs, 32, 1000);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        for (int i = 0; i < n; i++) {
            uint32_t tag = evs[i].data.u32;
            if (tag == TAG_WAKE) {
                char dump[16];
                while (read(h->wake_rd, dump, sizeof(dump)) > 0)
                    ;
            } else if (tag == TAG_UART) {
                on_uart(h);
            } else if (tag == TAG_LISTEN) {
                accept_ctl(h);
            } else if (tag >= TAG_SEAT0 && tag < TAG_SEAT0 + COUART_SEAT_COUNT) {
                on_seat(h, (int)(tag - TAG_SEAT0));
            } else if (tag >= TAG_CTL0 && tag < TAG_CTL0 + COUART_CTL_MAX) {
                on_ctl(h, (int)(tag - TAG_CTL0));
            }
        }
        tx_drain(h);
        tx_lock_expire(h);
        for (int i = 0; i < COUART_SEAT_COUNT; i++)
            couart_seat_keep_raw(&h->seats[i]);
        /* EPOLLET can swallow a real unplug after a phantom HUP. Poll the node. */
        if (!h->stop && !h->suspended && h->uart_online &&
            !uart_node_present(&h->uart))
            uart_go_offline(h, "device node gone");
        if (!h->stop && !h->suspended && !h->uart_online)
            (void)uart_go_online(h, 1);
    }
    return 0;
}

void couart_hub_fini(couart_hub_t *h)
{
    if (!h)
        return;
    for (int i = 0; i < COUART_CTL_MAX; i++)
        close_ctl(h, i);
    if (h->listen_fd >= 0) {
        close(h->listen_fd);
        h->listen_fd = -1;
    }
    unlink(h->control_path);
    announce(h, "\r\n[couart] hub stopping\r\n");
    /* Close copper before fork so the park child cannot keep TIOCEXCL. */
    if (h->uart.fd >= 0) {
        del_epoll(h, h->uart.fd);
        couart_uart_close(&h->uart);
    }
    int parked = couart_seats_park(h->seats, h->name);
    if (parked == 1) {
        for (int i = 0; i < COUART_SEAT_COUNT; i++)
            couart_seat_drop_fds(&h->seats[i]);
    } else {
        for (int i = 0; i < COUART_SEAT_COUNT; i++)
            couart_seat_close(&h->seats[i]);
    }
    if (h->wake_rd >= 0)
        close(h->wake_rd);
    if (h->wake_wr >= 0)
        close(h->wake_wr);
    if (h->epfd >= 0)
        close(h->epfd);
    h->wake_rd = h->wake_wr = h->epfd = -1;
    if (parked != 1)
        rmdir(h->instance_dir);
}
