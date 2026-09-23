#include "yinit.h"

static svc_t services[MAX_SERVICES];
static int svc_count;
static mount_record_t mounts[MAX_MOUNTS];
static int mount_count;
static int cgroup_available;
static char cgroup_root[PATH_MAX] = YINIT_CGROUP_ROOT;
static int log_fd = -1;
static int ctrl_fd = -1;
static volatile sig_atomic_t do_shutdown;
static volatile sig_atomic_t do_reload;
static const char *service_dir = YINIT_SERVICE_DIR;

/* ---- Logging ---- */
static void log_msg(const char *level, const char *fmt, ...) {
    char buf[MAX_LINE];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    int n = snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d][%s] ",
        tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
        tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, level);
    if (n < 0 || (size_t)n >= sizeof(buf)) return;

    va_list ap;
    va_start(ap, fmt);
    int rest = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    if (rest < 0) return;
    n += rest;
    if ((size_t)n >= sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';

    if (log_fd >= 0) (void)write(log_fd, buf, (size_t)n);
    if (log_fd < 0 || getpid() == 1) (void)write(STDERR_FILENO, buf, (size_t)n);
}

#define LOG_I(...) log_msg("INFO", __VA_ARGS__)
#define LOG_W(...) log_msg("WARN", __VA_ARGS__)
#define LOG_E(...) log_msg("ERR ", __VA_ARGS__)

/* ---- Small filesystem helpers ---- */
static int is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return -1;
    if (is_directory(path)) return 0;

    char tmp[PATH_MAX];
    if (copy_str(tmp, sizeof(tmp), path) < 0) return -1;
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (*tmp && mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return is_directory(tmp) ? 0 : -1;
}

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int write_text_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = strlen(value);
    ssize_t written = write(fd, value, len);
    int saved = errno;
    close(fd);
    errno = saved;
    return written == (ssize_t)len ? 0 : -1;
}

static int path_component_safe(const char *path) {
    if (!path || !*path || path[0] == '/') return 0;
    if (strstr(path, "//") || strstr(path, "../") || strstr(path, "/..") ||
        strcmp(path, "..") == 0) return 0;
    return 1;
}

static int append_path(char *dst, size_t dst_size, const char *base, const char *suffix) {
    size_t base_len = strlen(base), suffix_len = strlen(suffix);
    if (base_len + suffix_len + 1 > dst_size) return -1;
    memcpy(dst, base, base_len);
    memcpy(dst + base_len, suffix, suffix_len + 1);
    return 0;
}

/* ---- Service parser ---- */
static void normalize_name(char *name) {
    if (has_suffix(name, ".service"))
        name[strlen(name) - strlen(".service")] = '\0';
}

static void init_service(svc_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = SVC_INACTIVE;
    s->type = TYPE_SIMPLE;
    s->restart = RESTART_NO;
    s->restart_sec = 5;
    s->restart_max = 5;
    s->timeout_sec = 90;
    s->pid = -1;
    s->pgid = -1;
}

static void append_environment(svc_t *s, const char *value) {
    if (!value || !*value) return;
    size_t used = strlen(s->env);
    if (used && used + 1 < sizeof(s->env)) s->env[used++] = ' ';
    if (used < sizeof(s->env) - 1)
        copy_str(s->env + used, sizeof(s->env) - used, value);
}

static void parse_svc(const char *path, svc_t *s) {
    init_service(s);

    FILE *file = fopen(path, "r");
    if (!file) {
        LOG_E("Cannot read service file %s: %s", path, strerror(errno));
        return;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), file)) {
        char *p = strtrim(line);
        if (!*p || *p == '#' || *p == '[') continue;

        char *value = strchr(p, '=');
        if (!value) continue;
        *value++ = '\0';
        char *key = strtrim(p);
        value = strtrim(value);

        if (streq(key, "Description")) copy_str(s->desc, sizeof(s->desc), value);
        else if (streq(key, "ExecStartPre")) copy_str(s->exec_pre, sizeof(s->exec_pre), value);
        else if (streq(key, "ExecStartPost")) copy_str(s->exec_post, sizeof(s->exec_post), value);
        else if (streq(key, "ExecStop")) copy_str(s->exec_stop, sizeof(s->exec_stop), value);
        else if (streq(key, "ExecStart")) {
            if (s->exec_count < MAX_EXECS)
                copy_str(s->exec[s->exec_count++], sizeof(s->exec[0]), value);
            else
                LOG_W("Too many ExecStart entries in %s", path);
        } else if (streq(key, "WorkingDirectory")) copy_str(s->workdir, sizeof(s->workdir), value);
        else if (streq(key, "User")) copy_str(s->user, sizeof(s->user), value);
        else if (streq(key, "Environment")) append_environment(s, value);
        else if (streq(key, "CGroup")) copy_str(s->cgroup, sizeof(s->cgroup), value);
        else if (streq(key, "Type")) {
            if (streq(value, "simple")) s->type = TYPE_SIMPLE;
            else if (streq(value, "forking")) s->type = TYPE_FORKING;
            else if (streq(value, "notify")) s->type = TYPE_NOTIFY;
            else if (streq(value, "oneshot")) s->type = TYPE_ONESHOT;
            else if (streq(value, "idle")) s->type = TYPE_IDLE;
            else LOG_W("Unknown service type '%s' in %s", value, path);
        } else if (streq(key, "Restart")) {
            if (streq(value, "always")) s->restart = RESTART_ALWAYS;
            else if (streq(value, "on-failure")) s->restart = RESTART_ON_FAILURE;
            else if (streq(value, "on-abort")) s->restart = RESTART_ON_ABORT;
            else if (streq(value, "no")) s->restart = RESTART_NO;
            else LOG_W("Unknown restart policy '%s' in %s", value, path);
        } else if (streq(key, "RestartSec")) s->restart_sec = atoi(value);
        else if (streq(key, "StartLimitBurst")) s->restart_max = atoi(value);
        else if (streq(key, "TimeoutStartSec")) s->timeout_sec = atoi(value);
        else if (streq(key, "Nice")) s->nice_val = atoi(value);
        else if (streq(key, "MemoryLimit")) s->mem_limit = atol(value);
        else if (streq(key, "TTY")) s->tty = streq(value, "yes") || streq(value, "true");
        else if (streq(key, "After") || streq(key, "Requires") || streq(key, "Wants")) {
            char depbuf[MAX_LINE];
            copy_str(depbuf, sizeof(depbuf), value);
            char *save = NULL;
            for (char *tok = strtok_r(depbuf, " \t", &save);
                 tok && s->dep_count < MAX_DEPS;
                 tok = strtok_r(NULL, " \t", &save)) {
                copy_str(s->deps[s->dep_count].name,
                         sizeof(s->deps[s->dep_count].name), tok);
                normalize_name(s->deps[s->dep_count].name);
                s->deps[s->dep_count].is_requires = streq(key, "Requires");
                s->dep_count++;
            }
        }
    }
    fclose(file);

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    copy_str(s->name, sizeof(s->name), base);
    normalize_name(s->name);
}

static int service_name_cmp(const void *a, const void *b) {
    const svc_t *sa = a, *sb = b;
    return strcmp(sa->name, sb->name);
}

static void load_services(void) {
    svc_count = 0;
    DIR *dir = opendir(service_dir);
    if (!dir) {
        LOG_E("Cannot open %s: %s", service_dir, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) && svc_count < MAX_SERVICES) {
        if (entry->d_name[0] == '.' || !has_suffix(entry->d_name, ".service")) continue;
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", service_dir, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        parse_svc(path, &services[svc_count]);
        svc_count++;
    }
    closedir(dir);
    qsort(services, (size_t)svc_count, sizeof(services[0]), service_name_cmp);
}

static int find_service_index(const char *name) {
    for (int i = 0; i < svc_count; i++)
        if (streq(services[i].name, name)) return i;
    /* Numbered profiles remain readable while yinitctl also accepts the
     * logical suffix, e.g. `status network` for 07-network.service. */
    for (int i = 0; i < svc_count; i++) {
        const char *short_name = services[i].name;
        while (*short_name && isdigit((unsigned char)*short_name)) short_name++;
        if (*short_name == '-') short_name++;
        if (streq(short_name, name)) return i;
    }
    return -1;
}

/* A deterministic dependency sort. Missing optional dependencies are warnings;
 * missing Requires dependencies are left to the start-time dependency check. */
static int visit_service(int index, int marks[], svc_t sorted[], int *sorted_count) {
    if (marks[index] == 2) return 0;
    if (marks[index] == 1) {
        LOG_E("Dependency cycle includes %s", services[index].name);
        return -1;
    }

    marks[index] = 1;
    for (int i = 0; i < services[index].dep_count; i++) {
        int dep = find_service_index(services[index].deps[i].name);
        if (dep < 0) {
            LOG_W("%s references missing dependency %s%s", services[index].name,
                  services[index].deps[i].name,
                  services[index].deps[i].is_requires ? " (required)" : "");
            continue;
        }
        (void)visit_service(dep, marks, sorted, sorted_count);
    }
    marks[index] = 2;
    sorted[(*sorted_count)++] = services[index];
    return 0;
}

static void sort_services(void) {
    static svc_t sorted[MAX_SERVICES];
    int marks[MAX_SERVICES] = {0};
    int sorted_count = 0;
    for (int i = 0; i < svc_count; i++)
        if (marks[i] == 0) (void)visit_service(i, marks, sorted, &sorted_count);
    memcpy(services, sorted, sizeof(svc_t) * (size_t)sorted_count);
    svc_count = sorted_count;
}

/* ---- Mounts and cgroups ---- */
static void record_mount(const char *target) {
    if (mount_count >= MAX_MOUNTS) return;
    copy_str(mounts[mount_count].target, sizeof(mounts[0].target), target);
    mounts[mount_count++].mounted_by_us = 1;
}

static int mount_one(const char *source, const char *target, const char *type,
                     unsigned long flags, const char *data) {
    if (mkdir_p(target, 0755) < 0) {
        LOG_E("Cannot create mount point %s: %s", target, strerror(errno));
        return -1;
    }
    if (mount(source, target, type, flags, data) == 0) {
        record_mount(target);
        LOG_I("Mounted %s on %s", type, target);
        return 0;
    }
    if (errno == EBUSY) return 0; /* already provided by the kernel/initramfs */
    LOG_W("Cannot mount %s on %s: %s", type, target, strerror(errno));
    return -1;
}

static void create_device_nodes(void) {
    static const struct { const char *path; mode_t mode; int major_num; int minor_num; } nodes[] = {
        {"/dev/null",    0666 | S_IFCHR, 1, 3},
        {"/dev/zero",    0666 | S_IFCHR, 1, 5},
        {"/dev/random",  0666 | S_IFCHR, 1, 8},
        {"/dev/urandom", 0666 | S_IFCHR, 1, 9},
        {"/dev/tty",     0666 | S_IFCHR, 5, 0},
        {"/dev/console", 0600 | S_IFCHR, 5, 1},
        {"/dev/ptmx",    0666 | S_IFCHR, 5, 2},
        {NULL, 0, 0, 0}
    };
    for (int i = 0; nodes[i].path; i++) {
        if (access(nodes[i].path, F_OK) == 0) continue;
        if (mknod(nodes[i].path, nodes[i].mode,
                  makedev(nodes[i].major_num, nodes[i].minor_num)) < 0)
            LOG_W("Cannot create %s: %s", nodes[i].path, strerror(errno));
    }
}

static int mount_filesystems(void) {
    int failures = 0;
    if (mount_one("proc", "/proc", "proc", 0, NULL) < 0) failures++;
    if (mount_one("sysfs", "/sys", "sysfs", 0, NULL) < 0) failures++;
    if (mount_one("devtmpfs", "/dev", "devtmpfs", 0, "mode=0755") < 0) failures++;
    if (mount_one("tmpfs", "/run", "tmpfs", 0, "mode=0755,nosuid,nodev") < 0) failures++;
    if (mount_one("tmpfs", "/tmp", "tmpfs", 0, "mode=1777,nosuid,nodev") < 0) failures++;
    if (mount_one("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=0620") < 0) failures++;

    create_device_nodes();
    return failures;
}

static const char *service_cgroup_name(const svc_t *s) {
    return s->cgroup[0] ? s->cgroup : s->name;
}

static int cgroup_path_for(const svc_t *s, char *path, size_t size) {
    const char *rel = service_cgroup_name(s);
    if (!path_component_safe(rel)) return -1;
    int n = snprintf(path, size, "%s/yinit/%s", cgroup_root, rel);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int cgroup_controllers_available(const char *root) {
    char controllers_file[PATH_MAX];
    return append_path(controllers_file, sizeof(controllers_file), root,
                       "/cgroup.controllers") == 0 &&
           access(controllers_file, R_OK) == 0;
}

static void setup_cgroups(void) {
    if (cgroup_controllers_available(YINIT_CGROUP_ROOT)) {
        copy_str(cgroup_root, sizeof(cgroup_root), YINIT_CGROUP_ROOT);
    } else {
        /* Some kernels expose sysfs without a writable cgroup directory.
         * cgroup v2 may be mounted at any writable mount point, so use the
         * already-mounted /run tmpfs as a safe fallback. */
        copy_str(cgroup_root, sizeof(cgroup_root), "/run/yinit/cgroup");
        if (mkdir_p(cgroup_root, 0755) < 0 ||
            mount_one("none", cgroup_root, "cgroup2", 0, NULL) < 0 ||
            !cgroup_controllers_available(cgroup_root)) {
            LOG_W("cgroup v2 is unavailable; process groups will be used for cleanup");
            return;
        }
    }
    char yinit_cgroup_root[PATH_MAX];
    if (append_path(yinit_cgroup_root, sizeof(yinit_cgroup_root), cgroup_root,
                    "/yinit") < 0 ||
        mkdir_p(cgroup_root, 0755) < 0 || mkdir_p(yinit_cgroup_root, 0755) < 0) {
        LOG_W("Cannot create yinit cgroup root: %s", strerror(errno));
        return;
    }
    /* Best effort: controllers may already be owned by another manager. */
    char control_file[PATH_MAX];
    if (append_path(control_file, sizeof(control_file), cgroup_root,
                    "/cgroup.subtree_control") == 0)
        (void)write_text_file(control_file, "+cpu +memory +pids");
    cgroup_available = 1;
    LOG_I("cgroup v2 service isolation enabled at %s", cgroup_root);
}

static int cgroup_create_for(const svc_t *s) {
    if (!cgroup_available) return 0;
    char path[PATH_MAX];
    if (cgroup_path_for(s, path, sizeof(path)) < 0) {
        LOG_E("Unsafe cgroup path for %s", s->name);
        return -1;
    }
    if (mkdir_p(path, 0755) < 0 || !is_directory(path)) {
        LOG_W("Cannot create cgroup for %s: %s", s->name, strerror(errno));
        return -1;
    }
    if (s->mem_limit > 0) {
        char file[PATH_MAX], value[64];
        if (append_path(file, sizeof(file), path, "/memory.max") < 0) return -1;
        snprintf(value, sizeof(value), "%ld", s->mem_limit);
        if (write_text_file(file, value) < 0)
            LOG_W("Cannot set memory limit for %s: %s", s->name, strerror(errno));
    }
    return 0;
}

static void cgroup_attach_for(const svc_t *s, pid_t pid) {
    if (!cgroup_available) return;
    char path[PATH_MAX], file[PATH_MAX], value[32];
    if (cgroup_path_for(s, path, sizeof(path)) < 0) return;
    if (append_path(file, sizeof(file), path, "/cgroup.procs") < 0) return;
    snprintf(value, sizeof(value), "%d", pid);
    if (write_text_file(file, value) < 0)
        LOG_W("Cannot attach %s (PID %d) to cgroup: %s", s->name, pid, strerror(errno));
}

static void cgroup_kill_for(const svc_t *s) {
    if (!cgroup_available) return;
    char path[PATH_MAX], file[PATH_MAX];
    if (cgroup_path_for(s, path, sizeof(path)) < 0) return;
    if (append_path(file, sizeof(file), path, "/cgroup.kill") < 0) return;
    if (write_text_file(file, "1") == 0) return;

    if (append_path(file, sizeof(file), path, "/cgroup.procs") < 0) return;
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &save); tok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        pid_t pid = (pid_t)strtol(tok, NULL, 10);
        if (pid > 1) (void)kill(pid, SIGKILL);
    }
}

static void cgroup_signal_for(const svc_t *s, int sig) {
    if (!cgroup_available) return;
    if (sig == SIGKILL) {
        cgroup_kill_for(s);
        return;
    }

    char path[PATH_MAX], file[PATH_MAX];
    if (cgroup_path_for(s, path, sizeof(path)) < 0) return;
    if (append_path(file, sizeof(file), path, "/cgroup.procs") < 0) return;
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &save); tok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        pid_t pid = (pid_t)strtol(tok, NULL, 10);
        if (pid > 1) (void)kill(pid, sig);
    }
}

static void cgroup_destroy_for(const svc_t *s) {
    if (!cgroup_available) return;
    char path[PATH_MAX];
    if (cgroup_path_for(s, path, sizeof(path)) < 0) return;
    if (rmdir(path) < 0 && errno != ENOENT && errno != EBUSY)
        LOG_W("Cannot remove cgroup for %s: %s", s->name, strerror(errno));
}

/* ---- Process management ---- */
typedef struct {
    char name[128];
    uid_t uid;
    gid_t gid;
} account_record_t;

static int lookup_account(const char *requested, account_record_t *account) {
    FILE *file = fopen("/etc/passwd", "r");
    if (!file) return -1;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), file)) {
        char *save = NULL;
        char *name = strtok_r(line, ":", &save);
        (void)strtok_r(NULL, ":", &save); /* password */
        char *uid_text = strtok_r(NULL, ":", &save);
        char *gid_text = strtok_r(NULL, ":", &save);
        if (!name || !uid_text || !gid_text) continue;
        char *end_uid = NULL, *end_gid = NULL;
        unsigned long uid = strtoul(uid_text, &end_uid, 10);
        unsigned long gid = strtoul(gid_text, &end_gid, 10);
        if (!end_uid || *end_uid != '\0' || !end_gid || *end_gid != '\0') continue;

        int matches_name = strcmp(requested, name) == 0;
        int matches_uid = 0;
        char *end_requested = NULL;
        errno = 0;
        unsigned long requested_uid = strtoul(requested, &end_requested, 10);
        if (errno == 0 && end_requested && *end_requested == '\0')
            matches_uid = requested_uid == uid;
        if (matches_name || matches_uid) {
            copy_str(account->name, sizeof(account->name), name);
            account->uid = (uid_t)uid;
            account->gid = (gid_t)gid;
            fclose(file);
            return 0;
        }
    }
    fclose(file);
    return -1;
}

static int lookup_supplementary_groups(const char *user, gid_t primary,
                                       gid_t *groups, size_t capacity, size_t *count) {
    *count = 0;
    if (capacity == 0) return -1;
    groups[(*count)++] = primary;

    FILE *file = fopen("/etc/group", "r");
    if (!file) return 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), file) && *count < capacity) {
        char *save = NULL;
        (void)strtok_r(line, ":", &save); /* group name */
        (void)strtok_r(NULL, ":", &save); /* password */
        char *gid_text = strtok_r(NULL, ":", &save);
        char *members = strtok_r(NULL, "\n", &save);
        if (!gid_text || !members) continue;
        char *end_gid = NULL;
        unsigned long gid = strtoul(gid_text, &end_gid, 10);
        if (!end_gid || *end_gid != '\0') continue;

        char member_copy[MAX_LINE];
        copy_str(member_copy, sizeof(member_copy), members);
        char *member_save = NULL;
        for (char *member = strtok_r(member_copy, ",", &member_save);
             member; member = strtok_r(NULL, ",", &member_save)) {
            if (strcmp(strtrim(member), user) == 0) {
                gid_t group = (gid_t)gid;
                int duplicate = 0;
                for (size_t i = 0; i < *count; i++)
                    if (groups[i] == group) duplicate = 1;
                if (!duplicate) groups[(*count)++] = group;
                break;
            }
        }
    }
    fclose(file);
    return 0;
}

static int apply_user(const svc_t *s) {
    if (!s || !s->user[0]) return 0;
    if (geteuid() != 0) {
        LOG_E("Service %s requests User=%s but yinit is not root", s->name, s->user);
        return -1;
    }

    account_record_t account;
    if (lookup_account(s->user, &account) < 0) {
        LOG_E("Unknown service user %s for %s", s->user, s->name);
        return -1;
    }

    gid_t groups[128];
    size_t group_count = 0;
    (void)lookup_supplementary_groups(account.name, account.gid,
                                      groups, sizeof(groups) / sizeof(groups[0]),
                                      &group_count);
    if (setgroups(group_count, groups) < 0 || setgid(account.gid) < 0 ||
        setuid(account.uid) < 0) return -1;
    return 0;
}

static void set_environment(const svc_t *s) {
    if (!s || !s->env[0]) return;
    char envbuf[MAX_ENV];
    copy_str(envbuf, sizeof(envbuf), s->env);
    char *save = NULL;
    for (char *tok = strtok_r(envbuf, " \t\r\n", &save); tok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            (void)setenv(tok, eq + 1, 1);
        }
    }
}

static void exec_command(const char *command) {
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
}

static int spawn_command(const svc_t *s, const char *command, pid_t *pid_out, pid_t *pgid_out) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!s || !s->tty) (void)setpgid(0, 0);
        if (s && s->workdir[0] && chdir(s->workdir) < 0) _exit(126);
        if (s && s->nice_val != 0) (void)nice(s->nice_val);
        if (s) set_environment(s);
        if (s && apply_user(s) < 0) _exit(126);
        (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
        exec_command(command);
        _exit(127);
    }

    if (!s || !s->tty) (void)setpgid(pid, pid);
    *pid_out = pid;
    *pgid_out = (s && s->tty) ? -1 : pid;
    return 0;
}

static int wait_for_pid(pid_t pid, int timeout_sec, int *status) {
    long long deadline = monotonic_ms() + (long long)(timeout_sec > 0 ? timeout_sec : 90) * 1000LL;
    for (;;) {
        pid_t result = waitpid(pid, status, WNOHANG);
        if (result == pid) return 0;
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (monotonic_ms() >= deadline) return 1;
        usleep(10000);
    }
}

static int status_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static void terminate_spawned(pid_t pid, pid_t pgid, int sig) {
    if (pgid > 1) (void)kill(-pgid, sig);
    else if (pid > 1) (void)kill(pid, sig);
}

static int run_sync(const svc_t *s, const char *command) {
    if (!command || !*command) return 0;
    pid_t pid = -1, pgid = -1;
    if (spawn_command(s, command, &pid, &pgid) < 0) return 1;
    if (s && cgroup_available) cgroup_attach_for(s, pid);

    int status = 0;
    int waited = wait_for_pid(pid, s ? s->timeout_sec : 90, &status);
    if (waited == 1) {
        terminate_spawned(pid, pgid, SIGTERM);
        usleep(100000);
        terminate_spawned(pid, pgid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        LOG_E("Command timed out: %s", command);
        return 124;
    }
    if (waited < 0) return 1;
    return status_code(status);
}

static int dependencies_met(const svc_t *s) {
    for (int i = 0; i < s->dep_count; i++) {
        if (!s->deps[i].is_requires) continue;
        int index = find_service_index(s->deps[i].name);
        if (index < 0 || services[index].state != SVC_ACTIVE) return 0;
    }
    return 1;
}

static void signal_service(const svc_t *s, int sig) {
    if (s->pgid > 1) (void)kill(-s->pgid, sig);
    else if (s->pid > 1) (void)kill(s->pid, sig);
}

static void start_service(svc_t *s) {
    if (s->state == SVC_ACTIVE || s->state == SVC_STARTING) return;
    if (s->exec_count == 0) {
        LOG_E("Service %s has no ExecStart", s->name);
        s->state = SVC_FAILED;
        s->start_failed = 1;
        return;
    }
    if (!dependencies_met(s)) {
        LOG_E("Required dependencies are not active for %s", s->name);
        s->state = SVC_FAILED;
        s->start_failed = 1;
        return;
    }

    LOG_I("Starting %s", s->name);
    s->state = SVC_STARTING;
    s->start_failed = 0;
    (void)cgroup_create_for(s);

    if (run_sync(s, s->exec_pre) != 0) {
        LOG_E("ExecStartPre failed for %s", s->name);
        s->state = SVC_FAILED;
        s->start_failed = 1;
        cgroup_destroy_for(s);
        return;
    }

    if (s->type == TYPE_ONESHOT) {
        for (int i = 0; i < s->exec_count; i++) {
            int code = run_sync(s, s->exec[i]);
            if (code != 0) {
                LOG_E("ExecStart failed for %s with code %d", s->name, code);
                s->state = SVC_FAILED;
                s->start_failed = 1;
                cgroup_destroy_for(s);
                return;
            }
        }
        s->state = SVC_ACTIVE; /* RemainAfterExit semantics for dependencies. */
        s->pid = -1;
        s->pgid = -1;
    } else {
        pid_t pid = -1, pgid = -1;
        if (spawn_command(s, s->exec[0], &pid, &pgid) < 0) {
            LOG_E("Cannot fork service %s: %s", s->name, strerror(errno));
            s->state = SVC_FAILED;
            s->start_failed = 1;
            cgroup_destroy_for(s);
            return;
        }
        s->pid = pid;
        s->pgid = pgid;
        s->started_at = time(NULL);
        cgroup_attach_for(s, pid);
        s->state = SVC_ACTIVE;
        if (s->type == TYPE_NOTIFY)
            LOG_W("Type=notify is treated as supervised simple service: %s", s->name);
    }

    if (run_sync(s, s->exec_post) != 0)
        LOG_W("ExecStartPost failed for %s", s->name);
    if (s->type == TYPE_ONESHOT)
        cgroup_destroy_for(s);
    LOG_I("Started %s (PID %d)", s->name, s->pid);
}

static void stop_service(svc_t *s, int force) {
    if (s->state == SVC_INACTIVE) return;
    LOG_I("Stopping %s", s->name);
    s->state = SVC_STOPPING;
    int sig = force ? SIGKILL : SIGTERM;
    signal_service(s, sig);
    if (!force) cgroup_signal_for(s, SIGTERM);

    if (s->pid > 1) {
        int status = 0;
        int waited = wait_for_pid(s->pid, s->timeout_sec, &status);
        if (waited == 1 || waited < 0) {
            signal_service(s, SIGKILL);
            cgroup_kill_for(s);
            (void)waitpid(s->pid, &status, 0);
        }
    }
    s->pid = -1;
    s->pgid = -1;
    (void)run_sync(NULL, s->exec_stop);
    cgroup_kill_for(s);
    cgroup_destroy_for(s);
    s->state = SVC_INACTIVE;
    s->restarts = 0;
    s->next_restart_at = 0;
}

static void reload_service(svc_t *s) {
    if (s->state == SVC_ACTIVE) {
        LOG_I("Reloading %s", s->name);
        signal_service(s, SIGHUP);
    }
}

/* ---- Child reaping and restart policy ---- */
static void reap_children(void) {
    int status;
    for (;;) {
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            if (pid < 0 && errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < svc_count; i++) {
            svc_t *s = &services[i];
            if (s->pid != pid) continue;
            int code = status_code(status);
            s->exit_code = code;
            s->pid = -1;

            if (s->state == SVC_STOPPING) {
                s->state = SVC_INACTIVE;
                break;
            }

            if (s->type == TYPE_FORKING && code == 0) {
                LOG_I("Forking service parent exited: %s", s->name);
                s->state = SVC_ACTIVE;
                break;
            }

            int should_restart = s->restart == RESTART_ALWAYS ||
                (s->restart == RESTART_ON_FAILURE && code != 0) ||
                (s->restart == RESTART_ON_ABORT && WIFSIGNALED(status));
            if (s->state == SVC_ACTIVE && should_restart && s->restarts < s->restart_max) {
                s->restarts++;
                s->next_restart_at = time(NULL) +
                    (s->restart_sec > 0 ? s->restart_sec : 1);
                LOG_W("Scheduled restart for %s in %d seconds (%d/%d)",
                      s->name, s->restart_sec > 0 ? s->restart_sec : 1,
                      s->restarts, s->restart_max);
                s->state = SVC_INACTIVE;
            } else if (s->state == SVC_ACTIVE) {
                s->state = code == 0 ? SVC_INACTIVE : SVC_FAILED;
                if (code == 0) LOG_I("Service exited normally: %s", s->name);
                else LOG_W("Service exited: %s (code %d)", s->name, code);
            }
            break;
        }
    }
}

static void start_due_services(void) {
    time_t now = time(NULL);
    for (int i = 0; i < svc_count; i++) {
        svc_t *s = &services[i];
        if (s->state == SVC_INACTIVE && s->next_restart_at > 0 &&
            s->next_restart_at <= now) {
            s->next_restart_at = 0;
            start_service(s);
        }
    }
}

/* ---- Console and signals ---- */
static void setup_console(void) {
    int fd = open("/dev/console", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    if (fd != STDIN_FILENO) dup2(fd, STDIN_FILENO);
    if (fd != STDOUT_FILENO) dup2(fd, STDOUT_FILENO);
    if (fd != STDERR_FILENO) dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
}

static void on_sigchld(int sig) { (void)sig; }
static void on_shutdown_signal(int sig) { (void)sig; do_shutdown = 1; }
static void on_sighup(int sig) { (void)sig; do_reload = 1; }

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sa.sa_handler = on_sigchld;
    (void)sigaction(SIGCHLD, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = on_shutdown_signal;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGQUIT, &sa, NULL);
    (void)sigaction(SIGPWR, &sa, NULL);

    sa.sa_handler = on_sighup;
    (void)sigaction(SIGHUP, &sa, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGTTIN, SIG_IGN);
    (void)signal(SIGTTOU, SIG_IGN);
    (void)signal(SIGTSTP, SIG_IGN);
}

/* ---- Control socket ---- */
static int setup_control_socket(void) {
    if (mkdir_p(YINIT_STATE_DIR, 0755) < 0) {
        LOG_E("Cannot create %s: %s", YINIT_STATE_DIR, strerror(errno));
        return -1;
    }
    (void)unlink(YINIT_SOCKET);

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    copy_str(address.sun_path, sizeof(address.sun_path), YINIT_SOCKET);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    if (chmod(YINIT_SOCKET, 0600) < 0) LOG_W("Cannot restrict control socket: %s", strerror(errno));
    return fd;
}

static const char *state_string(svc_state_t state) {
    switch (state) {
        case SVC_INACTIVE: return "inactive";
        case SVC_STARTING: return "starting";
        case SVC_ACTIVE: return "active";
        case SVC_STOPPING: return "stopping";
        case SVC_FAILED: return "failed";
    }
    return "unknown";
}

static void send_response(const struct sockaddr_un *client, socklen_t client_len,
                          const char *response) {
    if (response && *response)
        (void)sendto(ctrl_fd, response, strlen(response), 0,
                     (const struct sockaddr *)client, client_len);
}

static void handle_control(void) {
    char buffer[4096];
    struct sockaddr_un client;
    socklen_t client_len = sizeof(client);
    int length = recvfrom(ctrl_fd, buffer, sizeof(buffer) - 1, 0,
                          (struct sockaddr *)&client, &client_len);
    if (length <= 0) return;
    buffer[length] = '\0';

    char *save = NULL;
    char *command = strtok_r(buffer, " \t\r\n", &save);
    char *argument = strtok_r(NULL, " \t\r\n", &save);
    char response[4096] = "";
    if (!command) {
        send_response(&client, client_len, "error: empty command\n");
        return;
    }

    if (streq(command, "status")) {
        if (argument) {
            int index = find_service_index(argument);
            if (index >= 0) {
                svc_t *s = &services[index];
                snprintf(response, sizeof(response), "%s %-10s PID:%d %s\n",
                         s->name, state_string(s->state), s->pid, s->desc);
            } else snprintf(response, sizeof(response), "Unknown service: %s\n", argument);
        } else {
            for (int i = 0; i < svc_count; i++) {
                char line[512];
                snprintf(line, sizeof(line), "%.127s %-10s PID:%d %.255s\n",
                         services[i].name, state_string(services[i].state),
                         services[i].pid, services[i].desc);
                strncat(response, line, sizeof(response) - strlen(response) - 1);
            }
        }
    } else if (streq(command, "start") && argument) {
        int index = find_service_index(argument);
        if (index >= 0) {
            services[index].restarts = 0;
            services[index].next_restart_at = 0;
            start_service(&services[index]);
            snprintf(response, sizeof(response), "OK\n");
        }
        else snprintf(response, sizeof(response), "Unknown service: %s\n", argument);
    } else if (streq(command, "start")) {
        snprintf(response, sizeof(response), "error: service name required\n");
    } else if (streq(command, "stop") && argument) {
        int index = find_service_index(argument);
        if (index >= 0) { stop_service(&services[index], 0); snprintf(response, sizeof(response), "OK\n"); }
        else snprintf(response, sizeof(response), "Unknown service: %s\n", argument);
    } else if (streq(command, "stop")) {
        snprintf(response, sizeof(response), "error: service name required\n");
    } else if (streq(command, "restart") && argument) {
        int index = find_service_index(argument);
        if (index >= 0) { stop_service(&services[index], 0); start_service(&services[index]); snprintf(response, sizeof(response), "OK\n"); }
        else snprintf(response, sizeof(response), "Unknown service: %s\n", argument);
    } else if (streq(command, "restart")) {
        snprintf(response, sizeof(response), "error: service name required\n");
    } else if (streq(command, "reload") && argument) {
        int index = find_service_index(argument);
        if (index >= 0) { reload_service(&services[index]); snprintf(response, sizeof(response), "OK\n"); }
        else snprintf(response, sizeof(response), "Unknown service: %s\n", argument);
    } else if (streq(command, "reload")) {
        snprintf(response, sizeof(response), "error: service name required\n");
    } else if (streq(command, "poweroff")) {
        do_shutdown = 1;
        snprintf(response, sizeof(response), "Power off requested\n");
    } else if (streq(command, "reboot")) {
        do_shutdown = 2;
        snprintf(response, sizeof(response), "Reboot requested\n");
    } else if (streq(command, "version")) {
        snprintf(response, sizeof(response), "Yinit %s\n", YINIT_VERSION);
    } else {
        snprintf(response, sizeof(response), "Unknown command\n");
    }
    send_response(&client, client_len, response);
}

/* ---- Shutdown ---- */
static void stop_all_services(void) {
    for (int i = svc_count - 1; i >= 0; i--)
        if (services[i].state == SVC_ACTIVE || services[i].state == SVC_STARTING)
            stop_service(&services[i], 0);
    for (int i = svc_count - 1; i >= 0; i--)
        if (services[i].state != SVC_INACTIVE)
            stop_service(&services[i], 1);
}

static void terminate_remaining_processes(void) {
    if (getpid() != 1) return;
    (void)kill(-1, SIGTERM); /* PID 1 is excluded by kill(2). */
    for (int i = 0; i < 50; i++) {
        reap_children();
        usleep(100000);
    }
    (void)kill(-1, SIGKILL);
    reap_children();
}

static int shutdown_system(int action) {
    LOG_I("Shutting down (%s)", action == 2 ? "reboot" : "poweroff");
    stop_all_services();
    terminate_remaining_processes();
    sync();

    int command = action == 2 ? LINUX_REBOOT_CMD_RESTART : LINUX_REBOOT_CMD_POWER_OFF;
    if (reboot(command) < 0) {
        LOG_E("reboot(%d) failed: %s", command, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- Diagnostics ---- */
static int check_configuration(const char *requested_dir) {
    if (requested_dir && *requested_dir) service_dir = requested_dir;
    load_services();
    if (svc_count == 0) return 1;
    sort_services();
    for (int i = 0; i < svc_count; i++) {
        if (services[i].exec_count == 0) {
            LOG_E("%s has no ExecStart", services[i].name);
            return 1;
        }
    }
    LOG_I("Configuration OK: %d services", svc_count);
    return 0;
}

/* ---- Main ---- */
int main(int argc, char *argv[]) {
    if (argc > 1 && (streq(argv[1], "--version") || streq(argv[1], "-V"))) {
        printf("Yinit %s\n", YINIT_VERSION);
        return 0;
    }
    if (argc > 1 && streq(argv[1], "--check"))
        return check_configuration(argc > 2 ? argv[2] : NULL);
    if (getpid() != 1) {
        fprintf(stderr, "Yinit refuses to run outside PID 1; use --check for validation.\n");
        return 2;
    }

    umask(022);
    (void)mkdir_p("/var/log", 0755);
    log_fd = open(YINIT_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    (void)mount_filesystems();
    setup_console();
    LOG_I("Yinit %s starting (PID %d)", YINIT_VERSION, getpid());
    setup_signals();
    (void)prctl(PR_SET_CHILD_SUBREAPER, 1);
    setup_cgroups();
    ctrl_fd = setup_control_socket();

    load_services();
    sort_services();
    LOG_I("Loaded %d services", svc_count);
    for (int i = 0; i < svc_count; i++) start_service(&services[i]);
    LOG_I("Main loop");

    for (;;) {
        reap_children();
        start_due_services();
        if (do_shutdown) {
            int action = do_shutdown;
            do_shutdown = 0;
            if (shutdown_system(action) == 0) {
                /* reboot(2) should not return.  If a platform does return
                 * success, PID 1 must remain alive rather than exiting. */
                for (;;) pause();
            }
        }
        if (do_reload) {
            do_reload = 0;
            for (int i = 0; i < svc_count; i++) reload_service(&services[i]);
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;
        if (ctrl_fd >= 0) {
            FD_SET(ctrl_fd, &readfds);
            maxfd = ctrl_fd;
        }
        struct timeval timeout = {0, 100000};
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
        if (ready > 0 && ctrl_fd >= 0 && FD_ISSET(ctrl_fd, &readfds)) handle_control();
    }

    if (ctrl_fd >= 0) {
        close(ctrl_fd);
        (void)unlink(YINIT_SOCKET);
    }
    if (log_fd >= 0) close(log_fd);
    return 0;
}
