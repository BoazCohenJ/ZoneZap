/* ZoneZap: removes the "file:Zone.Identifier" files that appear in WSL when
 * Windows copies files with alternate data streams into a Linux filesystem.
 *
 * It watches whole filesystems with fanotify (no per-directory watches), and when a
 * "<name>:Zone.Identifier" file is closed after writing, it saves the content in a
 * user.windows.Zone.Identifier extended attribute on <name> and deletes the file.
 */
#define _GNU_SOURCE
#include "logic.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#ifndef ZZ_VERSION
#define ZZ_VERSION "dev"
#endif

#ifndef FAN_REPORT_DFID_NAME
#define FAN_REPORT_FID 0x00000200
#define FAN_REPORT_DIR_FID 0x00000400
#define FAN_REPORT_NAME 0x00000800
#define FAN_REPORT_DFID_NAME (FAN_REPORT_DIR_FID | FAN_REPORT_NAME)
#endif
#ifndef FAN_EVENT_INFO_TYPE_DFID_NAME
#define FAN_EVENT_INFO_TYPE_DFID_NAME 2
#endif
#ifndef FAN_MARK_FILESYSTEM
#define FAN_MARK_FILESYSTEM 0x00000100
#endif
#ifndef FAN_CREATE
#define FAN_CREATE 0x00000100
#endif
#ifndef FAN_MOVED_TO
#define FAN_MOVED_TO 0x00000080
#endif

#define BIN_PATH "/usr/local/bin/zonezap"
#define RUN_DIR "/run/zonezap"
#define STATUS_PATH RUN_DIR "/status"
#define LOCK_PATH RUN_DIR "/lock"
#define LOG_PATH "/var/log/zonezap.log"
#define UNIT_PATH "/etc/systemd/system/zonezap.service"
#define WSLCONF_PATH "/etc/wsl.conf"
#define PENDING_MAX 512
#define PENDING_TIMEOUT_MS 3000
#define MAX_FS 32

/* ---------- options and logging ---------- */

static struct {
    zz_streams streams;
    int keep_xattr;
    int verbose;
    int quiet;
    int dry_run;
} opt;

static volatile sig_atomic_t stop_flag = 0;

static void logf_(const char *fmt, ...)
{
    if (opt.quiet)
        return;
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s ", ts);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)
#define VLOG(...) do { if (opt.verbose) logf_(__VA_ARGS__); } while (0)

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got + 1 < size) {
        ssize_t n = read(fd, buf + got, size - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    return (int)got;
}

static int write_file_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.zonezap-tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)n;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Runs a program (no shell) and returns its exit code, or -1. */
static int run_prog(char *const argv[], int silent)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (silent) {
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) {
                dup2(dn, 1);
                dup2(dn, 2);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---------- status file ---------- */

static struct {
    long long removed;
    char last[PATH_MAX + 64];
    char watching[1024];
    time_t started;
} stats;

static void write_status(void)
{
    char buf[4096];
    int n = snprintf(buf, sizeof buf,
                     "pid=%d\nversion=%s\nstarted=%lld\nremoved=%lld\nlast=%s\nwatching=%s\n",
                     (int)getpid(), ZZ_VERSION, (long long)stats.started, stats.removed,
                     stats.last, stats.watching);
    if (n > 0)
        write_file_atomic(STATUS_PATH, buf, (size_t)n, 0644);
}

static int status_value(const char *status, const char *key, char *out, size_t out_size)
{
    size_t kl = strlen(key);
    const char *p = status;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *e = strchr(v, '\n');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= out_size)
                n = out_size - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return 0;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return -1;
}

/* Returns the pid of a running ZoneZap watcher, or 0. */
static int running_pid(void)
{
    char st[4096], v[32];
    if (read_file(STATUS_PATH, st, sizeof st) <= 0 || status_value(st, "pid", v, sizeof v) != 0)
        return 0;
    int pid = atoi(v);
    if (pid <= 0 || kill(pid, 0) != 0)
        return 0;
    char exe[PATH_MAX], link[64];
    snprintf(link, sizeof link, "/proc/%d/exe", pid);
    ssize_t n = readlink(link, exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        if (!strstr(exe, "zonezap"))
            return 0; /* stale file, pid reused */
    }
    return pid;
}

/* ---------- removing one entry ---------- */

static void dir_path(int dirfd, char *out, size_t size)
{
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", dirfd);
    ssize_t n = readlink(link, out, size - 1);
    if (n < 0)
        n = 0;
    out[n] = 0;
}

/* Judges dirfd/name and removes it if it is Windows stream junk. Returns 1 if removed. */
static int zap_entry(int dirfd, const char *name)
{
    size_t base_len;
    int idx = zz_match_name(name, &opt.streams, &base_len);
    if (idx < 0)
        return 0;

    char dpath[PATH_MAX];
    dir_path(dirfd, dpath, sizeof dpath);

    struct stat st;
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return 0; /* already gone */

    char head[ZZ_MAX_JUNK_SIZE + 1];
    ssize_t head_len = 0;
    if (S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= ZZ_MAX_JUNK_SIZE) {
        int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return 0;
        size_t got = 0;
        while (got < sizeof head) {
            ssize_t n = read(fd, head + got, sizeof head - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        close(fd);
        head_len = (ssize_t)got;
    }
    zz_verdict v = zz_judge(name, S_ISREG(st.st_mode), (long long)st.st_size, head,
                            (size_t)head_len, &opt.streams);
    if (v != ZZ_ZAP) {
        if (opt.verbose && v == ZZ_KEEP_CONTENT) {
            char hex[3 * 24 + 1] = "";
            for (ssize_t i = 0; i < head_len && i < 24; i++)
                snprintf(hex + 3 * i, 4, "%02x ", (unsigned char)head[i]);
            VLOG("%s/%s: %s (%lld bytes: %s)", dpath, name, zz_verdict_text(v),
                 (long long)st.st_size, hex);
        } else {
            VLOG("%s/%s: %s", dpath, name, zz_verdict_text(v));
        }
        return 0;
    }
    if (opt.dry_run) {
        printf("%s/%s\n", dpath, name);
        return 1;
    }

    /* keep the content as an extended attribute on the real file */
    int saved = 0;
    if (opt.keep_xattr && head_len > 0) {
        char base[NAME_MAX + 1];
        if (base_len <= NAME_MAX) {
            memcpy(base, name, base_len);
            base[base_len] = 0;
            struct stat bst;
            if (fstatat(dirfd, base, &bst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(bst.st_mode)) {
                int bfd = openat(dirfd, base, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                if (bfd >= 0) {
                    char xname[ZZ_MAX_STREAM_NAME + 32];
                    zz_xattr_name(opt.streams.names[idx], xname, sizeof xname);
                    size_t xl = (size_t)head_len > 4000 ? 4000 : (size_t)head_len;
                    /* Explorer writes the stream again with less in it (just ZoneId);
                     * don't let that replace a fuller copy kept a moment earlier */
                    char old[4096];
                    ssize_t ol = fgetxattr(bfd, xname, old, sizeof old);
                    if (ol > 0 && !zz_zone_should_replace(old, (size_t)ol, head, xl)) {
                        saved = 1;
                        VLOG("%s/%s: kept the fuller zone info already saved", dpath, base);
                    } else if (fsetxattr(bfd, xname, head, xl, 0) == 0) {
                        saved = 1;
                    } else {
                        VLOG("%s/%s: could not save xattr: %s", dpath, base, strerror(errno));
                    }
                    close(bfd);
                }
            }
        }
    }

    if (unlinkat(dirfd, name, 0) != 0) {
        if (errno != ENOENT)
            LOG("could not remove %s/%s: %s", dpath, name, strerror(errno));
        return 0;
    }
    stats.removed++;
    snprintf(stats.last, sizeof stats.last, "%s/%s", dpath, name);
    LOG("removed %s/%s%s", dpath, name, saved ? " (zone info kept as xattr)" : "");
    return 1;
}

/* ---------- filesystems to watch ---------- */

typedef struct {
    char path[PATH_MAX];
    dev_t dev;
    fsid_t fsid;
    int mount_fd;
} watched_fs;

static watched_fs fss[MAX_FS];
static int nfs = 0;

/* Undo the \040-style escapes in /proc/self/mountinfo. */
static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' &&
            r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static int add_fs(const char *path, int quiet_fail)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if (!quiet_fail)
            LOG("cannot watch %s: %s", path, strerror(errno));
        return -1;
    }
    for (int i = 0; i < nfs; i++)
        if (fss[i].dev == st.st_dev)
            return 0;
    if (nfs >= MAX_FS)
        return -1;
    struct statfs sf;
    if (statfs(path, &sf) != 0)
        return -1;
    int mfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (mfd < 0)
        return -1;
    watched_fs *w = &fss[nfs++];
    snprintf(w->path, sizeof w->path, "%s", path);
    w->dev = st.st_dev;
    memcpy(&w->fsid, &sf.f_fsid, sizeof w->fsid);
    w->mount_fd = mfd;
    return 0;
}

/* Finds local disk filesystems from /proc/self/mountinfo, preferring a mount of the
 * filesystem root for each device. */
static void discover_fs(void)
{
    FILE *f = fopen("/proc/self/mountinfo", "re");
    if (!f) {
        add_fs("/", 0);
        return;
    }
    char line[4096];
    char cand[MAX_FS][PATH_MAX];
    int ncand = 0;
    /* two passes: first mounts whose root is "/", then the rest */
    char lines[256][PATH_MAX];
    int is_root[256];
    int nl = 0;
    while (fgets(line, sizeof line, f) && nl < 256) {
        char root[PATH_MAX], mp[PATH_MAX], fstype[64];
        /* id parent maj:min root mountpoint options [optional...] - fstype source super */
        if (sscanf(line, "%*s %*s %*s %4095s %4095s", root, mp) != 2)
            continue;
        char *dash = strstr(line, " - ");
        if (!dash || sscanf(dash + 3, "%63s", fstype) != 1)
            continue;
        unescape(mp);
        if (!zz_fs_type_watchable(fstype) || zz_mount_point_skipped(mp))
            continue;
        snprintf(lines[nl], PATH_MAX, "%s", mp);
        is_root[nl] = strcmp(root, "/") == 0;
        nl++;
    }
    fclose(f);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nl; i++)
            if (is_root[i] == (pass == 0) && ncand < MAX_FS)
                snprintf(cand[ncand++], PATH_MAX, "%s", lines[i]);
    for (int i = 0; i < ncand; i++)
        add_fs(cand[i], 1);
    if (nfs == 0)
        add_fs("/", 0);
}

static watched_fs *fs_by_fsid(const void *fsid)
{
    for (int i = 0; i < nfs; i++)
        if (memcmp(&fss[i].fsid, fsid, sizeof(fsid_t)) == 0)
            return &fss[i];
    return nfs == 1 ? &fss[0] : NULL;
}

/* ---------- watcher ---------- */

typedef struct {
    int dirfd;
    char name[NAME_MAX + 1];
    long long since;
} pending_t;

static pending_t pending[PENDING_MAX];
static int npending = 0;

static void pending_drop(int i)
{
    close(pending[i].dirfd);
    pending[i] = pending[--npending];
}

static int same_dir(int a, int b)
{
    struct stat sa, sb;
    return fstat(a, &sa) == 0 && fstat(b, &sb) == 0 && sa.st_dev == sb.st_dev &&
           sa.st_ino == sb.st_ino;
}

static void on_signal(int sig) { stop_flag = 1; }

static int lock_fd = -1;

static int take_lock(void)
{
    mkdir(RUN_DIR, 0755);
    lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0)
        return -1;
    return flock(lock_fd, LOCK_EX | LOCK_NB);
}

static void handle_event(struct fanotify_event_metadata *m)
{
    if (m->mask & FAN_Q_OVERFLOW) {
        LOG("event queue overflowed; run 'zonezap clean' to catch anything missed");
        return;
    }
    if (m->mask & FAN_ONDIR)
        return;
    char *p = (char *)(m + 1);
    char *end = (char *)m + m->event_len;
    while (p + sizeof(struct fanotify_event_info_header) <= end) {
        struct fanotify_event_info_header *h = (void *)p;
        if (h->len == 0 || p + h->len > end)
            break;
        if (h->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
            struct fanotify_event_info_fid *fid = (void *)p;
            struct file_handle *fh = (struct file_handle *)fid->handle;
            const char *name = (const char *)(fh->f_handle + fh->handle_bytes);
            if (zz_match_name(name, &opt.streams, NULL) < 0)
                return; /* the common case: not ours, cheap exit */
            watched_fs *w = fs_by_fsid(&fid->fsid);
            if (!w)
                return;
            int dirfd = open_by_handle_at(w->mount_fd, fh, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dirfd < 0) {
                VLOG("cannot open directory of %s: %s", name, strerror(errno));
                return;
            }
            /* fanotify merges events on the same file, so one event can be
             * CREATE|CLOSE_WRITE: only wait when it hasn't been closed yet */
            if ((m->mask & FAN_CREATE) && !(m->mask & (FAN_CLOSE_WRITE | FAN_MOVED_TO))) {
                /* wait for the writer to close it (or time out) */
                if (npending < PENDING_MAX) {
                    pending[npending].dirfd = dirfd;
                    snprintf(pending[npending].name, sizeof pending[npending].name, "%s", name);
                    pending[npending].since = now_ms();
                    npending++;
                    VLOG("seen %s, waiting for close", name);
                    return;
                }
                zap_entry(dirfd, name);
            } else {
                /* closed after writing, or renamed into place */
                for (int i = 0; i < npending; i++)
                    if (strcmp(pending[i].name, name) == 0 && same_dir(pending[i].dirfd, dirfd)) {
                        pending_drop(i);
                        break;
                    }
                if (zap_entry(dirfd, name))
                    write_status();
            }
            close(dirfd);
            return;
        }
        p += h->len;
    }
}

static int watch_loop(void)
{
    int fan = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK | FAN_REPORT_DFID_NAME,
                            O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (fan < 0) {
        if (errno == EPERM)
            fprintf(stderr, "zonezap: watching needs root. Try: sudo zonezap run\n");
        else if (errno == EINVAL)
            fprintf(stderr, "zonezap: this kernel is too old for fanotify name events "
                            "(needs Linux 5.9+). Run 'wsl --update' in Windows.\n");
        else
            fprintf(stderr, "zonezap: fanotify_init failed: %s\n", strerror(errno));
        return 1;
    }
    discover_fs();
    uint64_t mask = FAN_CREATE | FAN_MOVED_TO | FAN_CLOSE_WRITE;
    stats.watching[0] = 0;
    int marked = 0;
    for (int i = 0; i < nfs; i++) {
        if (fanotify_mark(fan, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask, AT_FDCWD, fss[i].path) != 0) {
            LOG("cannot watch %s: %s", fss[i].path, strerror(errno));
            continue;
        }
        marked++;
        size_t l = strlen(stats.watching);
        snprintf(stats.watching + l, sizeof stats.watching - l, "%s%s", l ? " " : "", fss[i].path);
    }
    if (!marked) {
        fprintf(stderr, "zonezap: no filesystem could be watched\n");
        return 1;
    }
    stats.started = time(NULL);
    write_status();
    LOG("ZoneZap %s watching %s for", ZZ_VERSION, stats.watching);
    for (int i = 0; i < opt.streams.count; i++)
        LOG("  *:%s", opt.streams.names[i]);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    static char buf[64 * 1024] __attribute__((aligned(8)));
    while (!stop_flag) {
        struct pollfd pfd = {fan, POLLIN, 0};
        int r = poll(&pfd, 1, npending ? 500 : 5000);
        if (r < 0 && errno != EINTR)
            break;
        if (r > 0) {
            for (;;) {
                ssize_t n = read(fan, buf, sizeof buf);
                if (n <= 0)
                    break;
                struct fanotify_event_metadata *m = (void *)buf;
                while (FAN_EVENT_OK(m, n)) {
                    if (m->vers == FANOTIFY_METADATA_VERSION)
                        handle_event(m);
                    if (m->fd >= 0)
                        close(m->fd);
                    m = FAN_EVENT_NEXT(m, n);
                }
            }
        }
        long long t = now_ms();
        int changed = 0;
        for (int i = npending - 1; i >= 0; i--)
            if (t - pending[i].since >= PENDING_TIMEOUT_MS) {
                changed |= zap_entry(pending[i].dirfd, pending[i].name);
                pending_drop(i);
            }
        if (changed)
            write_status();
    }
    LOG("stopping (removed %lld files)", stats.removed);
    unlink(STATUS_PATH);
    return 0;
}

static void daemonize(void)
{
    pid_t p = fork();
    if (p < 0)
        exit(1);
    if (p > 0)
        _exit(0);
    setsid();
    p = fork();
    if (p < 0)
        exit(1);
    if (p > 0)
        _exit(0);
    if (chdir("/") != 0) { /* nothing to do */ }
    umask(022);
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > 1024 * 1024)
        truncate(LOG_PATH, 0);
    int lf = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0)
        dup2(dn, 0);
    if (lf >= 0) {
        dup2(lf, 1);
        dup2(lf, 2);
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
}

static int cmd_run(int daemon)
{
    if (geteuid() != 0) {
        fprintf(stderr, "zonezap: watching needs root. Try: sudo zonezap run\n");
        return 1;
    }
    if (take_lock() != 0) {
        int pid = running_pid();
        fprintf(stderr, "zonezap: already running%s", pid ? "" : "\n");
        if (pid)
            fprintf(stderr, " (pid %d)\n", pid);
        return daemon ? 0 : 1;
    }
    if (daemon) {
        close(lock_fd); /* re-taken by the child below */
        daemonize();
        if (take_lock() != 0)
            return 0;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    return watch_loop();
}

/* ---------- clean ---------- */

static long long clean_found = 0;

static int clean_visit(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    if (stop_flag)
        return 1;
    const char *name = path + ftw->base;
    if (zz_match_name(name, &opt.streams, NULL) < 0)
        return 0;
    char dir[PATH_MAX];
    size_t dl = (size_t)ftw->base;
    if (dl == 0) {
        strcpy(dir, ".");
    } else {
        if (dl >= sizeof dir)
            return 0;
        memcpy(dir, path, dl);
        dir[dl] = 0;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
        return 0;
    clean_found += zap_entry(dfd, name);
    close(dfd);
    return 0;
}

static int cmd_clean(int argc, char **argv)
{
    const char *def[] = {NULL, NULL};
    char home[PATH_MAX];
    if (argc == 0) {
        /* default: root's filesystem when root, otherwise your home folder */
        if (geteuid() == 0) {
            def[0] = "/";
        } else {
            const char *h = getenv("HOME");
            snprintf(home, sizeof home, "%s", h ? h : ".");
            def[0] = home;
        }
        argv = (char **)def;
        argc = 1;
    }
    for (int i = 0; i < argc; i++) {
        if (!opt.dry_run)
            fprintf(stderr, "Cleaning %s ...\n", argv[i]);
        /* FTW_MOUNT: stay on one filesystem, so /mnt/c and friends are not walked */
        if (nftw(argv[i], clean_visit, 64, FTW_PHYS | FTW_MOUNT) != 0 && errno && errno != ENOENT)
            fprintf(stderr, "zonezap: %s: %s\n", argv[i], strerror(errno));
    }
    if (opt.dry_run)
        fprintf(stderr, "%lld file(s) would be removed (dry run)\n", clean_found);
    else
        fprintf(stderr, "%lld file(s) removed\n", clean_found);
    return 0;
}

/* ---------- status / info ---------- */

static int install_mode(char *out, size_t size)
{
    char buf[16384];
    if (access(UNIT_PATH, F_OK) == 0) {
        snprintf(out, size, "systemd service (%s)", UNIT_PATH);
        return 1;
    }
    if (read_file(WSLCONF_PATH, buf, sizeof buf) > 0 && strstr(buf, "zonezap")) {
        snprintf(out, size, "[boot] command in %s", WSLCONF_PATH);
        return 2;
    }
    snprintf(out, size, "not installed (no autostart)");
    return 0;
}

static int cmd_status(void)
{
    char mode[256];
    install_mode(mode, sizeof mode);
    int pid = running_pid();
    printf("ZoneZap %s\n", ZZ_VERSION);
    printf("Autostart: %s\n", mode);
    if (!pid) {
        printf("Watcher:   not running\n");
        return 3;
    }
    char st[4096], v[PATH_MAX + 64];
    read_file(STATUS_PATH, st, sizeof st);
    printf("Watcher:   running (pid %d)\n", pid);
    if (status_value(st, "watching", v, sizeof v) == 0)
        printf("Watching:  %s\n", v);
    if (status_value(st, "started", v, sizeof v) == 0) {
        time_t t = (time_t)atoll(v);
        char ts[64];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
        printf("Since:     %s\n", ts);
    }
    if (status_value(st, "removed", v, sizeof v) == 0)
        printf("Removed:   %s file(s)\n", v);
    if (status_value(st, "last", v, sizeof v) == 0 && v[0])
        printf("Last:      %s\n", v);
    return 0;
}

static void print_zone(const char *label, const char *buf, size_t len)
{
    zz_zone_info z;
    zz_parse_zone(buf, len, &z);
    printf("%s\n", label);
    if (z.zone_id >= 0)
        printf("  Zone:      %d (%s)\n", z.zone_id, zz_zone_name(z.zone_id));
    if (z.host_url[0])
        printf("  Came from: %s\n", z.host_url);
    if (z.referrer_url[0])
        printf("  Page:      %s\n", z.referrer_url);
    if (z.zone_id < 0 && !z.host_url[0])
        printf("  %.*s\n", (int)(len > 200 ? 200 : len), buf);
}

static int cmd_info(int argc, char **argv)
{
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        const char *f = argv[i];
        int any = 0;
        printf("%s\n", f);
        for (int s = 0; s < opt.streams.count; s++) {
            char xname[ZZ_MAX_STREAM_NAME + 32], val[4096];
            zz_xattr_name(opt.streams.names[s], xname, sizeof xname);
            ssize_t n = lgetxattr(f, xname, val, sizeof val - 1);
            if (n >= 0) {
                val[n] = 0;
                char label[128];
                snprintf(label, sizeof label, " %s (kept by ZoneZap):", opt.streams.names[s]);
                print_zone(label, val, (size_t)n);
                any = 1;
            }
            char side[PATH_MAX];
            snprintf(side, sizeof side, "%s:%s", f, opt.streams.names[s]);
            char sbuf[4096];
            int sn = read_file(side, sbuf, sizeof sbuf);
            if (sn >= 0) {
                char label[128];
                snprintf(label, sizeof label, " %s (still a separate file):", opt.streams.names[s]);
                print_zone(label, sbuf, (size_t)sn);
                any = 1;
            }
        }
        if (!any) {
            printf("  no Windows zone information\n");
            rc = 1;
        }
    }
    return rc;
}

/* ---------- install / uninstall ---------- */

static void self_exe(char *out, size_t size)
{
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n < 0)
        n = 0;
    out[n] = 0;
}

static int copy_self_to_bin(void)
{
    char me[PATH_MAX];
    self_exe(me, sizeof me);
    if (strcmp(me, BIN_PATH) == 0)
        return 0;
    int in = open(me, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    mkdir("/usr/local", 0755);
    mkdir("/usr/local/bin", 0755);
    char tmp[] = BIN_PATH ".new";
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) {
            ok = 0;
            break;
        }
    close(in);
    fchmod(out, 0755);
    close(out);
    if (!ok || rename(tmp, BIN_PATH) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void stop_running(void)
{
    int pid = running_pid();
    if (!pid)
        return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 50 && kill(pid, 0) == 0; i++)
        usleep(100000);
}

/* The run flags to bake into the autostart command, from the current options. */
static void run_flags(char *out, size_t size)
{
    out[0] = 0;
    zz_streams def;
    zz_streams_default(&def);
    for (int i = def.count; i < opt.streams.count; i++) {
        size_t l = strlen(out);
        snprintf(out + l, size - l, " --stream '%s'", opt.streams.names[i]);
    }
    if (!opt.keep_xattr)
        strncat(out, " --no-xattr", size - strlen(out) - 1);
    if (opt.verbose)
        strncat(out, " --verbose", size - strlen(out) - 1);
}

static int has_systemd(void) { return access("/run/systemd/system", F_OK) == 0; }

static int cmd_install(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "zonezap: install needs root. Try: sudo %s install\n", BIN_PATH);
        return 1;
    }
    for (int i = 0; i < opt.streams.count; i++)
        if (strchr(opt.streams.names[i], '\'') || strchr(opt.streams.names[i], ';')) {
            fprintf(stderr, "zonezap: stream names with quotes or ';' can't be installed\n");
            return 1;
        }
    stop_running();
    if (copy_self_to_bin() != 0) {
        fprintf(stderr, "zonezap: could not copy to %s: %s\n", BIN_PATH, strerror(errno));
        return 1;
    }
    printf("Copied to %s\n", BIN_PATH);
    char flags[1024];
    run_flags(flags, sizeof flags);

    if (has_systemd()) {
        char unit[2048];
        int n = snprintf(unit, sizeof unit,
                         "[Unit]\n"
                         "Description=ZoneZap: removes Windows :Zone.Identifier files\n"
                         "Documentation=https://github.com/BoazCohenJ/ZoneZap\n"
                         "After=local-fs.target\n\n"
                         "[Service]\n"
                         "ExecStart=%s run%s\n"
                         "Restart=on-failure\n\n"
                         "[Install]\n"
                         "WantedBy=multi-user.target\n",
                         BIN_PATH, flags);
        if (write_file_atomic(UNIT_PATH, unit, (size_t)n, 0644) != 0) {
            fprintf(stderr, "zonezap: could not write %s: %s\n", UNIT_PATH, strerror(errno));
            return 1;
        }
        char *reload[] = {"systemctl", "daemon-reload", NULL};
        char *enable[] = {"systemctl", "enable", "--now", "zonezap.service", NULL};
        char *restart[] = {"systemctl", "restart", "zonezap.service", NULL};
        run_prog(reload, 1);
        if (run_prog(enable, 1) != 0 || run_prog(restart, 1) != 0) {
            fprintf(stderr, "zonezap: systemctl could not start the service; see "
                            "'journalctl -u zonezap'\n");
            return 1;
        }
        printf("Installed as a systemd service (starts with this distro).\n");
    } else {
        char in[16384], out[20000];
        int had = read_file(WSLCONF_PATH, in, sizeof in);
        if (had < 0)
            in[0] = 0;
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "%s run --daemon%s", BIN_PATH, flags);
        if (zz_wslconf_add(in, cmd, out, sizeof out) < 0) {
            fprintf(stderr, "zonezap: %s is too large to edit\n", WSLCONF_PATH);
            return 1;
        }
        if (had > 0 && access(WSLCONF_PATH ".zonezap-bak", F_OK) != 0)
            write_file_atomic(WSLCONF_PATH ".zonezap-bak", in, (size_t)had, 0644);
        if (write_file_atomic(WSLCONF_PATH, out, strlen(out), 0644) != 0) {
            fprintf(stderr, "zonezap: could not write %s: %s\n", WSLCONF_PATH, strerror(errno));
            return 1;
        }
        printf("Added to the [boot] command in %s (starts with this distro).\n", WSLCONF_PATH);
        char *argv[16];
        int a = 0;
        argv[a++] = BIN_PATH;
        argv[a++] = "run";
        argv[a++] = "--daemon";
        zz_streams def;
        zz_streams_default(&def);
        for (int i = def.count; i < opt.streams.count && a < 12; i++) {
            argv[a++] = "--stream";
            argv[a++] = opt.streams.names[i];
        }
        if (!opt.keep_xattr)
            argv[a++] = "--no-xattr";
        if (opt.verbose)
            argv[a++] = "--verbose";
        argv[a] = NULL;
        run_prog(argv, 0);
    }
    for (int i = 0; i < 30 && !running_pid(); i++)
        usleep(100000);
    printf("\n");
    cmd_status();
    printf("\nRemove old leftovers with: sudo zonezap clean\n");
    return running_pid() ? 0 : 1;
}

static int cmd_uninstall(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "zonezap: uninstall needs root. Try: sudo zonezap uninstall\n");
        return 1;
    }
    if (access(UNIT_PATH, F_OK) == 0) {
        char *disable[] = {"systemctl", "disable", "--now", "zonezap.service", NULL};
        char *reload[] = {"systemctl", "daemon-reload", NULL};
        run_prog(disable, 1);
        unlink(UNIT_PATH);
        run_prog(reload, 1);
        printf("Removed the systemd service.\n");
    }
    char in[16384], out[20000];
    if (read_file(WSLCONF_PATH, in, sizeof in) > 0 && strstr(in, "zonezap")) {
        if (zz_wslconf_remove(in, out, sizeof out) >= 0 &&
            write_file_atomic(WSLCONF_PATH, out, strlen(out), 0644) == 0)
            printf("Removed ZoneZap from the [boot] command in %s.\n", WSLCONF_PATH);
    }
    stop_running();
    unlink(STATUS_PATH);
    if (unlink(BIN_PATH) == 0)
        printf("Removed %s.\n", BIN_PATH);
    printf("ZoneZap is uninstalled. Zone info it saved stays in user.windows.* xattrs.\n");
    return 0;
}

/* ---------- self-test ---------- */

static int wait_gone(const char *path, int timeout_ms, long long *took)
{
    long long t0 = now_ms();
    struct stat st;
    while (now_ms() - t0 < timeout_ms) {
        if (lstat(path, &st) != 0 && errno == ENOENT) {
            *took = now_ms() - t0;
            return 1;
        }
        usleep(1000);
    }
    *took = now_ms() - t0;
    return 0;
}

static int write_text(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, s, strlen(s));
    close(fd);
    return n == (ssize_t)strlen(s) ? 0 : -1;
}

static int xattr_has(const char *path, const char *needle)
{
    char v[4096];
    ssize_t n = lgetxattr(path, "user.windows.Zone.Identifier", v, sizeof v - 1);
    if (n < 0)
        return 0;
    v[n] = 0;
    return strstr(v, needle) != NULL;
}

static int capture(char *const argv[], char *out, size_t size)
{
    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t got = 0;
    ssize_t n;
    while (got + 1 < size && (n = read(pfd[0], out + got, size - 1 - got)) > 0)
        got += (size_t)n;
    out[got] = 0;
    close(pfd[0]);
    int st;
    waitpid(pid, &st, 0);
    while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r'))
        out[--got] = 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *find_powershell(void)
{
    static const char *c[] = {
        "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
        "/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe", NULL};
    for (int i = 0; c[i]; i++)
        if (access(c[i], X_OK) == 0)
            return c[i];
    return NULL;
}

static int cmd_self_test(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "zonezap: the self-test needs root. Try: sudo zonezap self-test\n");
        return 1;
    }
    int failed = 0;
    pid_t child = 0;
    if (running_pid()) {
        printf("Using the running watcher (pid %d).\n", running_pid());
    } else {
        printf("No watcher running; starting a temporary one.\n");
        child = fork();
        if (child == 0) {
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0)
                dup2(dn, 2);
            _exit(cmd_run(0));
        }
        for (int i = 0; i < 50 && !running_pid(); i++)
            usleep(100000);
        if (!running_pid()) {
            printf("FAIL  the watcher did not start (try: sudo zonezap run --verbose)\n");
            return 1;
        }
    }

    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "/var/tmp/zonezap-selftest-%d", (int)getpid());
    mkdir(dir, 0755);
    char f[PATH_MAX + 64], z[PATH_MAX + 96];
    long long took;
    usleep(200000);

    /* 1. a stream file written from Linux, the way the 9P server writes it */
    snprintf(f, sizeof f, "%s/linux.txt", dir);
    snprintf(z, sizeof z, "%s/linux.txt:Zone.Identifier", dir);
    write_text(f, "hello\n");
    write_text(z, "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://example.com/linux.txt\r\n");
    if (wait_gone(z, 3000, &took)) {
        printf("PASS  linux.txt:Zone.Identifier removed in %lld ms\n", took);
        if (xattr_has(f, "example.com/linux.txt"))
            printf("PASS  zone info kept in user.windows.Zone.Identifier\n");
        else if (opt.keep_xattr)
            printf("WARN  zone info was not kept as an xattr (filesystem without user xattrs?)\n");
    } else {
        printf("FAIL  linux.txt:Zone.Identifier still there after 3 s\n");
        failed++;
    }

    /* 2. safety: a look-alike file that isn't a Windows stream must stay */
    snprintf(z, sizeof z, "%s/notes:Zone.Identifier", dir);
    write_text(z, "my own notes, not from Windows\n");
    if (!wait_gone(z, 1500, &took))
        printf("PASS  a look-alike file with other content was kept\n");
    else {
        printf("FAIL  a look-alike file with other content was removed\n");
        failed++;
    }

    /* 3. the real thing: Windows copies a downloaded file into this distro */
    const char *ps = find_powershell();
    char winpath[PATH_MAX];
    char *wslpath[] = {"wslpath", "-w", dir, NULL};
    if (!ps || capture(wslpath, winpath, sizeof winpath) != 0 || !winpath[0]) {
        printf("SKIP  Windows copy test (no Windows interop here)\n");
    } else {
        char script[PATH_MAX * 2 + 512];
        snprintf(script, sizeof script,
                 "$ErrorActionPreference='Stop';"
                 "$s=Join-Path $env:TEMP ('zonezap-'+[guid]::NewGuid()+'.txt');"
                 "Set-Content -LiteralPath $s 'zonezap self-test';"
                 "Set-Content -LiteralPath $s -Stream Zone.Identifier -Value "
                 "\"[ZoneTransfer]`r`nZoneId=3`r`nHostUrl=https://example.com/from-windows.txt\";"
                 "Copy-Item -LiteralPath $s -Destination '%s\\from-windows.txt';"
                 "Remove-Item -LiteralPath $s;'copied'",
                 winpath);
        char *argv[] = {(char *)ps, "-NoProfile", "-NonInteractive", "-Command", script, NULL};
        char out[512];
        snprintf(f, sizeof f, "%s/from-windows.txt", dir);
        snprintf(z, sizeof z, "%s/from-windows.txt:Zone.Identifier", dir);
        int rc = capture(argv, out, sizeof out);
        struct stat st;
        if (rc != 0 || !strstr(out, "copied")) {
            printf("FAIL  Windows copy failed (exit %d): %s\n", rc, out);
            failed++;
        } else if (stat(f, &st) != 0) {
            printf("FAIL  the copied file is missing\n");
            failed++;
        } else if (wait_gone(z, 3000, &took)) {
            printf("PASS  Windows copy succeeded; its :Zone.Identifier file was removed "
                   "(gone %lld ms after the copy finished)\n", took);
            if (xattr_has(f, "from-windows.txt"))
                printf("PASS  zone info of the Windows copy kept as an xattr\n");
        } else {
            printf("FAIL  from-windows.txt:Zone.Identifier still there after 3 s\n");
            failed++;
        }
    }

    /* clean up */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                unlinkat(dirfd(d), e->d_name, 0);
        closedir(d);
    }
    rmdir(dir);
    if (child) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }
    printf(failed ? "\nSelf-test FAILED (%d)\n" : "\nSelf-test passed.\n", failed);
    return failed ? 1 : 0;
}

/* ---------- main ---------- */

static void usage(FILE *o)
{
    fprintf(o,
            "ZoneZap %s - removes the file:Zone.Identifier files Windows leaves in WSL\n\n"
            "Usage:\n"
            "  sudo zonezap install       Copy to " BIN_PATH " and start with this distro\n"
            "  sudo zonezap uninstall     Undo install\n"
            "  zonezap status             Is it running, and what has it removed\n"
            "  zonezap clean [PATH...]    Remove existing leftovers (default: / as root, else ~)\n"
            "  zonezap info FILE...       Show where a file came from (kept zone info)\n"
            "  sudo zonezap self-test     Copy a file from Windows and check it works\n"
            "  sudo zonezap run           Watch in the foreground (what install starts)\n\n"
            "Options:\n"
            "  --stream NAME   Also remove <file>:NAME (default: Zone.Identifier, SmartScreen)\n"
            "  --no-xattr      Don't keep the content in user.windows.<stream> xattrs\n"
            "  --dry-run       With clean: only list what would be removed\n"
            "  --daemon        With run: go to the background, log to " LOG_PATH "\n"
            "  --verbose       Log more\n"
            "  --version\n",
            ZZ_VERSION);
}

int main(int argc, char **argv)
{
    zz_streams_default(&opt.streams);
    opt.keep_xattr = 1;
    const char *cmd = NULL;
    int daemon = 0;
    char *rest[256];
    int nrest = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--stream") == 0 && i + 1 < argc) {
            if (zz_streams_add(&opt.streams, argv[++i]) != 0) {
                fprintf(stderr, "zonezap: bad or duplicate stream name '%s'\n", argv[i]);
                return 2;
            }
        } else if (strcmp(a, "--no-xattr") == 0) {
            opt.keep_xattr = 0;
        } else if (strcmp(a, "--dry-run") == 0 || strcmp(a, "-n") == 0) {
            opt.dry_run = 1;
        } else if (strcmp(a, "--daemon") == 0) {
            daemon = 1;
        } else if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
            opt.verbose = 1;
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "version") == 0) {
            printf("zonezap %s\n", ZZ_VERSION);
            return 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "help") == 0) {
            usage(stdout);
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "zonezap: unknown option %s\n\n", a);
            usage(stderr);
            return 2;
        } else if (!cmd) {
            cmd = a;
        } else if (nrest < 256) {
            rest[nrest++] = argv[i];
        }
    }
    if (!cmd) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "run") == 0)
        return cmd_run(daemon);
    if (strcmp(cmd, "clean") == 0)
        return cmd_clean(nrest, rest);
    if (strcmp(cmd, "status") == 0)
        return cmd_status();
    if (strcmp(cmd, "info") == 0) {
        if (!nrest) {
            fprintf(stderr, "zonezap: info needs a file\n");
            return 2;
        }
        return cmd_info(nrest, rest);
    }
    if (strcmp(cmd, "install") == 0)
        return cmd_install();
    if (strcmp(cmd, "uninstall") == 0)
        return cmd_uninstall();
    if (strcmp(cmd, "self-test") == 0)
        return cmd_self_test();
    fprintf(stderr, "zonezap: unknown command '%s'\n\n", cmd);
    usage(stderr);
    return 2;
}
