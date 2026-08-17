#include "yinit.h"

static svc_t services[MAX_SERVICES];
static int svc_count = 0;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t do_shutdown = 0;
static volatile sig_atomic_t do_reload = 0;
static int log_fd = -1;
static int ctrl_fd = -1;

/* ---- Logging ---- */
static void log_msg(const char *lvl, const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list ap;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int n = snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d][%s] ",
        tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec, lvl);
    va_start(ap, fmt);
    n += vsnprintf(buf+n, sizeof(buf)-n, fmt, ap);
    va_end(ap);
    buf[n++] = '\n';
    if (log_fd >= 0) write(log_fd, buf, n);
    if (getpid() == 1) write(STDERR_FILENO, buf, n);
}
#define LOG_I(...) log_msg("INFO", __VA_ARGS__)
#define LOG_W(...) log_msg("WARN", __VA_ARGS__)
#define LOG_E(...) log_msg("ERR ", __VA_ARGS__)

/* ---- Service file parser ---- */
static void parse_svc(const char *path, svc_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = SVC_INACTIVE;
    s->type = TYPE_SIMPLE;
    s->restart = RESTART_NO;
    s->restart_sec = 10;
    s->restart_max = 5;
    s->timeout_sec = 90;
    s->pid = -1;

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *p = strtrim(line);
        if (*p == '\0' || *p == '#' || *p == '[') continue;
        char *val = strchr(p, '=');
        if (!val) continue;
        *val++ = '\0';
        char *k = strtrim(p);
        char *v = strtrim(val);

        if (streq(k, "Description"))        strncpy(s->desc, v, 255);
        else if (streq(k, "ExecStartPre"))  strncpy(s->exec_pre, v, MAX_CMD-1);
        else if (streq(k, "ExecStartPost")) strncpy(s->exec_post, v, MAX_CMD-1);
        else if (streq(k, "ExecStop"))      strncpy(s->exec_stop, v, MAX_CMD-1);
        else if (streq(k, "ExecStart")) {
            if (s->exec_count < MAX_EXECS)
                strncpy(s->exec[s->exec_count++], v, MAX_CMD-1);
        }
        else if (streq(k, "WorkingDirectory")) strncpy(s->workdir, v, 511);
        else if (streq(k, "User")) strncpy(s->user, v, 127);
        else if (streq(k, "Environment")) {
            if (s->env[0]) strncat(s->env, " ", MAX_ENV-strlen(s->env)-1);
            strncat(s->env, v, MAX_ENV-strlen(s->env)-1);
        }
        else if (streq(k, "CGroup")) strncpy(s->cgroup, v, 255);
        else if (streq(k, "Type")) {
            if (streq(v, "simple"))  s->type = TYPE_SIMPLE;
            if (streq(v, "forking")) s->type = TYPE_FORKING;
            if (streq(v, "notify"))  s->type = TYPE_NOTIFY;
            if (streq(v, "oneshot")) s->type = TYPE_ONESHOT;
            if (streq(v, "idle"))    s->type = TYPE_IDLE;
        }
        else if (streq(k, "Restart")) {
            if (streq(v, "always"))     s->restart = RESTART_ALWAYS;
            if (streq(v, "on-failure")) s->restart = RESTART_ON_FAILURE;
            if (streq(v, "on-abort"))   s->restart = RESTART_ON_ABORT;
        }
        else if (streq(k, "RestartSec"))      s->restart_sec = atoi(v);
        else if (streq(k, "StartLimitBurst")) s->restart_max = atoi(v);
        else if (streq(k, "TimeoutStartSec")) s->timeout_sec = atoi(v);
        else if (streq(k, "Nice"))            s->nice_val = atoi(v);
        else if (streq(k, "MemoryLimit"))     s->mem_limit = atol(v);
        else if (streq(k, "After") || streq(k, "Requires") || streq(k, "Wants")) {
            char tmp[MAX_LINE];
            strncpy(tmp, v, MAX_LINE-1);
            char *tok = strtok(tmp, " ");
            while (tok && s->dep_count < MAX_DEPS) {
                strncpy(s->deps[s->dep_count].name, tok, MAX_NAME-1);
                s->deps[s->dep_count].is_requires = streq(k, "Requires");
                s->dep_count++;
                tok = strtok(NULL, " ");
            }
        }
    }
    fclose(f);

    /* Extract name from path */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(s->name, base, MAX_NAME-1);
    char *dot = strrchr(s->name, '.');
    if (dot) *dot = '\0';
}

static void load_services(void) {
    DIR *d = opendir(YINIT_SERVICE_DIR);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) && svc_count < MAX_SERVICES) {
        if (ent->d_name[0] == '.') continue;
        if (!strhas(ent->d_name, ".service")) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", YINIT_SERVICE_DIR, ent->d_name);
        parse_svc(path, &services[svc_count]);
        LOG_I("Loaded: %s - %s", services[svc_count].name, services[svc_count].desc);
        svc_count++;
    }
    closedir(d);
}

static svc_t *find_svc(const char *name) {
    for (int i = 0; i < svc_count; i++)
        if (streq(services[i].name, name)) return &services[i];
    return NULL;
}

/* ---- Topological sort ---- */
static void sort_services(void) {
    static svc_t sorted[MAX_SERVICES];
    int sorted_count = 0;
    int used[MAX_SERVICES] = {0};

    for (int pass = 0; pass < svc_count; pass++) {
        for (int i = 0; i < svc_count; i++) {
            if (used[i]) continue;
            int deps_met = 1;
            for (int j = 0; j < services[i].dep_count; j++) {
                for (int k = 0; k < sorted_count; k++) {
                    if (streq(services[i].deps[j].name, sorted[k].name))
                        goto dep_found;
                }
                deps_met = 0;
                break;
                dep_found:;
            }
            if (deps_met || pass == svc_count - 1) {
                sorted[sorted_count++] = services[i];
                used[i] = 1;
            }
        }
    }
    memcpy(services, sorted, sizeof(svc_t) * sorted_count);
    svc_count = sorted_count;
}

/* ---- cgroup helpers ---- */
static void cgroup_create(const char *path) {
    if (!path[0]) return;
    char p[512];
    mkdir(YINIT_CGROUP_ROOT "/yinit", 0755);
    snprintf(p, sizeof(p), "%s/yinit/%s", YINIT_CGROUP_ROOT, path);
    mkdir(p, 0755);
}

static void cgroup_attach(const char *path, pid_t pid) {
    if (!path[0]) return;
    char p[512], buf[16];
    snprintf(p, sizeof(p), "%s/yinit/%s/cgroup.procs", YINIT_CGROUP_ROOT, path);
    snprintf(buf, sizeof(buf), "%d", pid);
    int fd = open(p, O_WRONLY);
    if (fd >= 0) { write(fd, buf, strlen(buf)); close(fd); }
}

static void cgroup_set_mem(const char *path, long limit) {
    if (!path[0] || limit <= 0) return;
    char p[512], buf[32];
    snprintf(p, sizeof(p), "%s/yinit/%s/memory.max", YINIT_CGROUP_ROOT, path);
    snprintf(buf, sizeof(buf), "%ld", limit);
    int fd = open(p, O_WRONLY);
    if (fd >= 0) { write(fd, buf, strlen(buf)); close(fd); }
}

static void cgroup_destroy(const char *path) {
    if (!path[0]) return;
    char p[512];
    snprintf(p, sizeof(p), "%s/yinit/%s", YINIT_CGROUP_ROOT, path);
    rmdir(p);
}

/* ---- Process management ---- */
static void exec_cmd(const char *cmd) {
    char *argv[] = {"/bin/sh", "-c", (char*)cmd, NULL};
    execv("/bin/sh", argv);
}

static void set_env(svc_t *s) {
    if (s->env[0]) {
        char tmp[MAX_ENV];
        strncpy(tmp, s->env, MAX_ENV-1);
        char *tok = strtok(tmp, " ");
        while (tok) {
            char *eq = strchr(tok, '=');
            if (eq) { *eq = '\0'; setenv(tok, eq+1, 1); }
            tok = strtok(NULL, " ");
        }
    }
}

static void run_script(const char *cmd, svc_t *s) {
    if (!cmd[0]) return;
    pid_t p = fork();
    if (p == 0) {
        if (s && s->workdir[0]) chdir(s->workdir);
        if (s) set_env(s);
        exec_cmd(cmd);
        _exit(127);
    } else if (p > 0) {
        waitpid(p, NULL, 0);
    }
}

static int check_deps(svc_t *s) {
    for (int i = 0; i < s->dep_count; i++) {
        if (s->deps[i].is_requires) {
            svc_t *dep = find_svc(s->deps[i].name);
            if (!dep || dep->state != SVC_ACTIVE) return -1;
        }
    }
    return 0;
}

static void start_svc(svc_t *s) {
    if (s->state == SVC_ACTIVE || s->state == SVC_STARTING) return;
    if (!s->exec[0][0]) { s->state = SVC_INACTIVE; return; }
    if (check_deps(s) < 0) {
        LOG_E("Dependencies not met: %s", s->name);
        s->state = SVC_FAILED;
        return;
    }

    LOG_I("Starting: %s", s->name);
    s->state = SVC_STARTING;

    run_script(s->exec_pre, s);

    pid_t pid = fork();
    if (pid < 0) {
        LOG_E("Fork failed: %s", s->name);
        s->state = SVC_FAILED;
        return;
    }

    if (pid == 0) {
        if (s->workdir[0]) chdir(s->workdir);
        if (s->nice_val != 0) nice(s->nice_val);
        set_env(s);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        exec_cmd(s->exec[0]);
        _exit(127);
    }

    s->pid = pid;
    s->started_at = time(NULL);

    if (s->cgroup[0]) {
        cgroup_create(s->cgroup);
        cgroup_attach(s->cgroup, pid);
        if (s->mem_limit > 0) cgroup_set_mem(s->cgroup, s->mem_limit);
    }

    if (s->type == TYPE_ONESHOT) {
        waitpid(pid, NULL, 0);
        s->state = SVC_ACTIVE;
        s->pid = -1;
    } else {
        s->state = SVC_ACTIVE;
    }

    if (s->state == SVC_ACTIVE) run_script(s->exec_post, s);
    LOG_I("Started: %s (PID %d)", s->name, pid);
}

static void stop_svc(svc_t *s, int force) {
    if (s->state == SVC_INACTIVE) return;
    LOG_I("Stopping: %s", s->name);
    s->state = SVC_STOPPING;

    if (s->pid > 0) {
        int sig = force ? SIGKILL : SIGTERM;
        kill(s->pid, sig);
        for (int i = 0; i < s->timeout_sec * 10 && s->pid > 0; i++) {
            pid_t r = waitpid(s->pid, NULL, WNOHANG);
            if (r > 0) break;
            usleep(100000);
        }
        if (s->pid > 0) {
            kill(s->pid, SIGKILL);
            waitpid(s->pid, NULL, 0);
        }
    }

    run_script(s->exec_stop, s);
    if (s->cgroup[0]) cgroup_destroy(s->cgroup);

    s->pid = -1;
    s->state = SVC_INACTIVE;
    s->restarts = 0;
    LOG_I("Stopped: %s", s->name);
}

static void reload_svc(svc_t *s) {
    if (s->state == SVC_ACTIVE && s->pid > 0) {
        LOG_I("Reloading: %s", s->name);
        kill(s->pid, SIGHUP);
    }
}

/* ---- Reap children ---- */
static void reap(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < svc_count; i++) {
            svc_t *s = &services[i];
            if (s->pid == pid) {
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
                s->exit_code = code;
                s->pid = -1;

                if (s->state == SVC_STOPPING) {
                    s->state = SVC_INACTIVE;
                    continue;
                }

                if (s->state == SVC_ACTIVE && s->restart != RESTART_NO) {
                    int should = (s->restart == RESTART_ALWAYS) ||
                                 (s->restart == RESTART_ON_FAILURE && code != 0) ||
                                 (s->restart == RESTART_ON_ABORT && WIFSIGNALED(status));
                    if (should && s->restarts < s->restart_max) {
                        s->restarts++;
                        LOG_I("Restarting: %s (%d/%d)", s->name, s->restarts, s->restart_max);
                        s->state = SVC_INACTIVE;
                        sleep(s->restart_sec);
                        start_svc(s);
                        continue;
                    }
                }

                if (s->state == SVC_ACTIVE) {
                    LOG_I("Service exited: %s (code %d)", s->name, code);
                    s->state = (code != 0) ? SVC_FAILED : SVC_INACTIVE;
                }
                break;
            }
        }
    }
}

/* ---- Mount filesystems ---- */
static void mount_fs(void) {
    static const struct { const char *target; const char *type; const char *opts; } mounts[] = {
        {"/proc",   "proc",     NULL},
        {"/sys",    "sysfs",    NULL},
        {"/dev",    "devtmpfs", "mode=755"},
        {"/run",    "tmpfs",    "mode=0755"},
        {"/tmp",    "tmpfs",    "mode=1777"},
        {NULL, NULL, NULL}
    };

    for (int i = 0; mounts[i].target; i++) {
        mkdir(mounts[i].target, 0755);
        mount(mounts[i].type, mounts[i].target, mounts[i].type, 0, mounts[i].opts);
    }

    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620");

    /* Essential device nodes */
    static const struct { const char *p; mode_t m; int maj; int min; } devs[] = {
        {"/dev/null",    0666|S_IFCHR, 1, 3},
        {"/dev/zero",    0666|S_IFCHR, 1, 5},
        {"/dev/random",  0666|S_IFCHR, 1, 8},
        {"/dev/urandom", 0666|S_IFCHR, 1, 9},
        {"/dev/tty",     0666|S_IFCHR, 5, 0},
        {"/dev/console", 0600|S_IFCHR, 5, 1},
        {"/dev/ptmx",    0666|S_IFCHR, 5, 2},
        {NULL, 0, 0, 0}
    };
    for (int i = 0; devs[i].p; i++)
        if (access(devs[i].p, F_OK) != 0)
            mknod(devs[i].p, devs[i].m, makedev(devs[i].maj, devs[i].min));
}

/* ---- Console ---- */
static void setup_console(void) {
    int fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        if (fd != STDIN_FILENO)  dup2(fd, STDIN_FILENO);
        if (fd != STDOUT_FILENO) dup2(fd, STDOUT_FILENO);
        if (fd != STDERR_FILENO) dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

/* ---- Signal handlers ---- */
static void on_sigchld(int sig) { (void)sig; }
static void on_sigterm(int sig) { (void)sig; do_shutdown = 1; }
static void on_sighup(int sig)  { (void)sig; do_reload = 1; }

static void setup_signals(void) {
    struct sigaction sa = {0};
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sa.sa_handler = on_sigchld;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_sighup;
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
}

/* ---- Control socket ---- */
static int setup_ctrl(void) {
    unlink(YINIT_SOCKET);
    mkdir(YINIT_STATE_DIR, 0755);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, YINIT_SOCKET, sizeof(addr.sun_path)-1);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    chmod(YINIT_SOCKET, 0666);
    return fd;
}

static const char *state_str(svc_state_t st) {
    switch(st) {
        case SVC_INACTIVE: return "inactive";
        case SVC_STARTING: return "starting";
        case SVC_ACTIVE:   return "active";
        case SVC_STOPPING: return "stopping";
        case SVC_FAILED:   return "failed";
    }
    return "unknown";
}

static void handle_ctrl(void) {
    char buf[4096];
    struct sockaddr_un ca;
    socklen_t cl = sizeof(ca);
    int len = recvfrom(ctrl_fd, buf, sizeof(buf)-1, 0, (struct sockaddr*)&ca, &cl);
    if (len <= 0) return;
    buf[len] = '\0';

    char *cmd = strtok(buf, " ");
    char *arg = strtok(NULL, " ");
    char resp[4096] = "";

    if (streq(cmd, "status")) {
        if (arg) {
            svc_t *s = find_svc(arg);
            if (s) snprintf(resp, sizeof(resp), "%-20s %-10s PID:%-8d %s\n",
                s->name, state_str(s->state), s->pid, s->desc);
            else snprintf(resp, sizeof(resp), "Unknown service: %s\n", arg);
        } else {
            for (int i = 0; i < svc_count; i++) {
                char line[256];
                snprintf(line, sizeof(line), "%-20s %-10s PID:%-8d %s\n",
                    services[i].name, state_str(services[i].state),
                    services[i].pid, services[i].desc);
                strncat(resp, line, sizeof(resp)-strlen(resp)-1);
            }
        }
    } else if (streq(cmd, "start")) {
        if (arg) { svc_t *s = find_svc(arg); if (s) start_svc(s); }
    } else if (streq(cmd, "stop")) {
        if (arg) { svc_t *s = find_svc(arg); if (s) stop_svc(s, 0); }
    } else if (streq(cmd, "restart")) {
        if (arg) { svc_t *s = find_svc(arg); if (s) { stop_svc(s, 0); start_svc(s); } }
    } else if (streq(cmd, "reload")) {
        if (arg) { svc_t *s = find_svc(arg); if (s) reload_svc(s); }
    } else if (streq(cmd, "poweroff")) {
        do_shutdown = 1;
        snprintf(resp, sizeof(resp), "Power off requested\n");
    } else if (streq(cmd, "reboot")) {
        do_shutdown = 2;
        snprintf(resp, sizeof(resp), "Reboot requested\n");
    } else if (streq(cmd, "version")) {
        snprintf(resp, sizeof(resp), "Yinit %s\n", YINIT_VERSION);
    } else {
        snprintf(resp, sizeof(resp), "Unknown: %s\n", cmd ? cmd : "");
    }

    if (resp[0]) sendto(ctrl_fd, resp, strlen(resp), 0, (struct sockaddr*)&ca, cl);
}

/* ---- Shutdown ---- */
static void shutdown_sys(void) {
    LOG_I("Shutting down...");
    for (int i = svc_count - 1; i >= 0; i--)
        if (services[i].state == SVC_ACTIVE || services[i].state == SVC_STARTING)
            stop_svc(&services[i], 0);
    sleep(1);
    for (int i = svc_count - 1; i >= 0; i--)
        if (services[i].state != SVC_INACTIVE)
            stop_svc(&services[i], 1);
    sync();
    umount2("/proc", MNT_DETACH);
    umount2("/sys", MNT_DETACH);
    umount2("/dev/pts", MNT_DETACH);
    umount2("/dev", MNT_DETACH);
    umount2("/run", MNT_DETACH);
    umount2("/tmp", MNT_DETACH);
    sync();
}

/* ---- Main ---- */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    setup_console();

    mkdir("/var/log", 0755);
    log_fd = open(YINIT_LOG, O_WRONLY|O_CREAT|O_APPEND, 0644);
    LOG_I("Yinit %s starting (PID %d)", YINIT_VERSION, getpid());

    setup_signals();
    mount_fs();
    ctrl_fd = setup_ctrl();

    load_services();
    sort_services();
    LOG_I("Loaded %d services", svc_count);

    for (int i = 0; i < svc_count; i++)
        if (services[i].state == SVC_INACTIVE)
            start_svc(&services[i]);

    LOG_I("Main loop");

    while (running) {
        reap();

        fd_set fds;
        struct timeval tv = {0, 50000};
        FD_ZERO(&fds);
        if (ctrl_fd >= 0) { FD_SET(ctrl_fd, &fds); }

        if (select(ctrl_fd + 1, &fds, NULL, NULL, &tv) > 0) {
            if (ctrl_fd >= 0 && FD_ISSET(ctrl_fd, &fds)) handle_ctrl();
        }

        if (do_shutdown) {
            if (do_shutdown == 2) {
                shutdown_sys();
                sync();
                reboot(LINUX_REBOOT_CMD_RESTART);
            } else {
                shutdown_sys();
                reboot(LINUX_REBOOT_CMD_POWER_OFF);
            }
        }

        if (do_reload) {
            do_reload = 0;
            LOG_I("Reloading...");
            for (int i = 0; i < svc_count; i++)
                if (services[i].state == SVC_ACTIVE)
                    reload_svc(&services[i]);
        }
    }

    if (ctrl_fd >= 0) { close(ctrl_fd); unlink(YINIT_SOCKET); }
    if (log_fd >= 0) close(log_fd);
    return 0;
}
