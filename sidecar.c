Warning: Permanently added 'drivers-respectively-libs-caribbean.trycloudflare.com' (ED25519) to the list of known hosts.
/*
 * sidecar — chroot launcher for am_alac_decryptor's `wrapper` binary.
 *
 * Drop-in replacement for the original 7.8 KB sidecar with the following
 * fixes/additions:
 *
 *   1.  PTY for the child  →  the inner main's stdout becomes line-buffered
 *       (instead of fully-buffered as in a pure pipe), so login progress
 *       like "[+] Attempting login..." actually surfaces.  Forwarded to
 *       our stdout/stderr verbatim.
 *
 *   2.  DNS bootstrap        →  copy /etc/{resolv.conf,hosts,nsswitch.conf,
 *       services} into <rootfs>/etc/ before chroot, then create
 *       /dev/{urandom,random,null,zero} via mknod.  The original sidecar
 *       only did /dev/urandom, which is enough to start but not enough
 *       for many libc's resolver paths.
 *
 *   3.  Signal forwarding    →  SIGINT / SIGTERM / SIGHUP / SIGQUIT all
 *       forwarded to the child.  We track the child PID with a sig_atomic
 *       and use a self-pipe so we can wake from sigwait race-free.
 *
 *   4.  Configurable         →  --rootfs / --bin / --cwd-in-chroot CLI flags
 *       so the same binary works for any chroot+exec target.  Anything
 *       after `--` is passed through verbatim to the child.
 *
 *   5.  Verbose / health     →  --verbose prints what we're about to do
 *       BEFORE the chroot (so failures during setup are visible).
 *       --pid-file writes the child's PID for systemd / k8s probes.
 *
 *   6.  Graceful exit        →  on receipt of SIGTERM, send SIGTERM to
 *       child, wait up to --grace-secs (default 10s) for it to exit,
 *       then SIGKILL.  Exit with the child's exit status.
 *
 *   7.  Readiness gate       →  --wait-ports P[,P..] (--wait-timeout SECS)
 *       polls 127.0.0.1:Pi from inside the supervise loop until each
 *       port is reachable; this interleaves with PTY draining so a
 *       chatty child (Apple's main writes >4 KB during library load)
 *       does NOT stall while we wait.  Exposed to operators as:
 *         --json-events  → one-line {"t":ms,"event":...} on stderr
 *         --ready-fd N   → one '\1\n' on fd N when ports are ready,
 *                          fires at most once per sidecar lifetime so
 *                          systemd-style readiness doesn't double-fire
 *                          on restart.
 *
 *   8.  Graceful restart    →  SIGUSR1 makes us TERM the running child,
 *       wait up to --grace-secs, KILL on timeout, then re-fork with
 *       the same argv.  Useful when Apple's auth state goes stale and
 *       you want to re-login without redeploying.
 *
 * Build:
 *      cc -O2 -Wall -Wextra -o sidecar sidecar.c -lutil
 *
 * Usage (drop-in compatible with the original):
 *      ./sidecar -M 20020 -D 10020 --login=user@host:pass
 *
 *      # equivalent expanded form:
 *      ./sidecar --rootfs ./rootfs --bin /system/bin/main \
 *                --verbose --pid-file /run/wrapper.pid \
 *                -- -M 20020 -D 10020 --login=user@host:pass
 *
 *  Original sidecar's exact CLI is preserved: bare flags before any `--`
 *  are passed through to the child as-is, matching the legacy contract.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mount.h>

/* ─────────────────────── globals (signal-handler reach) ─────────────── */

static volatile sig_atomic_t g_child_pid     = 0;
static volatile sig_atomic_t g_restart_flag  = 0;   /* set by SIGUSR1 */
static int                   g_signal_pipe[2] = {-1, -1};
static int                   g_verbose       = 0;
static int                   g_json_events   = 0;
static int                   g_grace_secs    = 10;
static int                   g_userns        = 1;   /* user namespace mode (no root needed) */

/* ─────────────────────── small helpers ──────────────────────────────── */

static void vlog(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[sidecar] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Monotonic-ish wall-clock millisecond timestamp for event correlation. */
static long long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Emit one structured event line on stderr.
 *
 *   - JSON mode  → {"t":<ms>,"event":"<name>",<extra-kv-pairs>}
 *   - plain mode → [sidecar] <name> <extra-kv-pairs>     (only if g_verbose)
 *
 * `extra` is a printf-style fragment the caller already formatted; it must
 * be a sequence of `key=value` pairs separated by spaces in plain mode and
 * pre-formed JSON key:value pairs (with leading comma) in JSON mode. To
 * avoid double-quoting hell, callers use `evt(...)` and `evt_kv(...)`
 * helpers rather than raw printf.
 */
static void emit_event(const char *name, const char *extra_json,
                       const char *extra_plain) {
    if (g_json_events) {
        fprintf(stderr, "{\"t\":%lld,\"event\":\"%s\"%s}\n",
                now_ms(), name, extra_json ? extra_json : "");
        fflush(stderr);
    } else if (g_verbose) {
        fprintf(stderr, "[sidecar] %s%s%s\n",
                name,
                extra_plain && *extra_plain ? " " : "",
                extra_plain ? extra_plain : "");
    }
}

/* Convenience: emit a single-line event with no payload. */
static void evt(const char *name) {
    emit_event(name, "", "");
}

static void die(const char *what) {
    fprintf(stderr, "[sidecar] FATAL: %s: %s\n", what, strerror(errno));
    exit(1);
}

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(out, buf + w, n - w);
            if (k < 0) { close(in); close(out); return -1; }
            w += k;
        }
    }
    close(in); close(out);
    return 0;
}

static void mkdir_p(const char *path, mode_t mode) {
    /* Best-effort `mkdir -p`. Caller doesn't care if already exists. */
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
                /* swallow: maybe parent perms; final mkdir below will report */
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
        /* still ok; caller checks */
    }
}

/* ─────────────────────── pre-chroot setup ───────────────────────────── */

static void seed_chroot_etc(const char *rootfs) {
    char path[4096];
    snprintf(path, sizeof path, "%s/etc", rootfs);
    mkdir_p(path, 0755);

    static const struct { const char *src; const char *rel; } items[] = {
        { "/etc/resolv.conf",   "etc/resolv.conf"   },
        { "/etc/hosts",         "etc/hosts"         },
        { "/etc/nsswitch.conf", "etc/nsswitch.conf" },
        { "/etc/services",      "etc/services"      },
        { NULL, NULL }
    };
    for (size_t i = 0; items[i].src; i++) {
        char dst[4096];
        snprintf(dst, sizeof dst, "%s/%s", rootfs, items[i].rel);
        if (copy_file(items[i].src, dst) == 0) {
            vlog("seeded %s → %s", items[i].src, dst);
        } else {
            vlog("(skipped) %s → %s: %s", items[i].src, dst, strerror(errno));
        }
    }
}

static void seed_chroot_dev(const char *rootfs) {
    char dev[4096];
    snprintf(dev, sizeof dev, "%s/dev", rootfs);
    mkdir_p(dev, 0755);

    static const struct {
        const char *name;
        unsigned    major;
        unsigned    minor;
        mode_t      mode;
    } nodes[] = {
        { "urandom", 1, 9, S_IFCHR | 0666 },
        { "random",  1, 8, S_IFCHR | 0666 },
        { "null",    1, 3, S_IFCHR | 0666 },
        { "zero",    1, 5, S_IFCHR | 0666 },
        { NULL, 0, 0, 0 }
    };
    for (size_t i = 0; nodes[i].name; i++) {
        char node[4096];
        snprintf(node, sizeof node, "%s/dev/%s", rootfs, nodes[i].name);
        unlink(node);   /* best-effort; ignore failure */
        if (mknod(node, nodes[i].mode, makedev(nodes[i].major, nodes[i].minor)) == 0) {
            chmod(node, nodes[i].mode & 07777);
            vlog("created /dev/%s (major %u minor %u)", nodes[i].name,
                 nodes[i].major, nodes[i].minor);
        } else {
            vlog("(skipped) /dev/%s: %s", nodes[i].name, strerror(errno));
        }
    }
}

/* ─────────────────────── user namespace helpers ────────────────────────── */

static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(data);
    ssize_t w = write(fd, data, len);
    close(fd);
    return (w == len) ? 0 : -1;
}

static void setup_userns(uid_t real_uid, gid_t real_gid) {
    if (write_file("/proc/self/setgroups", "deny\n") < 0) {
        vlog("(warn) write setgroups failed: %s", strerror(errno));
    }
    char map[64];
    snprintf(map, sizeof map, "0 %d 1\n", real_uid);
    if (write_file("/proc/self/uid_map", map) < 0) {
        fprintf(stderr, "[sidecar] FATAL: write uid_map failed: %s\n", strerror(errno));
        _exit(1);
    }
    snprintf(map, sizeof map, "0 %d 1\n", real_gid);
    if (write_file("/proc/self/gid_map", map) < 0) {
        fprintf(stderr, "[sidecar] FATAL: write gid_map failed: %s\n", strerror(errno));
        _exit(1);
    }
    vlog("user namespace: mapped uid %d->0, gid %d->0", real_uid, real_gid);
}

static void bind_mount_dev(const char *rootfs) {
    /* Match wrapper_new: only bind-mount /dev/urandom, not all of /dev.
     * Mounting all of /dev can confuse the Android main binary. */
    char target[4096];
    snprintf(target, sizeof target, "%s/dev/urandom", rootfs);
    /* Remove stale symlink if present */
    struct stat st;
    if (lstat(target, &st) == 0 && S_ISLNK(st.st_mode)) {
        unlink(target);
        vlog("removed stale symlink /dev/urandom");
    }
    /* Ensure the mount target exists as a regular file */
    if (lstat(target, &st) != 0) {
        int fd = open(target, O_WRONLY | O_CREAT, 0666);
        if (fd >= 0) close(fd);
    }
    if (mount("/dev/urandom", target, NULL, MS_BIND, NULL) == 0) {
        vlog("bind-mounted /dev/urandom -> %s", target);
    } else {
        vlog("(warn) bind-mount /dev/urandom failed: %s", strerror(errno));
    }
}

static void bind_mount_proc_sys(const char *rootfs) {
    char path[4096];
    snprintf(path, sizeof path, "%s/proc", rootfs);
    mkdir_p(path, 0755);
    if (mount("proc", path, "proc", 0, NULL) == 0) {
        vlog("mounted procfs at %s", path);
    } else if (mount("/proc", path, NULL, MS_BIND | MS_REC, NULL) == 0) {
        vlog("bind-mounted /proc -> %s", path);
    } else {
        vlog("(warn) mount proc failed: %s", strerror(errno));
    }
    snprintf(path, sizeof path, "%s/sys", rootfs);
    mkdir_p(path, 0755);
    if (mount("/sys", path, NULL, MS_BIND | MS_REC, NULL) == 0) {
    }
}

/* ─────────────────────── port-readiness probe ───────────────────────── */

/* Try a single non-blocking TCP connect to 127.0.0.1:port with a short
 * deadline. Returns 1 on success, 0 on refused / timeout. The chroot lives
 * in our network namespace, so a loopback connect is the right semantics
 * for "is the wrapper listening?". */
static int probe_port(unsigned short port, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) return 0;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(s, (struct sockaddr *)&sa, sizeof sa);
    if (rc == 0) { close(s); return 1; }
    if (errno != EINPROGRESS) { close(s); return 0; }

    struct pollfd p = { s, POLLOUT, 0 };
    int n = poll(&p, 1, timeout_ms);
    if (n <= 0) { close(s); return 0; }

    int err = 0;
    socklen_t elen = sizeof err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
        close(s); return 0;
    }
    close(s);
    return 1;
}

/* Per-spawn port-wait state. Owned by supervise_until_done().
 *
 * The supervise loop's poll() schedules a 200ms tick whenever
 * `tick_due_ms <= now_ms()`. On each tick we probe any unseen ports
 * non-blockingly. When all_seen flips to 1 we emit `port_ready_all` (and
 * the caller signals the ready_fd). When `timeout_at_ms` is exceeded
 * before that, we emit `port_probe_timeout` once and stop ticking.
 *
 * Probing is interleaved with master_fd draining so a chatty child
 * (e.g. Apple's main, which writes >4KB during library load) never
 * stalls because its PTY filled up. */
typedef struct {
    const unsigned short *ports;
    size_t                n_ports;
    int                  *seen;        /* length n_ports; calloc'd by caller */
    long long             timeout_at_ms;
    long long             tick_due_ms;
    int                   complete;    /* 1 = stopped probing */
    int                   success;     /* meaningful only when complete=1 */
} PortWaitState;

/* ─────────────────────── signal plumbing ────────────────────────────── */

static void on_signal(int sig) {
    /* SIGUSR1 = restart request; never forward to the child (it would
     * misinterpret it). All other signals are forwarded so the child sees
     * them at the same time we do. */
    if (sig == SIGUSR1) {
        g_restart_flag = 1;
    } else if (g_child_pid > 0) {
        kill(g_child_pid, sig);
    }
    if (g_signal_pipe[1] >= 0) {
        unsigned char b = (unsigned char)sig;
        ssize_t r = write(g_signal_pipe[1], &b, 1);
        (void)r;
    }
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    /* SIGCHLD: just wake the loop; reaping is done via waitpid */
    sigaction(SIGCHLD, &sa, NULL);
}

/* ─────────────────────── parent loop ────────────────────────────────── */

typedef enum { END_EXITED, END_RESTART_REQUESTED } SuperviseEnd;

/* Run the per-child supervision loop until the child is dead. Returns:
 *   END_EXITED              → the child exited on its own (or after we
 *                             forwarded a TERM/INT/HUP/QUIT). `*exit_code`
 *                             is the value to return from main().
 *   END_RESTART_REQUESTED   → SIGUSR1 was received; we drove the child to
 *                             termination (TERM, then SIGKILL after the
 *                             grace deadline) and reaped it. The caller
 *                             should re-spawn.
 *
 * The function emits structured events on entry (`child_supervising`),
 * when a terminating signal is observed (`child_terminating`), when the
 * grace deadline expires (`child_force_kill`), and on exit
 * (`child_exited` / `child_restart_ack`).
 */
static SuperviseEnd supervise_until_done(pid_t child, int master_fd,
                                         PortWaitState *pw,
                                         int  ready_fd,
                                         int *ready_fd_signalled,
                                         int *exit_code) {
    struct pollfd pfds[2];
    pfds[0].fd = master_fd;          pfds[0].events = POLLIN;
    pfds[1].fd = g_signal_pipe[0];   pfds[1].events = POLLIN;

    int    term_signaled = 0;
    time_t term_at       = 0;
    int    restart_acked = 0;

    {
        char json[64], plain[64];
        snprintf(json,  sizeof json,  ",\"pid\":%d", (int)child);
        snprintf(plain, sizeof plain, "pid=%d",       (int)child);
        emit_event("child_supervising", json, plain);
    }

    for (;;) {
        /* Compute the poll deadline as the min of any active timer:
         *   - SIGTERM grace deadline (if term_signaled)
         *   - port-probe tick (if !pw->complete)
         */
        int timeout_ms = -1;
        long long now = now_ms();
        if (term_signaled) {
            time_t now_s = time(NULL);
            time_t deadline = term_at + g_grace_secs;
            int t = (deadline > now_s) ? (int)((deadline - now_s) * 1000) : 0;
            if (timeout_ms < 0 || t < timeout_ms) timeout_ms = t;
        }
        if (pw && !pw->complete) {
            long long t = pw->tick_due_ms - now;
            if (t < 0) t = 0;
            if (timeout_ms < 0 || (int)t < timeout_ms) timeout_ms = (int)t;
        }

        int n = poll(pfds, 2, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("poll");
        }
        if (term_signaled) {
            time_t now_s = time(NULL);
            if (now_s >= term_at + g_grace_secs) {
                evt("child_force_kill");
                kill(child, SIGKILL);
                term_signaled = 0;   /* avoid re-entering this branch */
            }
        }

        /* Port-probe tick: still ticking if no signal+master event arrived. */
        if (pw && !pw->complete && now_ms() >= pw->tick_due_ms) {
            int all_seen = 1;
            for (size_t i = 0; i < pw->n_ports; i++) {
                if (pw->seen[i]) continue;
                if (probe_port(pw->ports[i], 30)) {  /* 30ms probe timeout (loopback is instant) */
                    pw->seen[i] = 1;
                    char json[64], plain[64];
                    snprintf(json,  sizeof json,  ",\"port\":%u", pw->ports[i]);
                    snprintf(plain, sizeof plain, "port=%u",       pw->ports[i]);
                    emit_event("port_probe_first", json, plain);
                } else {
                    all_seen = 0;
                }
            }
            if (all_seen) {
                pw->complete = 1;
                pw->success  = 1;
                evt("port_ready_all");
                if (ready_fd >= 0 && ready_fd_signalled &&
                    !*ready_fd_signalled) {
                    ssize_t w = write(ready_fd, "\1\n", 2);
                    (void)w;
                    *ready_fd_signalled = 1;
                    evt("ready_fd_signalled");
                }
            } else if (now_ms() >= pw->timeout_at_ms) {
                pw->complete = 1;
                pw->success  = 0;
                evt("port_probe_timeout");
            } else {
                pw->tick_due_ms = now_ms() + 50;  /* 50ms probe tick (was 200ms) */
            }
        }
        (void)n;

        /* Forward child output */
        if (pfds[0].revents & POLLIN) {
            char buf[4096];
            ssize_t r = read(master_fd, buf, sizeof buf);
            if (r > 0) {
                ssize_t w = 0;
                while (w < r) {
                    ssize_t k = write(STDOUT_FILENO, buf + w, r - w);
                    if (k < 0) break;
                    w += k;
                }
            } else if (r == 0 || (r < 0 && errno != EINTR)) {
                pfds[0].fd = -1;
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            pfds[0].fd = -1;
        }

        /* Drain signal pipe */
        if (pfds[1].revents & POLLIN) {
            unsigned char sig;
            while (read(g_signal_pipe[0], &sig, 1) == 1) {
                if (sig == SIGTERM || sig == SIGINT ||
                    sig == SIGHUP  || sig == SIGQUIT) {
                    if (!term_signaled) {
                        term_signaled = 1;
                        term_at = time(NULL);
                        char json[80], plain[80];
                        snprintf(json,  sizeof json,
                                 ",\"sig\":%d,\"grace_secs\":%d",
                                 (int)sig, g_grace_secs);
                        snprintf(plain, sizeof plain,
                                 "sig=%d grace=%ds", (int)sig, g_grace_secs);
                        emit_event("child_terminating", json, plain);
                    }
                }
                /* SIGCHLD: reap below.  SIGUSR1: handled via g_restart_flag. */
            }
        }

        /* Restart request? Treat it like a TERM but remember we're restarting. */
        if (g_restart_flag && !restart_acked) {
            restart_acked = 1;
            if (!term_signaled) {
                term_signaled = 1;
                term_at = time(NULL);
                char json[64], plain[64];
                snprintf(json,  sizeof json,  ",\"grace_secs\":%d", g_grace_secs);
                snprintf(plain, sizeof plain, "grace=%ds", g_grace_secs);
                emit_event("child_restart_request", json, plain);
            }
            kill(child, SIGTERM);
            g_restart_flag = 0;
        }

        /* Reap child */
        int status;
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) {
            int code = WIFEXITED(status)   ? WEXITSTATUS(status)
                     : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                     : 1;
            char json[96], plain[96];
            snprintf(json,  sizeof json,
                     ",\"pid\":%d,\"code\":%d", (int)child, code);
            snprintf(plain, sizeof plain,
                     "pid=%d code=%d", (int)child, code);
            emit_event(restart_acked ? "child_restart_ack" : "child_exited",
                       json, plain);
            *exit_code = code;
            return restart_acked ? END_RESTART_REQUESTED : END_EXITED;
        }
        if (r < 0 && errno != ECHILD && errno != EINTR) die("waitpid");
    }
}

/* Fork + chroot + exec. Returns the child's pid; if PTY is in use, the
 * caller-provided slot is filled with the master fd and the slave is
 * already closed in the parent. */
static pid_t spawn_child(const char *rootfs, const char *bin,
                         const char *cwd_in_chroot,
                         char *const final_argv[], char *const envp[],
                         int use_pty, int *master_fd_out) {
    int master_fd = -1, slave_fd = -1;

    /* Step 1: PTY allocation BEFORE any namespace changes (needs /dev/pts).
     * In userns mode, /dev/pts is often unavailable — default to pipe for speed.
     * Use --force-pty to override. */
    if (use_pty && g_userns) {
        /* Try openpty but don't warn on expected userns failure */
        if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0) {
            use_pty = 0;  /* silent fallback to pipe — expected in userns */
        } else {
            fcntl(master_fd, F_SETFL, O_NONBLOCK);
        }
    } else if (use_pty) {
        if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0) {
            fprintf(stderr, "[sidecar] (warn) openpty failed: %s — falling back to pipe mode\n",
                    strerror(errno));
            use_pty = 0;
        } else {
            fcntl(master_fd, F_SETFL, O_NONBLOCK);
        }
    }

    /* Step 2: Enter namespaces BEFORE fork (like wrapper_new does).
     * This way the child is born inside the namespace. */
    uid_t saved_uid = 0;
    gid_t saved_gid = 0;
    if (g_userns) {
        saved_uid = getuid();
        saved_gid = getgid();
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID) < 0) {
            fprintf(stderr, "[sidecar] FATAL: unshare(NEWUSER|NEWNS) failed: %s\n"
                    "  Ensure /proc/sys/kernel/unprivileged_userns_clone == 1\n",
                    strerror(errno));
            _exit(1);
        }
        setup_userns(saved_uid, saved_gid);
        /* We are now uid 0 inside the namespace — mounts work */
        bind_mount_dev(rootfs);
        bind_mount_proc_sys(rootfs);
        /* DNS seed skipped in userns: fake uid 0 can't write to host-owned rootfs/etc,
         * and main doesn't need DNS (it uses IPs or parent-resolved hostnames). */
        vlog("namespace setup complete, forking child");
    }

    /* Step 3: Fork — child inherits the namespace */
    pid_t child = fork();
    if (child < 0) die("fork");

    if (child == 0) {
        /* ── child ── */
        if (use_pty) {
            close(master_fd);
            setsid();
            if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
                /* not fatal on all kernels */
            }
            dup2(slave_fd, STDIN_FILENO);
            dup2(slave_fd, STDOUT_FILENO);
            dup2(slave_fd, STDERR_FILENO);
            if (slave_fd > STDERR_FILENO) close(slave_fd);
        }

        if (chdir(rootfs) < 0)        die("chdir(rootfs)");
        if (chroot(".") < 0)          die("chroot");
        if (chdir(cwd_in_chroot) < 0) die("chdir(cwd_in_chroot)");

        execve(bin, final_argv, envp);
        die("execve");
        _exit(127);
    }

    /* parent */
    if (use_pty) close(slave_fd);
    if (master_fd_out) *master_fd_out = master_fd;
    {
        char json[64], plain[64];
        snprintf(json,  sizeof json,  ",\"pid\":%d", (int)child);
        snprintf(plain, sizeof plain, "pid=%d",       (int)child);
        emit_event("child_forked", json, plain);
    }
    return child;
}

/* ─────────────────────── main ───────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr,
        "sidecar — chroot launcher for am_alac wrapper\n"
        "\n"
        "Usage:\n"
        "  %s [sidecar-flags...] [--] [child-args...]\n"
        "\n"
        "Sidecar flags:\n"
        "  --rootfs PATH         chroot target (default: ./rootfs)\n"
        "  --bin PATH            executable inside chroot (default: /system/bin/main)\n"
        "  --cwd-in-chroot PATH  cwd to set after chroot (default: /)\n"
        "  --pid-file PATH       write child PID here\n"
        "  --grace-secs N        SIGTERM→SIGKILL grace (default: 10)\n"
        "  --no-pty              don't allocate a PTY (legacy buffered mode)\n"
        "  --userns              use user namespace (no root needed, default)\n"
        "  --no-userns           use real chroot (needs root)\n"
        "  --no-dns-seed         don't copy /etc/{resolv.conf,...} into chroot\n"
        "  --wait-ports P[,P..]  block until each loopback TCP port responds\n"
        "  --wait-timeout N      seconds to wait for ports (default: 120)\n"
        "  --ready-fd N          write '\\1\\n' on fd N when ports are ready\n"
        "  --json-events         emit one-line JSON status events on stderr\n"
        "  --verbose, -v         log setup steps\n"
        "  --help, -h            this message\n"
        "\n"
        "Lifecycle signals:\n"
        "  SIGINT / SIGTERM / SIGHUP / SIGQUIT — forwarded to the child;\n"
        "    on first one, the child gets `--grace-secs`s before SIGKILL.\n"
        "  SIGUSR1 — graceful restart: terminate the current child, wait\n"
        "    for it, then re-fork the same wrapper. New child re-uses the\n"
        "    same `--wait-ports`/--ready-fd discipline. `--ready-fd` is\n"
        "    only signalled the *first* time, so consumers won't see a\n"
        "    spurious second readiness ping.\n"
        "\n"
        "All other arguments before `--` (and everything after `--`) are\n"
        "passed through to the child binary.  This makes us a drop-in\n"
        "replacement for the legacy sidecar that did:\n"
        "  ./sidecar -M 20020 -D 10020 --login=user:pass\n",
        argv0);
}

/* Parse "10020,20020" into ports[]. Returns count, -1 on parse error. */
static ssize_t parse_port_list(const char *s, unsigned short *ports,
                               size_t cap) {
    if (!s || !*s) return 0;
    size_t n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p || v <= 0 || v > 65535) return -1;
        if (n >= cap) return -1;
        ports[n++] = (unsigned short)v;
        p = end;
    }
    return (ssize_t)n;
}

int main(int argc, char *argv[], char *envp[]) {
    const char *rootfs        = "./rootfs";
    const char *bin           = "/system/bin/main";
    const char *cwd_in_chroot = "/";
    const char *pid_file      = NULL;
    int         use_pty       = 1;
    int         seed_dns      = 1;
    const char *wait_ports_s  = NULL;
    int         wait_timeout  = 120;
    int         ready_fd      = -1;

    /* Two-pass arg parse: pull out our flags, leave the rest for child. */
    char *child_argv[argc + 2];
    int   child_argc = 0;

    int saw_double_dash = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (saw_double_dash) {
            child_argv[child_argc++] = argv[i];
            continue;
        }
        if (!strcmp(a, "--"))               { saw_double_dash = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { g_verbose = 1; continue; }
        if (!strcmp(a, "--json-events"))    { g_json_events = 1; continue; }
        if (!strcmp(a, "--no-pty"))         { use_pty = 0;  continue; }
        if (!strcmp(a, "--no-dns-seed"))    { seed_dns = 0; continue; }
        if (!strcmp(a, "--userns"))         { g_userns = 1; continue; }
        if (!strcmp(a, "--no-userns"))      { g_userns = 0; continue; }
        if (!strncmp(a, "--rootfs=", 9))    { rootfs = a + 9; continue; }
        if (!strcmp(a, "--rootfs") && i+1 < argc) { rootfs = argv[++i]; continue; }
        if (!strncmp(a, "--bin=", 6))       { bin = a + 6; continue; }
        if (!strcmp(a, "--bin") && i+1 < argc) { bin = argv[++i]; continue; }
        if (!strncmp(a, "--cwd-in-chroot=", 16)) { cwd_in_chroot = a + 16; continue; }
        if (!strcmp(a, "--cwd-in-chroot") && i+1 < argc) { cwd_in_chroot = argv[++i]; continue; }
        if (!strncmp(a, "--pid-file=", 11)) { pid_file = a + 11; continue; }
        if (!strcmp(a, "--pid-file") && i+1 < argc) { pid_file = argv[++i]; continue; }
        if (!strncmp(a, "--grace-secs=", 13)) { g_grace_secs = atoi(a + 13); continue; }
        if (!strcmp(a, "--grace-secs") && i+1 < argc) { g_grace_secs = atoi(argv[++i]); continue; }
        if (!strncmp(a, "--wait-ports=", 13)) { wait_ports_s = a + 13; continue; }
        if (!strcmp(a, "--wait-ports") && i+1 < argc) { wait_ports_s = argv[++i]; continue; }
        if (!strncmp(a, "--wait-timeout=", 15)) { wait_timeout = atoi(a + 15); continue; }
        if (!strcmp(a, "--wait-timeout") && i+1 < argc) { wait_timeout = atoi(argv[++i]); continue; }
        if (!strncmp(a, "--ready-fd=", 11)) { ready_fd = atoi(a + 11); continue; }
        if (!strcmp(a, "--ready-fd") && i+1 < argc) { ready_fd = atoi(argv[++i]); continue; }
        /* Unknown → pass through to child (drop-in compat) */
        child_argv[child_argc++] = argv[i];
    }
    /* argv[0] for the child becomes the bin path (Linux convention) */
    child_argv[child_argc] = NULL;

    /* Build the final child argv with bin as argv[0]. */
    char *final_argv[child_argc + 2];
    final_argv[0] = (char *)bin;
    for (int i = 0; i < child_argc; i++) final_argv[i + 1] = child_argv[i];
    final_argv[child_argc + 1] = NULL;

    vlog("rootfs = %s",        rootfs);
    vlog("bin    = %s",        bin);
    vlog("cwd    = %s (post-chroot)", cwd_in_chroot);
    vlog("pty    = %s",        use_pty ? "yes" : "no");
    vlog("dns    = %s",        seed_dns ? "seeded" : "skipped");
    vlog("grace  = %d s",      g_grace_secs);
    vlog("argv[0] = %s, %d additional args", final_argv[0], child_argc);

    /* Resolve --wait-ports up front so we fail fast on bad input. */
    unsigned short wait_ports[16];
    size_t         wait_ports_n = 0;
    if (wait_ports_s) {
        ssize_t r = parse_port_list(wait_ports_s, wait_ports,
                                    sizeof wait_ports / sizeof wait_ports[0]);
        if (r < 0) {
            fprintf(stderr,
                    "[sidecar] FATAL: bad --wait-ports value: %s\n",
                    wait_ports_s);
            return 2;
        }
        wait_ports_n = (size_t)r;
    }

    /* Pre-chroot bookkeeping (only done once — restarts inherit the seeded
     * rootfs). */
    if (seed_dns && !g_userns) seed_chroot_etc(rootfs);
    /* In userns mode, DNS seeding happens in spawn_child after namespace setup */
    if (!g_userns) {
        /* Real-root mode: create device nodes via mknod (needs CAP_MKNOD) */
        seed_chroot_dev(rootfs);
    }
    /* In userns mode, /dev bind-mount happens in the child after unshare() */
    evt("chroot_ready");

    vlog("userns = %s", g_userns ? "yes (no root needed)" : "no (real chroot)");

    /* Self-pipe for signals + signal handlers */
    if (pipe(g_signal_pipe) < 0) die("pipe");
    fcntl(g_signal_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_signal_pipe[1], F_SETFL, O_NONBLOCK);
    install_signal_handlers();

    int  ready_fd_signalled = 0;
    int  generation         = 0;
    int  exit_code          = 0;

    for (;;) {
        generation++;
        {
            char json[64], plain[64];
            snprintf(json,  sizeof json,  ",\"generation\":%d", generation);
            snprintf(plain, sizeof plain, "generation=%d",       generation);
            emit_event("spawning", json, plain);
        }

        int master_fd = -1;
        pid_t child = spawn_child(rootfs, bin, cwd_in_chroot,
                                  final_argv, envp, use_pty, &master_fd);
        g_child_pid = child;

        if (pid_file) {
            FILE *f = fopen(pid_file, "w");
            if (f) { fprintf(f, "%d\n", child); fclose(f); }
            else   { vlog("could not write pid-file %s: %s",
                          pid_file, strerror(errno)); }
        }

        /* If --ready-fd was given without --wait-ports, signal readiness
         * immediately on first generation: there's no port to gate on. */
        if (!wait_ports_n && !ready_fd_signalled && ready_fd >= 0) {
            ssize_t w = write(ready_fd, "\1\n", 2);
            (void)w;
            ready_fd_signalled = 1;
            evt("ready_fd_signalled");
        }

        if (use_pty) {
            /* Build a fresh per-spawn PortWaitState so probing is
             * interleaved with the supervise loop's master_fd draining.
             * This is the fix for chatty children whose stdout would
             * otherwise fill the PTY while we blocked in wait_for_ports. */
            PortWaitState pw_storage;
            PortWaitState *pw = NULL;
            int ports_seen_buf[16];
            if (wait_ports_n) {
                memset(ports_seen_buf, 0, sizeof ports_seen_buf);
                pw_storage.ports         = wait_ports;
                pw_storage.n_ports       = wait_ports_n;
                pw_storage.seen          = ports_seen_buf;
                pw_storage.timeout_at_ms = now_ms() + (long long)wait_timeout * 1000;
                pw_storage.tick_due_ms   = now_ms();
                pw_storage.complete      = 0;
                pw_storage.success       = 0;
                pw = &pw_storage;
                evt("ports_waiting");
            }

            SuperviseEnd how = supervise_until_done(
                child, master_fd, pw, ready_fd,
                &ready_fd_signalled, &exit_code);
            close(master_fd);
            if (how == END_EXITED) return exit_code;
            /* END_RESTART_REQUESTED: loop and respawn. */
        } else {
            /* No PTY: simple wait. SIGUSR1 is still honored — we use
             * waitpid + a tiny inner loop so we can spot it. */
            for (;;) {
                int status;
                pid_t r = waitpid(child, &status, WNOHANG);
                if (r == child) {
                    if (WIFEXITED(status))   exit_code = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
                    else                     exit_code = 1;
                    break;
                }
                if (r < 0 && errno != EINTR) die("waitpid");
                if (g_restart_flag) {
                    g_restart_flag = 0;
                    kill(child, SIGTERM);
                    /* Wait synchronously up to grace_secs, then KILL. */
                    long long deadline = now_ms() + (long long)g_grace_secs * 1000;
                    while (now_ms() < deadline) {
                        r = waitpid(child, &status, WNOHANG);
                        if (r == child) goto reaped_for_restart;
                        struct timespec t = { 0, 100 * 1000 * 1000 };
                        nanosleep(&t, NULL);
                    }
                    kill(child, SIGKILL);
                    waitpid(child, &status, 0);
                reaped_for_restart:
                    evt("child_restart_ack");
                    goto next_generation;
                }
                /* Block on a single signal-pipe read with a 1s cap so we
                 * remain responsive to SIGUSR1. */
                struct pollfd p = { g_signal_pipe[0], POLLIN, 0 };
                poll(&p, 1, 1000);
                if (p.revents & POLLIN) {
                    unsigned char drain;
                    while (read(g_signal_pipe[0], &drain, 1) == 1) {}
                }
            }
            return exit_code;
        next_generation: ;
        }
    }
}
