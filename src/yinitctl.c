/*
 * yinitctl - Control tool for Yinit
 * Usage: yinitctl <command> [service]
 * Commands: status, start, stop, restart, reload, list, poweroff, reboot
 */
#include "yinit.h"

static int send_cmd(const char *cmd, char *resp, size_t resplen) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    char tmppath[64];
    snprintf(tmppath, sizeof(tmppath), "/tmp/yinitctl-%d", getpid());

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, tmppath, sizeof(addr.sun_path)-1);
    unlink(tmppath);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }

    struct sockaddr_un dest = {0};
    dest.sun_family = AF_UNIX;
    strncpy(dest.sun_path, YINIT_SOCKET, sizeof(dest.sun_path)-1);

    if (sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
        perror("sendto"); close(fd); unlink(tmppath); return -1;
    }

    fd_set fds;
    struct timeval tv = {5, 0};
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
        int len = recv(fd, resp, resplen - 1, 0);
        if (len > 0) {
            resp[len] = '\0';
            close(fd);
            unlink(tmppath);
            return 0;
        }
    }

    close(fd);
    unlink(tmppath);
    fprintf(stderr, "yinitctl: no response from yinit\n");
    return -1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Yinit Control Tool v" YINIT_VERSION "\n\n"
        "Usage: %s <command> [service]\n\n"
        "Commands:\n"
        "  status [service]   Show service status\n"
        "  start  <service>   Start a service\n"
        "  stop   <service>   Stop a service\n"
        "  restart <service>  Restart a service\n"
        "  reload <service>   Reload a service (SIGHUP)\n"
        "  list               List all services\n"
        "  poweroff           Power off the system\n"
        "  reboot             Reboot the system\n"
        "  version            Show version\n"
        "  help               Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    char resp[4096] = "";
    char cmd[4096] = "";

    if (streq(argv[1], "help") || streq(argv[1], "--help") || streq(argv[1], "-h")) {
        print_usage(argv[0]);
        return 0;
    }

    if (streq(argv[1], "version") || streq(argv[1], "-V")) {
        snprintf(cmd, sizeof(cmd), "version");
    } else if (streq(argv[1], "status")) {
        if (argc > 2) snprintf(cmd, sizeof(cmd), "status %s", argv[2]);
        else snprintf(cmd, sizeof(cmd), "status");
    } else if (streq(argv[1], "list")) {
        snprintf(cmd, sizeof(cmd), "status");
    } else if (streq(argv[1], "start")) {
        if (argc < 3) { fprintf(stderr, "Usage: %s start <service>\n", argv[0]); return 1; }
        snprintf(cmd, sizeof(cmd), "start %s", argv[2]);
    } else if (streq(argv[1], "stop")) {
        if (argc < 3) { fprintf(stderr, "Usage: %s stop <service>\n", argv[0]); return 1; }
        snprintf(cmd, sizeof(cmd), "stop %s", argv[2]);
    } else if (streq(argv[1], "restart")) {
        if (argc < 3) { fprintf(stderr, "Usage: %s restart <service>\n", argv[0]); return 1; }
        snprintf(cmd, sizeof(cmd), "restart %s", argv[2]);
    } else if (streq(argv[1], "reload")) {
        if (argc < 3) { fprintf(stderr, "Usage: %s reload <service>\n", argv[0]); return 1; }
        snprintf(cmd, sizeof(cmd), "reload %s", argv[2]);
    } else if (streq(argv[1], "poweroff")) {
        snprintf(cmd, sizeof(cmd), "poweroff");
    } else if (streq(argv[1], "reboot")) {
        snprintf(cmd, sizeof(cmd), "reboot");
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    if (send_cmd(cmd, resp, sizeof(resp)) == 0) {
        printf("%s", resp);
    }

    return 0;
}
