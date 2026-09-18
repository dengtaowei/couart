#ifndef COUART_H
#define COUART_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define COUART_NAME_MAX 64
#define COUART_PATH_MAX 256
#define COUART_SEAT_COUNT 3
#define COUART_CTL_MAX 8
#define COUART_HIST_SIZE 65536
#define COUART_CTL_LINE_MAX 8192
#define COUART_TXQ_MAX 64
#define COUART_TXFRAME 4096
#define COUART_TXLOCK_S 30.0

enum couart_seat_id {
    COUART_SEAT_CONSOLE = 0,
    COUART_SEAT_AGENT = 1,
    COUART_SEAT_WATCH = 2
};

typedef struct couart_uart {
    int fd;
    int baud;
    char path[COUART_PATH_MAX];
    char by_id[COUART_PATH_MAX];
} couart_uart_t;

typedef struct couart_seat {
    const char *role;
    int writable;
    int master_fd;
    int slave_fd;
    int link_owned;
    char pts_name[128];
    char link_path[COUART_PATH_MAX];
    /* Bytes we wrote to the master (UART RX / banners). A PTY with ECHO
     * will loop them back; filter_tx strips that before it hits the UART. */
    uint8_t echo[COUART_TXFRAME];
    size_t echo_len;
    /* Seat keystrokes deferred while an agent TXLOCK is held. */
    uint8_t hold[COUART_TXFRAME];
    size_t hold_len;
} couart_seat_t;

typedef struct couart_client {
    int fd;
    size_t in_len;
    char in[COUART_CTL_LINE_MAX];
} couart_client_t;

typedef struct couart_txf {
    uint8_t data[COUART_TXFRAME];
    size_t len;
    size_t off;
    int skip_seat;
} couart_txf_t;

typedef struct couart_hub {
    char name[COUART_NAME_MAX];
    char instance_dir[COUART_PATH_MAX];
    char control_path[COUART_PATH_MAX];
    couart_uart_t uart;
    couart_seat_t seats[COUART_SEAT_COUNT];
    couart_client_t ctl[COUART_CTL_MAX];
    uint8_t hist[COUART_HIST_SIZE];
    size_t hist_head;
    size_t hist_len;
    uint64_t hist_seq;
    couart_txf_t txq[COUART_TXQ_MAX];
    int txq_rd;
    int txq_n;
    int tx_locked;
    double tx_lock_until;
    int epfd;
    int listen_fd;
    int wake_rd;
    int wake_wr;
    int suspended;
    int stop;
    int uart_online;
} couart_hub_t;

/* log */
void couart_log(const char *level, const char *fmt, ...);
#define COUART_INFO(...)  couart_log("INFO",  __VA_ARGS__)
#define COUART_WARN(...)  couart_log("WARN",  __VA_ARGS__)
#define COUART_ERROR(...) couart_log("ERROR", __VA_ARGS__)

/* paths */
int couart_valid_name(const char *name);
int couart_runtime_dir(char *out, size_t n);
int couart_instance_dir(char *out, size_t n, const char *name);
int couart_control_path(char *out, size_t n, const char *name);
int couart_pid_path(char *out, size_t n, const char *name);
int couart_seat_path(char *out, size_t n, const char *name, const char *role);
int couart_park_path(char *out, size_t n, const char *name);
int couart_ensure_dir(const char *path, mode_t mode);

/* uart */
int  couart_uart_open(couart_uart_t *u, const char *path, int baud);
void couart_uart_close(couart_uart_t *u);
int  couart_uart_write(couart_uart_t *u, const uint8_t *data, size_t len);
int  couart_uart_fd_current(const couart_uart_t *u);
int  couart_uart_valid_baud(int baud);
int  couart_uart_same_node(const couart_uart_t *u, const char *path);
int  couart_uart_set_baud(couart_uart_t *u, int baud);

/* seats */
int  couart_seat_open(couart_seat_t *s, const char *link_path,
                      const char *role, int writable);
void couart_seat_close(couart_seat_t *s);
void couart_seat_drop_fds(couart_seat_t *s);
int  couart_seat_write(couart_seat_t *s, const uint8_t *data, size_t len);
size_t couart_seat_filter_tx(couart_seat_t *s, uint8_t *data, size_t len);
void couart_seat_keep_raw(couart_seat_t *s);
int  couart_pts_foreign_openers(const char *pts_name, pid_t skip);
int  couart_seats_adopt(couart_seat_t seats[COUART_SEAT_COUNT], const char *name);
int  couart_seats_park(couart_seat_t seats[COUART_SEAT_COUNT], const char *name);

/* hub */
int  couart_hub_init(couart_hub_t *h, const char *name, const char *port, int baud);
int  couart_hub_run(couart_hub_t *h);
void couart_hub_request_stop(couart_hub_t *h);
void couart_hub_fini(couart_hub_t *h);

/* control socket helpers used by CLI */
int  couart_ctl_request(const char *name, const char *cmd,
                        char *reply, size_t reply_n, int timeout_ms);

int couart_mcp(int argc, char **argv);
int couart_cli(int argc, char **argv);
int couart_serve(int argc, char **argv);

#endif /* COUART_H */
