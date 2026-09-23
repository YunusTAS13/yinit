#ifndef YINIT_H
#define YINIT_H

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* These constants are part of the Linux reboot(2) ABI.  Keeping the small
 * definitions here lets the project build with a standalone musl toolchain
 * that does not ship a copy of the host's Linux headers. */
#ifndef LINUX_REBOOT_CMD_RESTART
#define LINUX_REBOOT_CMD_RESTART   0x01234567
#endif
#ifndef LINUX_REBOOT_CMD_POWER_OFF
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC
#endif

#define YINIT_VERSION       "1.2"
#define YINIT_DIR           "/etc/yinit"
#define YINIT_SERVICE_DIR   "/etc/yinit/services"
#define YINIT_STATE_DIR     "/run/yinit"
#define YINIT_SOCKET        "/run/yinit/control.sock"
#define YINIT_LOG           "/var/log/yinit.log"
#define YINIT_CGROUP_ROOT   "/sys/fs/cgroup"

#define MAX_SERVICES        256
#define MAX_DEPS            32
#define MAX_LINE            4096
#define MAX_NAME            128
#define MAX_CMD             4096
#define MAX_ENV             8192
#define MAX_EXECS           32
#define MAX_MOUNTS          16

typedef enum {
    SVC_INACTIVE,
    SVC_STARTING,
    SVC_ACTIVE,
    SVC_STOPPING,
    SVC_FAILED
} svc_state_t;

typedef enum {
    TYPE_SIMPLE,
    TYPE_FORKING,
    TYPE_NOTIFY,
    TYPE_ONESHOT,
    TYPE_IDLE
} svc_type_t;

typedef enum {
    RESTART_NO,
    RESTART_ALWAYS,
    RESTART_ON_FAILURE,
    RESTART_ON_ABORT
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
    char workdir[PATH_MAX];
    char user[128];
    char env[MAX_ENV];
    char cgroup[PATH_MAX];
    svc_type_t type;
    restart_mode_t restart;
    int restart_sec;
    int restart_max;
    int timeout_sec;
    int nice_val;
    long mem_limit;
    int tty;
    svc_state_t state;
    pid_t pid;
    pid_t pgid;
    int exit_code;
    int restarts;
    int start_failed;
    time_t started_at;
    time_t next_restart_at;
    dep_t deps[MAX_DEPS];
    int dep_count;
} svc_t;

typedef struct {
    char target[PATH_MAX];
    int mounted_by_us;
} mount_record_t;

static inline int streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static inline int has_suffix(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t slen = strlen(s), plen = strlen(suffix);
    return slen >= plen && strcmp(s + slen - plen, suffix) == 0;
}

static inline char *strtrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

static inline int copy_str(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0 || !src) return -1;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}

#endif
