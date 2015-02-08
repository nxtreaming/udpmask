#include <errno.h>
#include <libgen.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "log.h"
#include "transform.h"
#include "udpmask.h"

static int bind_sock = -1;
static struct sockaddr_in conn_addr;
static int timeout = UM_TIMEOUT;
static struct um_sockmap map[UM_MAX_CLIENT];
static int tlimit = -1;

static volatile sig_atomic_t signal_term = 0;

static int usage(void)
{
    const char ubuf[] =
    "Usage: udpmask -m mode -s mask\n"
    "               -c remote -o remote_port\n"
    "               [-l listen] [-p listen_port]\n"
    "               [-t timeout] [-d] [-P pidfile]\n"
    "               [-L mask limit] [-h]\n";
    fprintf(stderr, ubuf);
    return 1;
}

/////////////////////////////////////////////////////////////////////
// sock_fd_max
/////////////////////////////////////////////////////////////////////

static int sock_fd_max = -1;

static inline void update_sock_fd_max(void)
{
    sock_fd_max = bind_sock;
    for (int i = 0; i < ARRAY_SIZE(map); i++) {
        if (map[i].in_use && map[i].sock > sock_fd_max) {
            sock_fd_max = map[i].sock;
        }
    }
}

#define UPDATE_SOCK_FD_MAX_ADD(sock)        \
    do {                                    \
        if (sock > sock_fd_max) {           \
            sock_fd_max = sock;             \
        }                                   \
    } while (0)                             \

#define UPDATE_SOCK_FD_MAX_RM(sock)         \
    do {                                    \
        if (sock >= sock_fd_max) {          \
            update_sock_fd_max();           \
        }                                   \
    } while (0)                             \

/////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////
// um_sockmap
/////////////////////////////////////////////////////////////////////

#define UPDATE_LAST_USE(idx)                    \
    do {                                        \
        time_t time_val = time(NULL);           \
        if (time_val != TIME_INVALID) {         \
            map[idx].last_use = time_val;       \
        }                                       \
    } while (0)                                 \

static inline int um_sockmap_ins(int sock, struct sockaddr_in *addr)
{
    int i = 0;

    for (i = 0; i < ARRAY_SIZE(map); i++) {
        if (!map[i].in_use) {
            map[i].in_use = 1;
            map[i].sock = sock;
            map[i].last_use = TIME_INVALID;
            map[i].from = *addr;
            break;
        }
    }

    if (i >= ARRAY_SIZE(map)) {
        return -1;
    }

    return i;
}

static inline int um_sockmap_clean(fd_set *active_set)
{
    int purged = 0;

    if (timeout <= 0) {
        return purged;
    }

    time_t time_val = time(NULL);
    for (int i = 0; i < ARRAY_SIZE(map); i++) {
        if (map[i].in_use && (map[i].last_use == TIME_INVALID ||
            time_val - map[i].last_use >= timeout)) {
            map[i].in_use = 0;
            close(map[i].sock);
            FD_CLR(map[i].sock, active_set);

            UPDATE_SOCK_FD_MAX_RM(map[i].sock);

            log_info("Purged connection from [%s:%hu]",
                     inet_ntoa(map[i].from.sin_addr),
                     ntohs(map[i].from.sin_port));

            if (!purged) {
                purged = 1;
            }
        }
    }

    return purged;
}

/////////////////////////////////////////////////////////////////////

static inline int sockaddr_in_cmp(struct sockaddr_in *a,
                                  struct sockaddr_in *b)
{
    int af_cmp = a->sin_family - b->sin_family;
    if (af_cmp != 0) {
        return af_cmp;
    }

    int port_cmp = a->sin_port - b->sin_port;
    if (port_cmp != 0) {
        return port_cmp;
    }

    long in_addr_cmp = a->sin_addr.s_addr - b->sin_addr.s_addr;
    if (in_addr_cmp != 0) {
        return (int) in_addr_cmp;
    }

    return 0;
}

static void sighanlder(int signum)
{
    if (signum == SIGHUP || signum == SIGINT || signum == SIGTERM) {
        signal_term = 1;
    }
}

// Main loop
int start(enum um_mode mode)
{
    ssize_t ret;
    int select_ret;
    int sock_idx;
    int tmp_sock = -1;

    struct sockaddr_in recv_addr;
    socklen_t recv_addr_len = sizeof(recv_addr);

    unsigned char buf[UM_BUFFER];
    size_t buflen, outbuflen;

    fd_set active_fd_set, read_fd_set;

    FD_ZERO(&active_fd_set);
    FD_SET(bind_sock, &active_fd_set);

    log_info("Connection timeout %d", timeout);

    int clean_up_trigged;

    update_sock_fd_max();

    while (!signal_term) {
        read_fd_set = active_fd_set;

        sock_idx = -1;
        clean_up_trigged = 0;

        select_ret = select(sock_fd_max + 1, &read_fd_set, NULL, NULL, NULL);
        if (select_ret <= 0) {
            log_debug("select() returns %d", select_ret);
            continue;
        }

        if (FD_ISSET(bind_sock, &read_fd_set)) {
            // Deal with packets from "listening" socket
            ret = recvfrom(bind_sock, (void *) buf, UM_BUFFER, 0,
                           (struct sockaddr *) &recv_addr, &recv_addr_len);

            if (ret > 0) {
                buflen = (size_t) ret;

                // Try to locate existing connection from map
                for (int i = 0; i < ARRAY_SIZE(map); i++) {
                    if (map[i].in_use &&
                        sockaddr_in_cmp(&recv_addr, &(map[i].from)) == 0) {
                        sock_idx = i;
                        break;
                    }
                }

                if (sock_idx < 0) {
                    log_info("New connection from [%s:%hu]",
                             inet_ntoa(recv_addr.sin_addr),
                             ntohs(recv_addr.sin_port));

                    tmp_sock = NEW_SOCK();
                    if (tmp_sock < 0) {
                        log_err("socket(): %s", strerror(errno));
                    } else {
                        um_sockmap_clean(&active_fd_set);
                        clean_up_trigged = 1;

                        sock_idx = um_sockmap_ins(tmp_sock, &recv_addr);
                        if (sock_idx >= 0) {
                            // Inserted newly created socket into sockmap
                            FD_SET(tmp_sock, &active_fd_set);
                            connect(tmp_sock, (struct sockaddr *) &conn_addr,
                                    sizeof(conn_addr));
                            
                            UPDATE_SOCK_FD_MAX_ADD(tmp_sock);
                        } else {
                            // Failed to insert newly created socket into sockmap
                            log_warn("Max clients reached. "
                                     "Dropping new connection [%s:%hu]",
                                     inet_ntoa(recv_addr.sin_addr),
                                     ntohs(recv_addr.sin_port));
                        }
                    }
                } 
                
                // Check sock_idx again to deal with new connection
                if (sock_idx >= 0) {
                    transform(mode, buf, buflen, buf, &outbuflen, tlimit);
                    send(map[sock_idx].sock, (void *) buf, outbuflen, 0);

                    UPDATE_LAST_USE(sock_idx);
                }
            }
        }

        for (int i = 0; i < ARRAY_SIZE(map); i++) {
            if (map[i].in_use && FD_ISSET(map[i].sock, &read_fd_set)) {
                ret = recv(map[i].sock, (void *) buf, UM_BUFFER, 0);

                if (ret > 0) {
                    UPDATE_LAST_USE(i);

                    buflen = (size_t) ret;

                    transform(mode, buf, buflen, buf, &outbuflen, tlimit);
                    sendto(bind_sock, (void *) buf, outbuflen, 0,
                           (struct sockaddr *) &map[i].from,
                           sizeof(map[i].from));
                }
            }
        }

        if (!clean_up_trigged) {
            um_sockmap_clean(&active_fd_set);
        }
    }

    // Clean up
    for (int i = 0; i < ARRAY_SIZE(map); i++) {
        if (map[i].in_use) {
            map[i].in_use = 0;
            close(map[i].sock);
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    int ret = 0;

    memset((void *) &conn_addr, 0, sizeof(conn_addr));
    memset((void *) map, 0, sizeof(map));

    enum um_mode mode = UM_MODE_NONE;
    struct in_addr addr = { .s_addr = INADDR_ANY };
    uint16_t port = 0;
    struct in_addr addr_conn = { .s_addr = 0 };
    uint16_t port_conn = 0;

    struct sockaddr_in bind_addr;
    memset((void *) &bind_addr, 0, sizeof(bind_addr));

    const char *pidfile = 0;
    struct hostent * rh;
    int show_usage = 0, daemonize = 0;
    int c;
    int r;

    while ((c = getopt(argc, argv, "m:p:l:s:c:o:t:dP:L:h")) != -1) {
        switch (c) {
        case 'm':
            if (strcmp(optarg, "server") == 0) {
                mode = UM_MODE_SERVER;
            } else if (strcmp(optarg, "client") == 0) {
                mode = UM_MODE_CLIENT;
            } else {
                show_usage = 1;
            }
            break;

        case 's':
            r = load_mask(optarg);
            if (r < 0) {
                show_usage = 1;
            }
            break;

        case 'l':
            r = inet_pton(AF_INET, optarg, (void *) &addr);
            if (r == 0) {
                show_usage = 1;
            } else if (r < 0) {
                perror("inet_pton()");
                ret = 1;
                goto exit;
            }
            break;

        case 'p':
            port = (uint16_t) atoi(optarg);
            break;

        case 'c':
            rh = gethostbyname2(optarg, AF_INET);
            if (!rh) {
                herror("gethostbyname2()");
                ret = 1;
                goto exit;
            } else {
                memcpy(&addr_conn, rh->h_addr_list[0], rh->h_length);
            }

            break;

        case 'o':
            port_conn = (uint16_t) atoi(optarg);
            break;

        case 't':
            r = atoi(optarg);
            if (r >= 0) {
                timeout = r;
            }
            break;

        case 'd':
            daemonize = 1;
            break;

        case 'P':
            pidfile = optarg;
            break;

        case 'L':
            tlimit = atoi(optarg);
            break;

        case 'h':
        case '?':
        default:
            show_usage = 1;
            break;
        }
    }

    if (!mask_loaded || port_conn == 0 || addr_conn.s_addr == 0) {
        show_usage = 1;
    }

    bind_sock = NEW_SOCK();
    if (bind_sock < 0) {
        perror("socket()");
        ret = 1;
        goto exit;
    }

    switch (mode) {
    case UM_MODE_SERVER:
        if (port == 0) {
            port = UM_SERVER_PORT;
        }
        break;

    case UM_MODE_CLIENT:
        if (port == 0) {
            port = UM_CLIENT_PORT;
        }
        break;

    default:
        show_usage = 1;
        break;
    }

    if (show_usage) {
        ret = usage();
        goto exit;
    }

    signal(SIGHUP, sighanlder);
    signal(SIGINT, sighanlder);
    signal(SIGTERM, sighanlder);

    if (daemonize) {
        use_syslog = 1;

        pid_t pid, sid;
        pid = fork();
        if (pid < 0) {
            perror("fork()");
            ret = 1;
            goto exit;
        } else if (pid > 0) {
            goto exit;
        }

        umask(022);

        sid = setsid();
        if (sid < 0) {
            perror("setsid()");
            ret = 1;
            goto exit;
        }

        if (chdir("/") < 0) {
            perror("chdir()");
            ret = 1;
            goto exit;
        }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        if (!pidfile) {
            pidfile = "/var/run/udpmask.pid";
        }

        FILE *fp = fopen(pidfile, "w");
        if (fp) {
            fprintf(fp, "%i\n", getpid());
            fclose(fp);
        }
    }

    startlog(basename(argv[0]));

    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr = addr;
    bind_addr.sin_port = htons(port);

    log_info("Bind to [%s:%hu]", inet_ntoa(addr), port);

    r = bind(bind_sock, (struct sockaddr *) &bind_addr, sizeof(bind_addr));
    if (r != 0) {
        log_err("bind(): %s", strerror(errno));
        ret = 1;
        goto exit;
    }

    conn_addr.sin_family = AF_INET;
    conn_addr.sin_addr = addr_conn;
    conn_addr.sin_port = htons(port_conn);

    log_info("Remote address [%s:%hu]", inet_ntoa(addr_conn), port_conn);

    ret = start(mode);

    unload_mask();

exit:
    close(bind_sock);
    endlog();
    return ret;
}
