#include "couart.h"
#include "hex.h"

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void usage(FILE *fp)
{
    fprintf(fp,
        "couart — one UART, many collaborators\n"
        "\n"
        "USAGE:\n"
        "  couart attach <device> [--name NAME] [--baud RATE] [--foreground]\n"
        "  couart port <name> [<device>] [--baud RATE]\n"
        "  couart history <name> [--last BYTES]\n"
        "  couart detach <name>\n"
        "  couart list\n"
        "  couart status <name>\n"
        "  couart seats <name>\n"
        "  couart suspend <name>\n"
        "  couart resume <name>\n"
        "  couart mcp [--name NAME]\n"
        "  couart version\n"
        "\n"
        "Seats published under $XDG_RUNTIME_DIR/couart/<name>/:\n"
        "  console   read/write   WindTerm, minicom, picocom\n"
        "  agent     read/write   scripts / AI tools\n"
        "  watch     read-only    logs and observers\n"
        "\n"
        "Each seat is a normal PTY. Open it like a serial port.\n"
        "couart mcp speaks MCP JSON-RPC on stdio for AI agents.\n");
}

static const char *default_name(const char *device, char *buf, size_t n)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", device);
    const char *base = basename(tmp);
    if (strncmp(base, "tty", 3) == 0)
        base += 3;
    snprintf(buf, n, "%s", base);
    for (char *p = buf; *p; p++) {
        if (*p == '.' || *p == '/')
            *p = '-';
    }
    return buf;
}

static int wait_ready(const char *name, int timeout_ms)
{
    char reply[64];
    int slept = 0;
    while (slept < timeout_ms) {
        if (couart_ctl_request(name, "PING", reply, sizeof(reply), 200) == 0 &&
            strncmp(reply, "OK", 2) == 0)
            return 0;
        usleep(50000);
        slept += 50;
    }
    return -1;
}

static int print_status(const char *name)
{
    char reply[2048];
    if (couart_ctl_request(name, "STATUS", reply, sizeof(reply), 1000) != 0) {
        fprintf(stderr, "couart: instance '%s' is not running\n", name);
        return 1;
    }
    fputs(reply, stdout);
    return 0;
}

static int cmd_list(void)
{
    char root[COUART_PATH_MAX];
    if (couart_runtime_dir(root, sizeof(root)) != 0)
        return 1;
    DIR *d = opendir(root);
    if (!d) {
        printf("(no instances)\n");
        return 0;
    }
    int found = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        if (!couart_valid_name(de->d_name))
            continue;
        char reply[2048];
        if (couart_ctl_request(de->d_name, "STATUS", reply, sizeof(reply), 300) != 0)
            continue;
        found++;
        char name[COUART_NAME_MAX] = "";
        char port[COUART_PATH_MAX] = "";
        char baud[16] = "";
        char *line = reply;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = '\0';
            if (strncmp(line, "name=", 5) == 0)
                snprintf(name, sizeof(name), "%s", line + 5);
            else if (strncmp(line, "port=", 5) == 0)
                snprintf(port, sizeof(port), "%s", line + 5);
            else if (strncmp(line, "baud=", 5) == 0)
                snprintf(baud, sizeof(baud), "%s", line + 5);
            line = nl ? nl + 1 : NULL;
        }
        printf("%-16s %-28s %s\n", name[0] ? name : de->d_name, port, baud);
    }
    closedir(d);
    if (!found)
        printf("(no instances)\n");
    return 0;
}

static int spawn_serve(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return -1;
    return 0;
}

static int self_path(char *out, size_t n)
{
    ssize_t r = readlink("/proc/self/exe", out, n - 1);
    if (r < 0)
        return -1;
    out[r] = '\0';
    return 0;
}

static int cmd_attach(int argc, char **argv)
{
    const char *device = NULL;
    const char *name = NULL;
    int baud = 115200;
    int foreground = 0;

    static const struct option opts[] = {
        {"name", required_argument, NULL, 'n'},
        {"baud", required_argument, NULL, 'b'},
        {"foreground", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "n:b:fh", opts, NULL)) != -1) {
        switch (c) {
        case 'n': name = optarg; break;
        case 'b': baud = atoi(optarg); break;
        case 'f': foreground = 1; break;
        case 'h': usage(stdout); return 0;
        default: return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "couart attach: missing device\n");
        return 2;
    }
    device = argv[optind];

    char namebuf[COUART_NAME_MAX];
    if (!name)
        name = default_name(device, namebuf, sizeof(namebuf));
    if (!couart_valid_name(name)) {
        fprintf(stderr, "couart: invalid name '%s'\n", name);
        return 2;
    }

    char reply[64];
    if (couart_ctl_request(name, "PING", reply, sizeof(reply), 200) == 0) {
        fprintf(stderr, "couart: instance '%s' already running\n", name);
        fprintf(stderr, "rebind seats in place with:\n");
        fprintf(stderr, "  couart port %s %s --baud %d\n", name, device, baud);
        return print_status(name);
    }

    char self[PATH_MAX];
    if (self_path(self, sizeof(self)) != 0) {
        perror("readlink /proc/self/exe");
        return 1;
    }
    char baud_s[16];
    snprintf(baud_s, sizeof(baud_s), "%d", baud);

    if (foreground) {
        char *srv[] = {self, "serve", "--name", (char *)name, "--port", (char *)device,
                       "--baud", baud_s, "--foreground", NULL};
        execv(self, srv);
        perror("exec serve");
        return 1;
    }

    char *srv[] = {self, "serve", "--name", (char *)name, "--port", (char *)device,
                   "--baud", baud_s, NULL};
    if (spawn_serve(srv) != 0) {
        fprintf(stderr, "couart: failed to start hub (is %s free?)\n", device);
        return 1;
    }
    if (wait_ready(name, 3000) != 0) {
        fprintf(stderr, "couart: hub started but control socket never appeared\n");
        return 1;
    }

    printf("attached %s as '%s'\n", device, name);
    print_status(name);
    printf("\nOpen in WindTerm (Serial):\n");
    char seat[COUART_PATH_MAX];
    if (couart_seat_path(seat, sizeof(seat), name, "console") == 0)
        printf("  %s\n", seat);
    return 0;
}

static int status_field(const char *reply, const char *key, char *out, size_t n)
{
    size_t klen = strlen(key);
    const char *line = reply;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len > klen && strncmp(line, key, klen) == 0) {
            size_t vlen = len - klen;
            if (vlen >= n)
                vlen = n - 1;
            memcpy(out, line + klen, vlen);
            out[vlen] = '\0';
            return 0;
        }
        line = nl ? nl + 1 : NULL;
    }
    return -1;
}

static int cmd_port(int argc, char **argv)
{
    int baud = 0;
    int baud_set = 0;
    static const struct option opts[] = {
        {"baud", required_argument, NULL, 'b'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "b:h", opts, NULL)) != -1) {
        switch (c) {
        case 'b':
            baud = atoi(optarg);
            baud_set = 1;
            break;
        case 'h':
            fprintf(stdout,
                    "usage: couart port <name> [<device>] [--baud RATE]\n"
                    "  Rebind a running instance to a new /dev/ttyUSB* (or by-id)\n"
                    "  and/or baud. WindTerm seats stay connected.\n"
                    "  Omit device to keep the current node; omit --baud to keep\n"
                    "  the current rate.\n");
            return 0;
        default:
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "couart port: missing instance name\n");
        return 2;
    }
    const char *name = argv[optind++];
    const char *device = (optind < argc) ? argv[optind] : NULL;

    if (!device && !baud_set) {
        fprintf(stderr, "couart port: need a device and/or --baud\n");
        return 2;
    }
    if (!couart_valid_name(name)) {
        fprintf(stderr, "couart: invalid name '%s'\n", name);
        return 2;
    }
    if (baud_set && !couart_uart_valid_baud(baud)) {
        fprintf(stderr, "couart: unsupported baud %d\n", baud);
        return 2;
    }

    char cur_port[COUART_PATH_MAX] = "";
    if (!device) {
        char st[2048];
        if (couart_ctl_request(name, "STATUS", st, sizeof(st), 1000) != 0) {
            fprintf(stderr, "couart: instance '%s' is not running\n", name);
            return 1;
        }
        if (status_field(st, "port=", cur_port, sizeof(cur_port)) != 0 ||
            !cur_port[0] || strcmp(cur_port, "-") == 0) {
            fprintf(stderr, "couart: instance '%s' has no current port; pass a device\n",
                    name);
            return 1;
        }
        device = cur_port;
    }

    char cmd[COUART_PATH_MAX + 32];
    if (baud_set)
        snprintf(cmd, sizeof(cmd), "PORT %s %d", device, baud);
    else
        snprintf(cmd, sizeof(cmd), "PORT %s", device);

    char reply[2048];
    if (couart_ctl_request(name, cmd, reply, sizeof(reply), 3000) != 0) {
        fprintf(stderr, "couart: instance '%s' is not running\n", name);
        return 1;
    }
    fputs(reply, stdout);
    return strncmp(reply, "OK", 2) == 0 ? 0 : 1;
}

static int cmd_history(int argc, char **argv)
{
    int last = 8192;
    static const struct option opts[] = {
        {"last", required_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "l:h", opts, NULL)) != -1) {
        switch (c) {
        case 'l':
            last = atoi(optarg);
            break;
        case 'h':
            fprintf(stdout,
                    "usage: couart history <name> [--last BYTES]\n"
                    "  Print recent console TX + UART RX from the hub ring buffer.\n"
                    "  Default --last is 8192, max %d.\n",
                    (int)COUART_HIST_SIZE);
            return 0;
        default:
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "couart history: missing instance name\n");
        return 2;
    }
    const char *name = argv[optind];
    if (!couart_valid_name(name)) {
        fprintf(stderr, "couart: invalid name '%s'\n", name);
        return 2;
    }
    if (last <= 0 || last > (int)COUART_HIST_SIZE) {
        fprintf(stderr, "couart history: --last must be 1..%d\n",
                (int)COUART_HIST_SIZE);
        return 2;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "HISTORY %d", last);
    size_t reply_n = (size_t)last * 2 + 128;
    char *reply = malloc(reply_n);
    if (!reply)
        return 1;
    if (couart_ctl_request(name, cmd, reply, reply_n, 2000) != 0) {
        free(reply);
        fprintf(stderr, "couart: instance '%s' is not running\n", name);
        return 1;
    }
    if (strncmp(reply, "OK", 2) != 0) {
        fputs(reply, stderr);
        free(reply);
        return 1;
    }

    char *nl = strchr(reply, '\n');
    const char *hex = nl ? nl + 1 : "";
    while (*hex == '\n' || *hex == '\r')
        hex++;
    char *end = strchr(hex, '\n');
    if (end)
        *end = '\0';

    uint8_t *raw = malloc((size_t)last + 1);
    size_t n = 0;
    if (!raw || couart_hex_decode(hex, raw, (size_t)last, &n) != 0) {
        free(raw);
        free(reply);
        fprintf(stderr, "couart history: bad hub reply\n");
        return 1;
    }
    if (n > 0)
        fwrite(raw, 1, n, stdout);
    free(raw);
    free(reply);
    return 0;
}

static int cmd_ctl(const char *name, const char *cmd)
{
    if (!couart_valid_name(name)) {
        fprintf(stderr, "couart: invalid name '%s'\n", name);
        return 2;
    }
    char reply[2048];
    if (couart_ctl_request(name, cmd, reply, sizeof(reply), 1500) != 0) {
        fprintf(stderr, "couart: instance '%s' is not running\n", name);
        return 1;
    }
    fputs(reply, stdout);
    return strncmp(reply, "OK", 2) == 0 ? 0 : 1;
}

int couart_cli(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 ||
        strcmp(cmd, "-V") == 0) {
#ifdef COUART_VERSION
        printf("couart %s\n", COUART_VERSION);
#else
        printf("couart\n");
#endif
        return 0;
    }
    if (strcmp(cmd, "list") == 0)
        return cmd_list();
    if (strcmp(cmd, "attach") == 0)
        return cmd_attach(argc - 1, argv + 1);
    if (strcmp(cmd, "port") == 0 || strcmp(cmd, "bind") == 0)
        return cmd_port(argc - 1, argv + 1);
    if (strcmp(cmd, "history") == 0 || strcmp(cmd, "hist") == 0)
        return cmd_history(argc - 1, argv + 1);
    if (argc < 3) {
        fprintf(stderr, "couart %s: missing instance name\n", cmd);
        return 2;
    }
    const char *name = argv[2];
    if (strcmp(cmd, "detach") == 0)
        return cmd_ctl(name, "SHUTDOWN");
    if (strcmp(cmd, "status") == 0)
        return print_status(name);
    if (strcmp(cmd, "seats") == 0)
        return print_status(name);
    if (strcmp(cmd, "suspend") == 0)
        return cmd_ctl(name, "SUSPEND");
    if (strcmp(cmd, "resume") == 0)
        return cmd_ctl(name, "RESUME");

    fprintf(stderr, "couart: unknown command '%s'\n", cmd);
    usage(stderr);
    return 2;
}
