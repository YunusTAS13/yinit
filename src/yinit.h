#ifndef YINIT_H
#define YINIT_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/prctl.h>
#include <sys/sysmacros.h>
#include <sys/select.h>
#include <fcntl.h>
#include <linux/reboot.h>

#define YINIT_VERSION       "1.1.0"
#define YINIT_DIR           "/etc/yinit"
#define YINIT_SERVICE_DIR   "/etc/yinit/services"
#define YINIT_STATE_DIR     "/run/yinit"
#define YINIT_SOCKET        "/run/yinit/control.sock"
#define YINIT_LOG           "/var/log/yinit.log"
#define YINIT_CGROUP_ROOT   "/sys/fs/cgroup"
#define MAX_SERVICES        256
#define MAX_DEPS            32
#define MAX_LINE            1024
#define MAX_NAME            128
#define MAX_CMD             2048
#define MAX_ENV             4096
#define MAX_EXECS           16

static inline void copy_str(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    size_t len = src ? strlen(src) : 0;
    if (len >= dst_size) len = dst_size - 1;
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

typedef enum {
    SVC_INACTIVE, SVC_STARTING, SVC_ACTIVE, SVC_STOPPING, SVC_FAILED
} svc_state_t;

typedef enum {
    TYPE_SIMPLE, TYPE_FORKING, TYPE_NOTIFY, TYPE_ONESHOT, TYPE_IDLE
} svc_type_t;

typedef enum {
    RESTART_NO, RESTART_ALWAYS, RESTART_ON_FAILURE, RESTART_ON_ABORT
} restart_mode_t;

typedef struct {
    char name[MAX_NAME];
    int is_requires;
} dep_t;

typedef struct {
    char name[MAX_NAME];
    char desc[256];
    char exec[MAX_EXECS][MAX_CMD];
    int exec_count;
    char exec_pre[MAX_CMD];
    char exec_post[MAX_CMD];
    char exec_stop[MAX_CMD];
    char workdir[512];
    char user[128];
    char env[MAX_ENV];
    char cgroup[256];
    svc_type_t type;
    restart_mode_t restart;
    int restart_sec;
    int restart_max;
    int timeout_sec;
    int nice_val;
    long mem_limit;
    svc_state_t state;
    pid_t pid;
    int exit_code;
    int restarts;
    time_t started_at;
    time_t next_restart_at;
    dep_t deps[MAX_DEPS];
    int dep_count;
} svc_t;

static inline int streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static inline int strhas(const char *s, const char *p) {
    return strstr(s, p) != NULL;
}

static inline char *strtrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

static inline int starts(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

#endif
