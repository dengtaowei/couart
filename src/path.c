#include "couart.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int couart_valid_name(const char *name)
{
    if (!name || !name[0] || strlen(name) >= COUART_NAME_MAX)
        return 0;
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

int couart_runtime_dir(char *out, size_t n)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0])
        return snprintf(out, n, "%s/couart", xdg) < (int)n ? 0 : -1;
    return snprintf(out, n, "/tmp/couart-%d", (int)getuid()) < (int)n ? 0 : -1;
}

int couart_instance_dir(char *out, size_t n, const char *name)
{
    char root[COUART_PATH_MAX];
    if (!couart_valid_name(name) || couart_runtime_dir(root, sizeof(root)) != 0)
        return -1;
    return snprintf(out, n, "%s/%s", root, name) < (int)n ? 0 : -1;
}

int couart_control_path(char *out, size_t n, const char *name)
{
    char dir[COUART_PATH_MAX];
    if (couart_instance_dir(dir, sizeof(dir), name) != 0)
        return -1;
    return snprintf(out, n, "%s/control.sock", dir) < (int)n ? 0 : -1;
}

int couart_pid_path(char *out, size_t n, const char *name)
{
    char dir[COUART_PATH_MAX];
    if (couart_instance_dir(dir, sizeof(dir), name) != 0)
        return -1;
    return snprintf(out, n, "%s/pid", dir) < (int)n ? 0 : -1;
}

int couart_seat_path(char *out, size_t n, const char *name, const char *role)
{
    char dir[COUART_PATH_MAX];
    if (!role || couart_instance_dir(dir, sizeof(dir), name) != 0)
        return -1;
    return snprintf(out, n, "%s/%s", dir, role) < (int)n ? 0 : -1;
}

int couart_park_path(char *out, size_t n, const char *name)
{
    char dir[COUART_PATH_MAX];
    if (couart_instance_dir(dir, sizeof(dir), name) != 0)
        return -1;
    return snprintf(out, n, "%s/park.sock", dir) < (int)n ? 0 : -1;
}

int couart_ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
    }
    return -1;
}
