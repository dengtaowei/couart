#include "couart.h"
#include "hex.h"
#include "test_main.h"

#include <fcntl.h>
#include <pthread.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static couart_hub_t g_hub;

static void *hub_thread(void *arg)
{
    (void)arg;
    couart_hub_run(&g_hub);
    return NULL;
}

static ssize_t read_wait(int fd, char *buf, size_t n, int tries)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    for (int i = 0; i < tries; i++) {
        ssize_t r = read(fd, buf, n);
        if (r > 0)
            return r;
        usleep(20000);
    }
    return -1;
}

static void stop_hub(pthread_t tid, int master, int slave)
{
    couart_hub_request_stop(&g_hub);
    pthread_join(tid, NULL);
    couart_hub_fini(&g_hub);
    close(master);
    close(slave);
}

static void test_roundtrip(void)
{
    int master, slave;
    ASSERT(openpty(&master, &slave, NULL, NULL, NULL) == 0, "openpty device");
    char *slave_name = ttyname(slave);
    ASSERT(slave_name != NULL, "ttyname");

    char name[64];
    snprintf(name, sizeof(name), "t%d", (int)getpid());

    ASSERT_INT_EQ(couart_hub_init(&g_hub, name, slave_name, 115200), 0);

    pthread_t tid;
    ASSERT_INT_EQ(pthread_create(&tid, NULL, hub_thread, NULL), 0);
    usleep(80000);

    char reply[256];
    ASSERT_INT_EQ(couart_ctl_request(name, "PING", reply, sizeof(reply), 1000), 0);
    ASSERT(strncmp(reply, "OK", 2) == 0, "ping");

    char console_path[COUART_PATH_MAX];
    ASSERT_INT_EQ(couart_seat_path(console_path, sizeof(console_path), name, "console"), 0);
    int cons = open(console_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    ASSERT(cons >= 0, "open console seat");

    ASSERT_INT_EQ((int)write(cons, "from-term\n", 10), 10);
    char buf[256];
    ssize_t n = read_wait(master, buf, sizeof(buf), 50);
    ASSERT(n >= 10, "device got seat TX");
    ASSERT(memcmp(buf, "from-term\n", 10) == 0, "TX payload");

    ASSERT_INT_EQ((int)write(master, "from-dev\n", 9), 9);
    n = read_wait(cons, buf, sizeof(buf), 50);
    ASSERT(n >= 9, "seat got UART RX");
    ASSERT(memcmp(buf, "from-dev\n", 9) == 0, "RX payload");

    close(cons);
    stop_hub(tid, master, slave);
}

static void test_echo_filter(void)
{
    couart_seat_t s;
    memset(&s, 0, sizeof(s));
    int m, sl;
    ASSERT(openpty(&m, &sl, NULL, NULL, NULL) == 0, "openpty");
    s.master_fd = m;
    s.slave_fd = sl;

    ASSERT(couart_seat_write(&s, (const uint8_t *)"help\nmd.b 0x0 0x20\n", 19) > 0,
           "note output");
    uint8_t loop[32];
    memcpy(loop, "help\nmd.b 0x0 0x20\nbase\n", 24);
    size_t n = couart_seat_filter_tx(&s, loop, 24);
    ASSERT_INT_EQ((int)n, 5);
    ASSERT(memcmp(loop, "base\n", 5) == 0, "real TX survives");

    uint8_t typed[] = {'x'};
    n = couart_seat_filter_tx(&s, typed, 1);
    ASSERT_INT_EQ((int)n, 1);
    ASSERT_INT_EQ(typed[0], 'x');

    close(m);
    close(sl);
}

static void test_cr_not_eaten_when_raw(void)
{
    couart_seat_t s;
    memset(&s, 0, sizeof(s));
    int m, sl;
    ASSERT(openpty(&m, &sl, NULL, NULL, NULL) == 0, "openpty");
    s.master_fd = m;
    s.slave_fd = sl;
    couart_seat_keep_raw(&s);

    ASSERT(couart_seat_write(&s, (const uint8_t *)"\r\nboot> ", 8) > 0, "rx to seat");
    uint8_t cr[] = {'\r'};
    size_t n = couart_seat_filter_tx(&s, cr, 1);
    ASSERT_INT_EQ((int)n, 1);
    ASSERT_INT_EQ(cr[0], '\r');

    cr[0] = '\r';
    n = couart_seat_filter_tx(&s, cr, 1);
    ASSERT_INT_EQ((int)n, 1);

    close(m);
    close(sl);
}

static void test_write_not_mirrored(void)
{
    int master, slave;
    ASSERT(openpty(&master, &slave, NULL, NULL, NULL) == 0, "openpty device");
    char *slave_name = ttyname(slave);
    ASSERT(slave_name != NULL, "ttyname");

    char name[64];
    snprintf(name, sizeof(name), "w%d", (int)getpid());
    ASSERT_INT_EQ(couart_hub_init(&g_hub, name, slave_name, 115200), 0);
    pthread_t tid;
    ASSERT_INT_EQ(pthread_create(&tid, NULL, hub_thread, NULL), 0);
    usleep(80000);

    char console_path[COUART_PATH_MAX];
    ASSERT_INT_EQ(couart_seat_path(console_path, sizeof(console_path), name, "console"), 0);
    int cons = open(console_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    ASSERT(cons >= 0, "open console");

    const char *payload = "via-ctl\n";
    char hex[32];
    ASSERT_INT_EQ(couart_hex_encode((const uint8_t *)payload, 8, hex, sizeof(hex)), 0);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "WRITE %s", hex);
    char reply[128];
    ASSERT_INT_EQ(couart_ctl_request(name, cmd, reply, sizeof(reply), 1000), 0);
    ASSERT(strncmp(reply, "OK", 2) == 0, "write ok");

    char buf[256];
    ssize_t n = read_wait(master, buf, sizeof(buf), 50);
    ASSERT(n >= 8, "device got WRITE");
    ASSERT(memcmp(buf, payload, 8) == 0, "WRITE payload");

    n = read(cons, buf, sizeof(buf));
    ASSERT(n <= 0, "console must not see mirrored TX");

    close(cons);
    stop_hub(tid, master, slave);
}

static void test_foreign_openers(void)
{
    int m, sl;
    ASSERT(openpty(&m, &sl, NULL, NULL, NULL) == 0, "openpty");
    char *name = ttyname(sl);
    ASSERT(name != NULL, "ttyname");
    /* skip init so this process's slave fd is counted */
    ASSERT(couart_pts_foreign_openers(name, 1) >= 1, "slave is open");
    int extra = open(name, O_RDWR | O_NOCTTY);
    ASSERT(extra >= 0, "open slave extra");
    ASSERT(couart_pts_foreign_openers(name, 1) >= 2, "counts extra opener");
    close(extra);
    close(m);
    close(sl);
}

static void test_rebind(void)
{
    int m1, s1, m2, s2;
    ASSERT(openpty(&m1, &s1, NULL, NULL, NULL) == 0, "openpty 1");
    ASSERT(openpty(&m2, &s2, NULL, NULL, NULL) == 0, "openpty 2");
    char *n1 = ttyname(s1);
    char *n2 = ttyname(s2);
    ASSERT(n1 && n2, "ttyname");
    char path1[128], path2[128];
    snprintf(path1, sizeof(path1), "%s", n1);
    snprintf(path2, sizeof(path2), "%s", n2);
    /* Hub takes its own slave open; extra fds would only confuse TIOCEXCL. */
    close(s1);
    close(s2);
    s1 = s2 = -1;

    char name[64];
    snprintf(name, sizeof(name), "r%d", (int)getpid());
    ASSERT_INT_EQ(couart_hub_init(&g_hub, name, path1, 115200), 0);
    pthread_t tid;
    ASSERT_INT_EQ(pthread_create(&tid, NULL, hub_thread, NULL), 0);
    usleep(80000);

    char cmd[256], reply[512];
    snprintf(cmd, sizeof(cmd), "PORT %s 9600", path2);
    ASSERT_INT_EQ(couart_ctl_request(name, cmd, reply, sizeof(reply), 2000), 0);
    ASSERT(strncmp(reply, "OK", 2) == 0, "port ok");
    ASSERT(strstr(reply, "baud=9600") != NULL, "baud updated");

    ASSERT_INT_EQ(couart_ctl_request(name, "STATUS", reply, sizeof(reply), 1000), 0);
    ASSERT(strstr(reply, path2) != NULL, "status shows new port");

    const char *payload = "after-bind\n";
    char hex[32];
    ASSERT_INT_EQ(couart_hex_encode((const uint8_t *)payload, 11, hex, sizeof(hex)), 0);
    snprintf(cmd, sizeof(cmd), "WRITE %s", hex);
    ASSERT_INT_EQ(couart_ctl_request(name, cmd, reply, sizeof(reply), 1000), 0);
    ASSERT(strncmp(reply, "OK", 2) == 0, "write ok");

    char buf[256];
    ssize_t n = read_wait(m2, buf, sizeof(buf), 50);
    ASSERT(n >= 11, "new device got TX");
    ASSERT(memcmp(buf, payload, 11) == 0, "TX on rebound uart");

    n = read(m1, buf, sizeof(buf));
    ASSERT(n <= 0, "old device must not see TX after rebind");

    stop_hub(tid, m1, -1);
    close(m2);
}

int main(void)
{
    printf("test_hub\n");
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_echo_filter);
    RUN_TEST(test_cr_not_eaten_when_raw);
    RUN_TEST(test_write_not_mirrored);
    RUN_TEST(test_foreign_openers);
    RUN_TEST(test_rebind);
    TEST_REPORT();
}
