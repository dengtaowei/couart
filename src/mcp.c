#include "couart.h"
#include "hex.h"
#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef COUART_VERSION
#define COUART_VERSION "0.1.0"
#endif

static char g_name[COUART_NAME_MAX];

static void mcp_write_json(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s)
        return;
    fwrite(s, 1, strlen(s), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(s);
}

static void mcp_send_result(cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddNullToObject(resp, "id");
    cJSON_AddItemToObject(resp, "result", result);
    mcp_write_json(resp);
    cJSON_Delete(resp);
}

static void mcp_send_error(cJSON *id, int code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddNullToObject(resp, "id");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    mcp_write_json(resp);
    cJSON_Delete(resp);
}

static void mcp_send_text(cJSON *id, const char *text)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    mcp_send_result(id, result);
}

static int ctl(const char *cmd, char *reply, size_t n, int timeout_ms)
{
    return couart_ctl_request(g_name, cmd, reply, n, timeout_ms);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int history_snap(char *out, size_t out_n, size_t want, uint64_t *seq)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "HISTORY %zu", want);
    char *reply = malloc(want * 2 + 96);
    if (!reply)
        return -1;
    if (ctl(cmd, reply, want * 2 + 96, 2000) != 0) {
        free(reply);
        return -1;
    }
    if (seq) {
        char *sp = strstr(reply, "seq=");
        *seq = sp ? strtoull(sp + 4, NULL, 10) : 0;
    }
    char *nl = strchr(reply, '\n');
    const char *hex = nl ? nl + 1 : "";
    while (*hex == '\n' || *hex == '\r')
        hex++;
    char *end = strchr(hex, '\n');
    if (end)
        *end = '\0';
    uint8_t *raw = malloc(want + 1);
    size_t n = 0;
    if (!raw || couart_hex_decode(hex, raw, want, &n) != 0) {
        free(reply);
        free(raw);
        return -1;
    }
    if (n >= out_n)
        n = out_n - 1;
    memcpy(out, raw, n);
    out[n] = '\0';
    free(reply);
    free(raw);
    return (int)n;
}

static int history_text(char *out, size_t out_n, size_t want)
{
    return history_snap(out, out_n, want, NULL);
}

static int tx_lock(int timeout_ms)
{
    double t0 = now_ms();
    for (;;) {
        char reply[64];
        if (ctl("TXLOCK", reply, sizeof(reply), 1000) == 0 &&
            strncmp(reply, "OK", 2) == 0)
            return 0;
        if (now_ms() - t0 >= (double)timeout_ms)
            return -1;
        usleep(30000);
    }
}

static void tx_unlock(void)
{
    char reply[64];
    (void)ctl("TXUNLOCK", reply, sizeof(reply), 1000);
}

static int write_bytes(const uint8_t *data, size_t n)
{
    char *hex = malloc(n * 2 + 1);
    if (!hex || couart_hex_encode(data, n, hex, n * 2 + 1) != 0) {
        free(hex);
        return -1;
    }
    char *cmd = malloc(n * 2 + 8);
    if (!cmd) {
        free(hex);
        return -1;
    }
    snprintf(cmd, n * 2 + 8, "WRITE %s", hex);
    char reply[128];
    int rc = ctl(cmd, reply, sizeof(reply), 1500);
    free(hex);
    free(cmd);
    if (rc != 0 || strncmp(reply, "OK", 2) != 0)
        return -1;
    return 0;
}

static int regex_match(const char *text, const char *pattern)
{
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0)
        return -1;
    int rc = regexec(&re, text, 0, NULL, 0);
    regfree(&re);
    return rc == 0 ? 1 : 0;
}

static char *list_ports_text(void)
{
    glob_t g;
    memset(&g, 0, sizeof(g));
    size_t n = 0;
    char *out = calloc(1, 1);
    const char *pats[] = { "/dev/ttyUSB*", "/dev/ttyACM*", NULL };
    int flags = 0;
    int any = 0;
    for (int i = 0; pats[i]; i++) {
        if (glob(pats[i], flags, NULL, &g) == 0) {
            flags |= GLOB_APPEND;
            any = 1;
        }
    }
    if (!any) {
        free(out);
        return strdup("(no serial ports)");
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        size_t add = strlen(g.gl_pathv[i]) + 2;
        char *tmp = realloc(out, n + add);
        if (!tmp)
            break;
        out = tmp;
        n += (size_t)snprintf(out + n, add, "%s\n", g.gl_pathv[i]);
    }
    globfree(&g);
    if (n == 0) {
        free(out);
        return strdup("(no serial ports)");
    }
    return out;
}

static void handle_initialize(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "couart");
    cJSON_AddStringToObject(info, "version", COUART_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);
    mcp_send_result(id, result);
}

static cJSON *make_tool(const char *name, const char *desc, cJSON *props, cJSON *required)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "description", desc);
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", props ? props : cJSON_CreateObject());
    if (required)
        cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    return tool;
}

static void handle_tools_list(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();

    cJSON_AddItemToArray(tools, make_tool(
        "serial_port_status",
        "Status of the couart hub: port, baud, seats, online/suspended.",
        cJSON_CreateObject(), NULL));

    cJSON *p_cmd = cJSON_CreateObject();
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "string");
    cJSON_AddItemToObject(p_cmd, "command", s);
    s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "string");
    cJSON_AddItemToObject(p_cmd, "expect_pattern", s);
    s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "integer");
    cJSON_AddItemToObject(p_cmd, "timeout_ms", s);
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("command"));
    cJSON_AddItemToArray(tools, make_tool(
        "serial_send_command",
        "Write a command (newline appended if missing) and wait for expect_pattern (default: boot>).",
        p_cmd, req));

    cJSON *p_w = cJSON_CreateObject();
    s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "string");
    cJSON_AddItemToObject(p_w, "data", s);
    req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("data"));
    cJSON_AddItemToArray(tools, make_tool(
        "serial_write", "Write raw bytes without waiting.", p_w, req));

    cJSON_AddItemToArray(tools, make_tool(
        "serial_read", "Read recent UART output from the hub history.",
        cJSON_CreateObject(), NULL));

    cJSON *p_h = cJSON_CreateObject();
    s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "integer");
    cJSON_AddItemToObject(p_h, "last_bytes", s);
    cJSON_AddItemToArray(tools, make_tool(
        "serial_output_history",
        "Last N bytes of muxed UART traffic (commands + device output).",
        p_h, NULL));

    cJSON *p_wf = cJSON_CreateObject();
    s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "string");
    cJSON_AddItemToObject(p_wf, "pattern", s);
    s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "integer");
    cJSON_AddItemToObject(p_wf, "timeout_ms", s);
    req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("pattern"));
    cJSON_AddItemToArray(tools, make_tool(
        "serial_wait_for", "Wait for a regex in UART output. No TX.", p_wf, req));

    cJSON_AddItemToArray(tools, make_tool(
        "serial_suspend", "Release the physical UART for a flasher.",
        cJSON_CreateObject(), NULL));
    cJSON_AddItemToArray(tools, make_tool(
        "serial_resume", "Reclaim the physical UART after suspend.",
        cJSON_CreateObject(), NULL));
    cJSON_AddItemToArray(tools, make_tool(
        "serial_list_ports", "List /dev/ttyUSB* and /dev/ttyACM* nodes.",
        cJSON_CreateObject(), NULL));

    cJSON *p_bind = cJSON_CreateObject();
    s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "string");
    cJSON_AddStringToObject(s, "description",
                            "Device node or /dev/serial/by-id symlink");
    cJSON_AddItemToObject(p_bind, "device", s);
    s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "integer");
    cJSON_AddStringToObject(s, "description",
                            "Baud rate; omit to keep the current rate");
    cJSON_AddItemToObject(p_bind, "baud", s);
    req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("device"));
    cJSON_AddItemToArray(tools, make_tool(
        "serial_bind_port",
        "Rebind this named hub to a new /dev/ttyUSB* (or by-id) and optional baud. Seats stay up.",
        p_bind, req));

    cJSON_AddItemToObject(result, "tools", tools);
    mcp_send_result(id, result);
}

static const char *arg_str(cJSON *args, const char *key)
{
    if (!args)
        return NULL;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int arg_int(cJSON *args, const char *key, int def)
{
    if (!args)
        return def;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static char *tool_send_command(cJSON *args)
{
    const char *command = arg_str(args, "command");
    if (!command)
        return strdup("ERR missing command");
    const char *pat = arg_str(args, "expect_pattern");
    if (!pat || !pat[0])
        pat = "boot>";
    int timeout_ms = arg_int(args, "timeout_ms", 5000);
    if (timeout_ms < 100)
        timeout_ms = 100;
    if (timeout_ms > 120000)
        timeout_ms = 120000;

    if (tx_lock(timeout_ms) != 0)
        return strdup("ERR tx busy (another command in flight)");

    uint64_t seq0 = 0;
    char snap[COUART_HIST_SIZE + 1];
    if (history_snap(snap, sizeof(snap), 256, &seq0) < 0) {
        tx_unlock();
        return strdup("ERR hub not reachable — is couart attached?");
    }

    size_t clen = strlen(command);
    while (clen > 0 && (command[clen - 1] == '\n' || command[clen - 1] == '\r'))
        clen--;
    size_t n = clen + 1;
    uint8_t *raw = malloc(n);
    if (!raw) {
        tx_unlock();
        return strdup("ERR oom");
    }
    memcpy(raw, command, clen);
    raw[clen] = '\r';
    if (write_bytes(raw, n) != 0) {
        free(raw);
        tx_unlock();
        return strdup("ERR write failed (uart offline?)");
    }
    free(raw);

    char after[COUART_HIST_SIZE + 1];
    double t0 = now_ms();
    char *result = NULL;
    for (;;) {
        uint64_t seq1 = 0;
        int an = history_snap(after, sizeof(after), 16384, &seq1);
        if (an >= 0) {
            size_t neu = 0;
            if (seq1 > seq0) {
                neu = (size_t)(seq1 - seq0);
                if (neu > (size_t)an)
                    neu = (size_t)an;
            }
            const char *suffix = after + ((size_t)an - neu);
            int m = regex_match(suffix, pat);
            if (m == 1) {
                result = strdup(suffix);
                break;
            }
            if (m < 0) {
                result = strdup("ERR invalid expect_pattern regex");
                break;
            }
        }
        if (now_ms() - t0 >= (double)timeout_ms) {
            result = malloc((size_t)(an > 0 ? an : 0) + 64);
            if (!result)
                result = strdup("ERR timeout");
            else
                snprintf(result, (size_t)(an > 0 ? an : 0) + 64,
                         "(timeout waiting for /%s/)\n%s",
                         pat, an > 0 ? after : "");
            break;
        }
        usleep(50000);
    }
    tx_unlock();
    return result;
}

static char *tool_wait_for(cJSON *args)
{
    const char *pat = arg_str(args, "pattern");
    if (!pat)
        return strdup("ERR missing pattern");
    int timeout_ms = arg_int(args, "timeout_ms", 30000);
    char after[COUART_HIST_SIZE + 1];
    double t0 = now_ms();
    for (;;) {
        int an = history_text(after, sizeof(after), 16384);
        if (an >= 0) {
            int m = regex_match(after, pat);
            if (m == 1)
                return strdup(after);
            if (m < 0)
                return strdup("ERR invalid regex");
        }
        if (now_ms() - t0 >= (double)timeout_ms)
            return strdup("(timeout)");
        usleep(80000);
    }
}

static char *dispatch_tool(const char *name, cJSON *args)
{
    char reply[4096];
    if (strcmp(name, "serial_port_status") == 0) {
        if (ctl("STATUS", reply, sizeof(reply), 1500) != 0)
            return strdup("ERR hub not reachable");
        return strdup(reply);
    }
    if (strcmp(name, "serial_send_command") == 0)
        return tool_send_command(args);
    if (strcmp(name, "serial_write") == 0) {
        const char *data = arg_str(args, "data");
        if (!data)
            return strdup("ERR missing data");
        if (write_bytes((const uint8_t *)data, strlen(data)) != 0)
            return strdup("ERR write failed");
        return strdup("OK");
    }
    if (strcmp(name, "serial_read") == 0 || strcmp(name, "serial_output_history") == 0) {
        int last = arg_int(args, "last_bytes", 4096);
        if (last <= 0)
            last = 4096;
        if (last > (int)COUART_HIST_SIZE)
            last = (int)COUART_HIST_SIZE;
        char *buf = malloc((size_t)last + 1);
        if (!buf)
            return strdup("ERR oom");
        int n = history_text(buf, (size_t)last + 1, (size_t)last);
        if (n < 0) {
            free(buf);
            return strdup("ERR hub not reachable");
        }
        return buf;
    }
    if (strcmp(name, "serial_wait_for") == 0)
        return tool_wait_for(args);
    if (strcmp(name, "serial_suspend") == 0) {
        if (ctl("SUSPEND", reply, sizeof(reply), 1500) != 0)
            return strdup("ERR hub not reachable");
        return strdup(reply);
    }
    if (strcmp(name, "serial_resume") == 0) {
        if (ctl("RESUME", reply, sizeof(reply), 1500) != 0)
            return strdup("ERR hub not reachable");
        return strdup(reply);
    }
    if (strcmp(name, "serial_list_ports") == 0)
        return list_ports_text();
    if (strcmp(name, "serial_bind_port") == 0) {
        const char *dev = arg_str(args, "device");
        if (!dev || !dev[0])
            return strdup("ERR missing device");
        int baud = arg_int(args, "baud", 0);
        char cmd[COUART_PATH_MAX + 32];
        if (baud > 0)
            snprintf(cmd, sizeof(cmd), "PORT %s %d", dev, baud);
        else
            snprintf(cmd, sizeof(cmd), "PORT %s", dev);
        if (ctl(cmd, reply, sizeof(reply), 3000) != 0)
            return strdup("ERR hub not reachable");
        return strdup(reply);
    }
    return NULL;
}

static void dispatch(cJSON *msg)
{
    cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (!cJSON_IsString(method)) {
        if (id)
            mcp_send_error(id, -32600, "missing method");
        return;
    }
    const char *m = method->valuestring;
    if (strcmp(m, "initialize") == 0) {
        handle_initialize(id);
    } else if (strcmp(m, "notifications/initialized") == 0) {
        return;
    } else if (strcmp(m, "tools/list") == 0) {
        handle_tools_list(id);
    } else if (strcmp(m, "tools/call") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        cJSON *tname = params ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
        cJSON *args = params ? cJSON_GetObjectItemCaseSensitive(params, "arguments") : NULL;
        if (!cJSON_IsString(tname)) {
            mcp_send_error(id, -32602, "missing tool name");
            return;
        }
        char *text = dispatch_tool(tname->valuestring, args);
        if (!text)
            mcp_send_error(id, -32601, "unknown tool");
        else {
            mcp_send_text(id, text);
            free(text);
        }
    } else if (strcmp(m, "ping") == 0) {
        mcp_send_result(id, cJSON_CreateObject());
    } else if (id) {
        mcp_send_error(id, -32601, "method not found");
    }
}

static int first_instance(char *out, size_t n)
{
    char root[COUART_PATH_MAX];
    if (couart_runtime_dir(root, sizeof(root)) != 0)
        return -1;
    DIR *d = opendir(root);
    if (!d)
        return -1;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || !couart_valid_name(de->d_name))
            continue;
        snprintf(out, n, "%s", de->d_name);
        found = 1;
        break;
    }
    closedir(d);
    return found ? 0 : -1;
}

int couart_mcp(int argc, char **argv)
{
    const char *name = getenv("COUART_INSTANCE");
    static const struct option opts[] = {
        {"name", required_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "n:h", opts, NULL)) != -1) {
        switch (c) {
        case 'n': name = optarg; break;
        case 'h':
            fprintf(stderr, "usage: couart mcp [--name INSTANCE]\n");
            return 0;
        default:
            return 2;
        }
    }
    if (name && name[0]) {
        if (!couart_valid_name(name)) {
            fprintf(stderr, "couart mcp: invalid name\n");
            return 2;
        }
        snprintf(g_name, sizeof(g_name), "%s", name);
    } else if (first_instance(g_name, sizeof(g_name)) != 0) {
        fprintf(stderr, "couart mcp: no instance (pass --name or attach first)\n");
        return 1;
    }

    char *stdin_buf = malloc(8192);
    size_t cap = 8192, len = 0;
    if (!stdin_buf)
        return 1;

    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= 1 << 20) {
                len = 0;
                continue;
            }
            cap *= 2;
            void *t = realloc(stdin_buf, cap);
            if (!t)
                break;
            stdin_buf = t;
        }
        ssize_t n = read(STDIN_FILENO, stdin_buf + len, cap - len - 1);
        if (n <= 0)
            break;
        len += (size_t)n;
        for (;;) {
            char *nl = memchr(stdin_buf, '\n', len);
            if (!nl)
                break;
            *nl = '\0';
            cJSON *msg = cJSON_Parse(stdin_buf);
            if (msg) {
                dispatch(msg);
                cJSON_Delete(msg);
            }
            size_t rest = len - (size_t)(nl + 1 - stdin_buf);
            memmove(stdin_buf, nl + 1, rest);
            len = rest;
        }
    }
    free(stdin_buf);
    return 0;
}
