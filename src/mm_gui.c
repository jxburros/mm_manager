/*
 * mm_gui: a local control panel for mm_manager.
 *
 * Serves html/table_editor.html on http://127.0.0.1:<port>/ and gives it a
 * small API, so that everything can be done from the browser instead of a
 * terminal: edit tables and config files, find modems, start and stop
 * mm_manager and watch its output, look through mm_manager.db, convert
 * transcripts, and run the table generator scripts.
 *
 * Only connections from this computer are accepted.  Every API request must
 * carry the random token that is put into the page when it is served, and the
 * Host header must name 127.0.0.1 or localhost, so other web sites can't drive
 * the API from the browser.
 *
 * Optionally, there are accounts (see "Accounts" below): once an admin account
 * exists, everyone signs in, users only reach the phones and settings an admin
 * gave them, and -L lets other computers sign in too.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#if defined(_WIN32)
# define _CRT_RAND_S    /* rand_s() */
#elif !defined(__APPLE__)
# define _GNU_SOURCE    /* POSIX and BSD declarations even with -std=c11 */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "mm_osal.h"
#include "mm_sha256.h"
#include "sqlite3.h"

#ifdef _WIN32
# include <direct.h>
# include <shellapi.h>
# include <shlobj.h>
# ifdef _MSC_VER
#  pragma comment(lib, "shell32.lib")
# endif
# define PLATFORM_NAME  "windows"
# define EXE_SUFFIX     ".exe"
typedef struct _stat64 stat_t;
# define S_ISDIR_(m)    (((m) & _S_IFMT) == _S_IFDIR)
# define S_ISREG_(m)    (((m) & _S_IFMT) == _S_IFREG)
# define do_stat(p, s)  _stat64((p), (s))
# define do_mkdir(p)    _mkdir(p)
# define do_rmdir(p)    _rmdir(p)
# define do_chdir(p)    _chdir(p)
# define do_getcwd(b, n) _getcwd((b), (int)(n))
#else
# include <dirent.h>
# include <fcntl.h>
# include <signal.h>
# include <spawn.h>
# include <sys/wait.h>
# ifdef __APPLE__
#  include <mach-o/dyld.h>
#  define PLATFORM_NAME "macos"
# else
#  define PLATFORM_NAME "linux"
# endif
# define EXE_SUFFIX     ""
typedef struct stat stat_t;
# define S_ISDIR_(m)    S_ISDIR(m)
# define S_ISREG_(m)    S_ISREG(m)
# define do_stat(p, s)  stat((p), (s))
# define do_mkdir(p)    mkdir((p), 0777)
# define do_rmdir(p)    rmdir(p)
# define do_chdir(p)    chdir(p)
# define do_getcwd(b, n) getcwd((b), (n))
extern char **environ;
#endif /* _WIN32 */

#ifndef MM_SOURCE_DIR
# define MM_SOURCE_DIR  ""
#endif

#define DEFAULT_PORT        8273
#define PATH_LEN            1024
#define MAX_HEADER          16384
#define MAX_BODY            (64 * 1024 * 1024)
#define MAX_ARGS            64
#define JOB_BUF_MAX         (1024 * 1024)
#define STOP_GRACE_MS       5000
#define MAX_LIST_ENTRIES    5000
#define DB_FILE             "mm_manager.db"
#define CONFIGS_DIR         "configurations"    /* saved phone configurations, one folder each */

/* Python packages that the table generators import. */
static const char *python_packages[] = { "pandas", "requests", "xmltodict" };

/* ---------------------------------------------------------------------
 * Growable string buffer
 * --------------------------------------------------------------------- */

typedef struct {
    char   *s;
    size_t  len;
    size_t  cap;
} sbuf_t;

static void sb_grow(sbuf_t *b, size_t need) {
    size_t cap = b->cap ? b->cap : 256;
    char  *s;

    if (b->len + need + 1 <= b->cap) {
        return;
    }
    while (cap < b->len + need + 1) {
        cap *= 2;
    }
    s = (char *)realloc(b->s, cap);
    if (s == NULL) {
        fprintf(stderr, "mm_gui: out of memory.\n");
        exit(-ENOMEM);
    }
    b->s   = s;
    b->cap = cap;
}

static void sb_add(sbuf_t *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sb_puts(sbuf_t *b, const char *s) {
    sb_add(b, s, strlen(s));
}

static void sb_printf(sbuf_t *b, const char *fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    sb_grow(b, (size_t)n);
    va_start(ap, fmt);
    (void)vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* Append s as a JSON string, with quotes. */
static void sb_json(sbuf_t *b, const char *s, size_t n) {
    sb_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
            case '"':  sb_add(b, "\\\"", 2); break;
            case '\\': sb_add(b, "\\\\", 2); break;
            case '\n': sb_add(b, "\\n", 2);  break;
            case '\r': sb_add(b, "\\r", 2);  break;
            case '\t': sb_add(b, "\\t", 2);  break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    sb_printf(b, "\\u%04x", c);
                } else if (c >= 0x80) {
                    /* Pass UTF-8 through; show any other high byte as Latin-1. */
                    size_t len = (c >= 0xf0) ? 4 : (c >= 0xe0) ? 3 : (c >= 0xc0) ? 2 : 0;
                    size_t k;

                    for (k = 1; len && k < len; k++) {
                        if (i + k >= n || ((unsigned char)s[i + k] & 0xc0) != 0x80) {
                            break;
                        }
                    }
                    if (len && k == len) {
                        sb_add(b, &s[i], len);
                        i += len - 1;
                    } else {
                        sb_printf(b, "\\u%04x", c);
                    }
                } else {
                    sb_add(b, (const char *)&c, 1);
                }
                break;
        }
    }
    sb_add(b, "\"", 1);
}

static void sb_jstr(sbuf_t *b, const char *s) {
    sb_json(b, s, strlen(s));
}

/* ---------------------------------------------------------------------
 * Paths and files
 * --------------------------------------------------------------------- */

static char g_root[PATH_LEN];        /* working folder: tables/, config/, mm_manager.db */
static char g_bindir[PATH_LEN];      /* where mm_manager and the other tools are */
static char g_html[PATH_LEN];        /* table_editor.html */
static char g_scripts[PATH_LEN];     /* where the generate_*.py scripts are */
static char g_token[33];
static int  g_port = DEFAULT_PORT;
static char g_python[2][16];         /* e.g. { "python3" } or { "py", "-3" } */
static int  g_python_argc;
static char g_python_version[64];
static volatile int g_running = 1;

static int is_dir(const char *path) {
    stat_t st;

    return (do_stat(path, &st) == 0) && (S_ISDIR_(st.st_mode));
}

static int is_file(const char *path) {
    stat_t st;

    return (do_stat(path, &st) == 0) && (S_ISREG_(st.st_mode));
}

/* snprintf() that is fine with cutting src short (paths here are far shorter than PATH_LEN). */
static void copy_str(char *out, size_t len, const char *src) {
    if (snprintf(out, len, "%s", src) < 0) {
        out[0] = '\0';
    }
}

static void join_path(char *out, size_t len, const char *dir, const char *name) {
    if (dir[0] == '\0') {
        copy_str(out, len, name);
    } else if (snprintf(out, len, "%s/%s", dir, name) < 0) {
        out[0] = '\0';
    }
}

static void dir_name(char *path) {
    char *slash = strrchr(path, '/');
#ifdef _WIN32
    char *bslash = strrchr(path, '\\');

    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
#endif /* _WIN32 */
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(path, 2, ".");
    }
}

static void exe_dir(char *out, size_t len, const char *argv0) {
    out[0] = '\0';
#if defined(_WIN32)
    (void)argv0;
    if (GetModuleFileNameA(NULL, out, (DWORD)len) == 0) {
        out[0] = '\0';
    }
#elif defined(__APPLE__)
    {
        char     raw[PATH_LEN];
        uint32_t size = sizeof(raw);

        if (_NSGetExecutablePath(raw, &size) == 0) {
            copy_str(out, len, raw);
        }
    }
#else
    {
        ssize_t n = readlink("/proc/self/exe", out, len - 1);

        out[(n > 0) ? n : 0] = '\0';
    }
#endif
    if (out[0] == '\0') {
        copy_str(out, len, argv0);
    }
    dir_name(out);
}

/* Make a path absolute, so that it still works after chdir(). */
static void absolute(char *path, size_t len) {
#ifdef _WIN32
    char tmp[PATH_LEN];

    if (_fullpath(tmp, path, sizeof(tmp)) != NULL) {
        copy_str(path, len, tmp);
    }
#else
    char *tmp = realpath(path, NULL);

    if (tmp != NULL) {
        copy_str(path, len, tmp);
        free(tmp);
    }
#endif /* _WIN32 */
}

/*
 * A path from the browser must stay inside the working folder: relative,
 * no "..", no drive letters, no empty or hidden components.
 */
static int safe_path(const char *p, int allow_empty) {
    const char *c = p;

    if (p[0] == '\0') {
        return allow_empty;
    }
    if (strlen(p) > 512 || p[0] == '/' || p[0] == '\\' || strchr(p, ':') != NULL) {
        return 0;
    }
    while (*c) {
        const char *end = c;
        size_t      n;

        while (*end && *end != '/' && *end != '\\') {
            end++;
        }
        n = (size_t)(end - c);
        if (n == 0 || c[0] == '.') {
            return 0;
        }
        for (size_t i = 0; i < n; i++) {
            if ((unsigned char)c[i] < 0x20) {
                return 0;
            }
        }
        c = *end ? end + 1 : end;
        if (*end && *c == '\0') {
            return 0;   /* trailing slash */
        }
    }
    return 1;
}

static int mkdir_parents(const char *path) {
    char tmp[PATH_LEN];

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp; *p; p++) {
        /* Skip the root of an absolute path ("/" or "C:\\"). */
        if ((*p == '/' || *p == '\\') && p > tmp && p[-1] != ':') {
            char save = *p;

            *p = '\0';
            if (!is_dir(tmp) && do_mkdir(tmp) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = save;
        }
    }
    return 0;
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE          *f = fopen(path, "rb");
    unsigned char *data;
    long           size;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    *len = fread(data, 1, (size_t)size, f);
    data[*len] = '\0';
    fclose(f);
    return data;
}

static int write_file(const char *path, const char *data, size_t len) {
    FILE *f;

    if (mkdir_parents(path) != 0 || (f = fopen(path, "wb")) == NULL) {
        return -1;
    }
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f);
}

/* List a folder as JSON, descending up to depth levels.  keep(ctx, path), if given, picks the entries to show. */
typedef int (*keep_fn)(const void *ctx, const char *path);

static void list_dir(sbuf_t *out, const char *rel, int depth, int *count, keep_fn keep, const void *ctx) {
    char path[PATH_LEN];

    snprintf(path, sizeof(path), "%s", rel[0] ? rel : ".");
#ifdef _WIN32
    {
        char             pattern[PATH_LEN];
        WIN32_FIND_DATAA fd;
        HANDLE           h;

        join_path(pattern, sizeof(pattern), path, "*");
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            char     child[PATH_LEN];
            int      dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            uint64_t ft = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            int64_t  mtime = (int64_t)(ft / 10000000ULL) - 11644473600LL;

            if (fd.cFileName[0] == '.' || *count >= MAX_LIST_ENTRIES) {
                continue;
            }
            join_path(child, sizeof(child), rel, fd.cFileName);
            if (keep && !keep(ctx, child)) {
                continue;
            }
            sb_puts(out, (*count)++ ? ",{\"path\":" : "{\"path\":");
            sb_jstr(out, child);
            sb_printf(out, ",\"dir\":%s,\"size\":%llu,\"mtime\":%lld}", dir ? "true" : "false",
                      (unsigned long long)size, (long long)mtime);
            if (dir && depth > 0) {
                list_dir(out, child, depth - 1, count, keep, ctx);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR           *d = opendir(path);
        struct dirent *e;

        if (d == NULL) {
            return;
        }
        while ((e = readdir(d)) != NULL) {
            char   child[PATH_LEN];
            stat_t st;
            int    dir;

            if (e->d_name[0] == '.' || *count >= MAX_LIST_ENTRIES) {
                continue;
            }
            join_path(child, sizeof(child), rel, e->d_name);
            if ((keep && !keep(ctx, child)) || do_stat(child, &st) != 0) {
                continue;
            }
            dir = S_ISDIR_(st.st_mode);
            sb_puts(out, (*count)++ ? ",{\"path\":" : "{\"path\":");
            sb_jstr(out, child);
            sb_printf(out, ",\"dir\":%s,\"size\":%llu,\"mtime\":%lld}", dir ? "true" : "false",
                      (unsigned long long)st.st_size, (long long)st.st_mtime);
            if (dir && depth > 0) {
                list_dir(out, child, depth - 1, count, keep, ctx);
            }
        }
        closedir(d);
    }
#endif /* _WIN32 */
}

/* ---------------------------------------------------------------------
 * Child processes ("jobs")
 *
 * There are two: "manager" runs mm_manager itself, and "task" runs one-off
 * commands such as a modem scan or a table generator.  Output from stdout
 * and stderr is collected by a reader thread, keeping the last JOB_BUF_MAX
 * bytes; the browser polls for what it hasn't seen yet.
 * --------------------------------------------------------------------- */

typedef struct {
    const char    *name;
    osal_mutex_t   lock;
    int            running;
    int            started;
    int            exit_code;
    int            stopping;
    uint64_t       stop_at;
    char           title[1024];
    char          *buf;
    size_t         len;
    uint64_t       base;          /* output offset of buf[0] */
    osal_thread_t  reader;
    int            reader_valid;
#ifdef _WIN32
    HANDLE         process;
    HANDLE         out;
#else
    pid_t          pid;
    int            out;
#endif /* _WIN32 */
} job_t;

static job_t jobs[3] = { { .name = "manager" }, { .name = "task" }, { .name = "update" } };

static job_t *find_job(const char *name) {
    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
        if (strcmp(jobs[i].name, name) == 0) {
            return &jobs[i];
        }
    }
    return NULL;
}

static void job_append(job_t *job, const char *data, size_t n) {
    osal_mutex_lock(&job->lock);
    if (job->len + n > JOB_BUF_MAX) {
        size_t drop = job->len + n - JOB_BUF_MAX / 2;

        if (drop > job->len) {
            /* A single huge chunk: keep its tail. */
            data += drop - job->len;
            n    -= drop - job->len;
            job->base += drop;
            job->len   = 0;
        } else {
            memmove(job->buf, job->buf + drop, job->len - drop);
            job->len  -= drop;
            job->base += drop;
        }
    }
    memcpy(job->buf + job->len, data, n);
    job->len += n;
    osal_mutex_unlock(&job->lock);
}

static osal_thread_ret_t OSAL_THREAD_CALL job_reader(void *arg) {
    job_t *job = (job_t *)arg;
    char   chunk[4096];
    char   msg[96];
    int    code = -1;

#ifdef _WIN32
    DWORD n;
    DWORD status;

    while (ReadFile(job->out, chunk, sizeof(chunk), &n, NULL) && n > 0) {
        job_append(job, chunk, n);
    }
    CloseHandle(job->out);
    WaitForSingleObject(job->process, INFINITE);
    if (GetExitCodeProcess(job->process, &status)) {
        code = (int)status;
    }
#else
    ssize_t   n;
    siginfo_t info;

    while ((n = read(job->out, chunk, sizeof(chunk))) != 0) {
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        job_append(job, chunk, (size_t)n);
    }
    close(job->out);
    /* Wait without reaping, so that the pid can't be reused while job->running is set. */
    memset(&info, 0, sizeof(info));
    while (waitid(P_PID, (id_t)job->pid, &info, WEXITED | WNOWAIT) != 0 && errno == EINTR) {
    }
    code = (info.si_code == CLD_EXITED) ? info.si_status : 128 + info.si_status;
#endif /* _WIN32 */

    /* The last words go in before running is cleared, so a poll that sees it stopped has them. */
    snprintf(msg, sizeof(msg), "\n[finished, exit code %d]\n", code);
    job_append(job, msg, strlen(msg));

    osal_mutex_lock(&job->lock);
#ifdef _WIN32
    CloseHandle(job->process);
    job->process = NULL;
#else
    (void)waitpid(job->pid, NULL, 0);
#endif /* _WIN32 */
    job->running   = 0;
    job->exit_code = code;
    osal_mutex_unlock(&job->lock);
    return (osal_thread_ret_t)0;
}

#ifdef _WIN32
/* Quote one argument the way the Microsoft C runtime splits command lines. */
static void win_quote(sbuf_t *b, const char *arg) {
    if (arg[0] != '\0' && strpbrk(arg, " \t\n\v\"") == NULL) {
        sb_puts(b, arg);
        return;
    }
    sb_add(b, "\"", 1);
    for (const char *p = arg; ; p++) {
        size_t slashes = 0;

        while (*p == '\\') {
            slashes++;
            p++;
        }
        if (*p == '\0') {
            for (size_t i = 0; i < slashes * 2; i++) sb_add(b, "\\", 1);
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) sb_add(b, "\\", 1);
        } else {
            for (size_t i = 0; i < slashes; i++) sb_add(b, "\\", 1);
        }
        sb_add(b, p, 1);
    }
    sb_add(b, "\"", 1);
}
#endif /* _WIN32 */

/*
 * Start argv[] as job.  env_name/env_value, if given, are added to the
 * child's environment only.  Returns 0, or an error message.
 */
static const char *job_start(job_t *job, char **argv, const char *env_name, const char *env_value) {
    static char err[256];
    sbuf_t      title = { 0 };

    osal_mutex_lock(&job->lock);
    if (job->running) {
        osal_mutex_unlock(&job->lock);
        return "it is already running";
    }
    osal_mutex_unlock(&job->lock);

    if (job->reader_valid) {
        osal_thread_join(job->reader);
        job->reader_valid = 0;
    }

    for (int i = 0; argv[i]; i++) {
        if (i) sb_add(&title, " ", 1);
        sb_puts(&title, argv[i]);
    }

#ifdef _WIN32
    {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;
        HANDLE              rd, wr, nul;
        sbuf_t              cmd = { 0 };
        BOOL                ok;

        for (int i = 0; argv[i]; i++) {
            if (i) sb_add(&cmd, " ", 1);
            win_quote(&cmd, argv[i]);
        }
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            free(cmd.s);
            free(title.s);
            return "couldn't create a pipe";
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        memset(&si, 0, sizeof(si));
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = nul;
        si.hStdOutput = wr;
        si.hStdError  = wr;
        if (env_name) {
            SetEnvironmentVariableA(env_name, env_value);
        }
        ok = CreateProcessA(NULL, cmd.s, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        if (env_name) {
            SetEnvironmentVariableA(env_name, NULL);
        }
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) {
            CloseHandle(nul);
        }
        free(cmd.s);
        if (!ok) {
            snprintf(err, sizeof(err), "couldn't start %s (Windows error %lu)", argv[0], GetLastError());
            CloseHandle(rd);
            free(title.s);
            return err;
        }
        CloseHandle(pi.hThread);
        job->process = pi.hProcess;
        job->out     = rd;
    }
#else
    {
        int                        fds[2];
        int                        rc;
        pid_t                      pid;
        posix_spawn_file_actions_t fa;
        posix_spawnattr_t          attr;
        sigset_t                   sigs;

        if (pipe(fds) != 0) {
            free(title.s);
            return "couldn't create a pipe";
        }
        /* dup2() in the child clears close-on-exec on its stdout and stderr. */
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
        posix_spawnattr_init(&attr);
        sigemptyset(&sigs);
        posix_spawnattr_setsigmask(&attr, &sigs);
        sigaddset(&sigs, SIGPIPE);
        sigaddset(&sigs, SIGINT);
        posix_spawnattr_setsigdefault(&attr, &sigs);
        posix_spawnattr_setpgroup(&attr, 0);    /* its own group, so stop reaches its children too */
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
        if (env_name) {
            setenv(env_name, env_value, 1);
        }
        rc = posix_spawnp(&pid, argv[0], &fa, &attr, argv, environ);
        if (env_name) {
            unsetenv(env_name);
        }
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);
        close(fds[1]);
        if (rc != 0) {
            snprintf(err, sizeof(err), "couldn't start %s: %s", argv[0], strerror(rc));
            close(fds[0]);
            free(title.s);
            return err;
        }
        job->pid = pid;
        job->out = fds[0];
    }
#endif /* _WIN32 */

    osal_mutex_lock(&job->lock);
    snprintf(job->title, sizeof(job->title), "%s", title.s);
    job->running   = 1;
    job->started   = 1;
    job->stopping  = 0;
    job->exit_code = 0;
    job->base     += job->len;   /* offsets keep counting up across runs */
    job->len       = 0;
    osal_mutex_unlock(&job->lock);
    free(title.s);

    if (osal_thread_create(&job->reader, job_reader, job) != 0) {
        return "couldn't start the output reader";
    }
    job->reader_valid = 1;
    return NULL;
}

static void job_stop(job_t *job) {
    osal_mutex_lock(&job->lock);
    if (job->running && !job->stopping) {
        job->stopping = 1;
        job->stop_at  = osal_time_ms() + STOP_GRACE_MS;
#ifdef _WIN32
        /* Child processes have no console to send ^C to. */
        TerminateProcess(job->process, 1);
#else
        kill(-job->pid, SIGINT);
#endif /* _WIN32 */
    }
    osal_mutex_unlock(&job->lock);
}

/* Called about twice a second: kill jobs that ignored their stop request. */
static void jobs_tick(void) {
    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
        job_t *job = &jobs[i];

        osal_mutex_lock(&job->lock);
        if (job->running && job->stopping && osal_time_ms() > job->stop_at) {
#ifdef _WIN32
            TerminateProcess(job->process, 1);
#else
            kill(-job->pid, SIGKILL);
#endif /* _WIN32 */
            job->stop_at = osal_time_ms() + STOP_GRACE_MS;
        }
        osal_mutex_unlock(&job->lock);
    }
}

/* Run argv[] to completion and capture its output (used to find Python). */
static int run_capture(char **argv, char *out, size_t len) {
    static job_t probe = { .name = "probe" };
    int          code;

    if (probe.buf == NULL) {
        osal_mutex_init(&probe.lock);
        probe.buf = (char *)malloc(JOB_BUF_MAX);
    }
    if (job_start(&probe, argv, NULL, NULL) != NULL) {
        return -1;
    }
    osal_thread_join(probe.reader);
    probe.reader_valid = 0;
    code = probe.exit_code;
    snprintf(out, len, "%.*s", (int)((probe.len < len) ? probe.len : len - 1), probe.buf);
    return code;
}

static void find_python(void) {
    static const char *candidates[][2] = {
#ifdef _WIN32
        { "py", "-3" }, { "python", NULL }, { "python3", NULL },
#else
        { "python3", NULL }, { "python", NULL },
#endif /* _WIN32 */
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *argv[4];
        char  out[256];
        int   n = 0;

        argv[n++] = (char *)candidates[i][0];
        if (candidates[i][1]) argv[n++] = (char *)candidates[i][1];
        argv[n++] = "--version";
        argv[n]   = NULL;
        if (run_capture(argv, out, sizeof(out)) == 0 && strncmp(out, "Python 3", 8) == 0) {
            out[strcspn(out, "\r\n")] = '\0';
            copy_str(g_python_version, sizeof(g_python_version), out);
            g_python_argc = 0;
            for (int k = 0; k < 2 && candidates[i][k]; k++) {
                snprintf(g_python[g_python_argc++], sizeof(g_python[0]), "%s", candidates[i][k]);
            }
            return;
        }
    }
}

/* ---------------------------------------------------------------------
 * HTTP
 * --------------------------------------------------------------------- */

typedef struct account account_t;

typedef struct {
    osal_socket_t sock;
    int           local;            /* the request came from this computer */
    account_t    *who;              /* who is signed in; NULL with no accounts (anyone on this computer) */
    char          session[80];
    char          method[8];
    char          path[256];
    char          query[2048];
    char          host[256];
    char          token[64];
    char          sip_password[256];
    char         *body;
    size_t        body_len;
} request_t;

static int send_all(osal_socket_t s, const char *data, size_t len) {
    while (len > 0) {
        int n = (int)send(s, data, (int)((len > 65536) ? 65536 : len), 0);

        if (n <= 0) {
            return -1;
        }
        data += n;
        len  -= (size_t)n;
    }
    return 0;
}

static void respond(request_t *req, int status, const char *type, const char *body, size_t len) {
    char        head[512];
    const char *reason = (status == 200) ? "OK" : (status == 400) ? "Bad Request" : (status == 401) ? "Unauthorized" : (status == 403) ? "Forbidden" :
                         (status == 404) ? "Not Found" : (status == 409) ? "Conflict" : (status == 413) ? "Payload Too Large" : (status == 429) ? "Too Many Requests" :
                         "Internal Server Error";
    int         n = snprintf(head, sizeof(head),
                             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                             "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                             "Referrer-Policy: no-referrer\r\nConnection: close\r\n\r\n",
                             status, reason, type, (unsigned long long)len);

    if (send_all(req->sock, head, (size_t)n) == 0 && len > 0 && strcmp(req->method, "HEAD") != 0) {
        (void)send_all(req->sock, body, len);
    }
}

static void respond_json(request_t *req, int status, sbuf_t *b) {
    respond(req, status, "application/json; charset=utf-8", b->s ? b->s : "", b->len);
    free(b->s);
}

static void respond_error(request_t *req, int status, const char *msg) {
    sbuf_t b = { 0 };

    sb_puts(&b, "{\"ok\":false,\"error\":");
    sb_jstr(&b, msg);
    sb_puts(&b, "}");
    respond_json(req, status, &b);
}

static void respond_ok(request_t *req) {
    sbuf_t b = { 0 };

    sb_puts(&b, "{\"ok\":true}");
    respond_json(req, 200, &b);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Find key in the query string and URL-decode its value.  Returns 1 if found. */
static int query_param(const request_t *req, const char *key, char *out, size_t len) {
    const char *p   = req->query;
    size_t      klen = strlen(key);

    out[0] = '\0';
    while (*p) {
        const char *end = strchr(p, '&');

        if (end == NULL) {
            end = p + strlen(p);
        }
        if ((size_t)(end - p) >= klen && strncmp(p, key, klen) == 0 && (p[klen] == '=' || p + klen == end)) {
            size_t o = 0;

            for (p += klen + (p[klen] == '='); p < end && o + 1 < len; p++) {
                if (*p == '+') {
                    out[o++] = ' ';
                } else if (*p == '%' && p + 2 < end + 1 && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                    out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
                    p += 2;
                } else {
                    out[o++] = *p;
                }
            }
            out[o] = '\0';
            return 1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

static void header_value(const char *headers, const char *name, char *out, size_t len) {
    size_t      nlen = strlen(name);
    const char *p    = headers;

    out[0] = '\0';
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
#ifdef _WIN32
        if (_strnicmp(p, name, nlen) == 0 && p[nlen] == ':') {
#else
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
#endif /* _WIN32 */
            const char *v   = p + nlen + 1;
            const char *end = strstr(v, "\r\n");
            size_t      n;

            while (*v == ' ' || *v == '\t') {
                v++;
            }
            n = end ? (size_t)(end - v) : strlen(v);
            snprintf(out, len, "%.*s", (int)((n < len) ? n : len - 1), v);
            return;
        }
    }
}

/* Read one request.  Returns 0 on success, or an HTTP status to fail with. */
static int read_request(request_t *req) {
    static char head[MAX_HEADER + 1];
    size_t      got = 0;
    char       *end = NULL;
    char        clen[32];
    char        target[2304];
    size_t      have;
    long long   want;
    char       *q;

    while (end == NULL) {
        int n;

        if (got >= MAX_HEADER) {
            return 413;
        }
        n = (int)recv(req->sock, head + got, (int)(MAX_HEADER - got), 0);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
        head[got] = '\0';
        end = strstr(head, "\r\n\r\n");
    }
    *end = '\0';
    if (sscanf(head, "%7s %2303s", req->method, target) != 2) {
        return 400;
    }
    q = strchr(target, '?');
    if (q) {
        *q = '\0';
        copy_str(req->query, sizeof(req->query), q + 1);
    }
    copy_str(req->path, sizeof(req->path), target);
    header_value(head, "Host", req->host, sizeof(req->host));
    header_value(head, "X-MM-Token", req->token, sizeof(req->token));
    header_value(head, "X-MM-Session", req->session, sizeof(req->session));
    header_value(head, "X-MM-SIP-Password", req->sip_password, sizeof(req->sip_password));
    header_value(head, "Content-Length", clen, sizeof(clen));

    want = atoll(clen);
    if (want < 0 || want > MAX_BODY) {
        return 413;
    }
    req->body_len = (size_t)want;
    req->body = (char *)malloc(req->body_len + 1);
    if (req->body == NULL) {
        return 413;
    }
    have = got - (size_t)((end + 4) - head);
    if (have > req->body_len) {
        have = req->body_len;
    }
    memcpy(req->body, end + 4, have);
    while (have < req->body_len) {
        int n = (int)recv(req->sock, req->body + have, (int)((req->body_len - have > 65536) ? 65536 : req->body_len - have), 0);

        if (n <= 0) {
            return -1;
        }
        have += (size_t)n;
    }
    req->body[req->body_len] = '\0';
    return 0;
}

/*
 * Only accept requests addressed to this computer by name (stops DNS rebinding),
 * or to the address given with -L.
 */
static char g_listen[64];

static int host_ok(const char *host) {
    char ok[4][96];

    snprintf(ok[0], sizeof(ok[0]), "127.0.0.1:%d", g_port);
    snprintf(ok[1], sizeof(ok[1]), "localhost:%d", g_port);
    snprintf(ok[2], sizeof(ok[2]), "[::1]:%d", g_port);
    snprintf(ok[3], sizeof(ok[3]), "%s:%d", g_listen[0] ? g_listen : "127.0.0.1", g_port);
    for (int i = 0; i < 4; i++) {
        if (strcmp(host, ok[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * Accounts
 *
 * With no accounts, the control panel works as it always has: anyone on
 * this computer can use all of it, and nobody else can reach it.  Once an
 * admin adds an account, everyone signs in.  An admin can use everything.
 * A user only sees the phones an admin gave them, and on each phone only the
 * parts the admin ticked: its overview (name, call-in schedule, waiting
 * update), its records, and each group of settings.  The checks are made
 * here, not only in the page.
 *
 * The accounts live in .mm_gui/accounts in the working folder.  Folders whose
 * names start with "." can't be reached through /api/file, so the page can't
 * read or change it directly.  Deleting the file turns sign-in off again.
 * --------------------------------------------------------------------- */

#define ACCOUNTS_FILE       ".mm_gui/accounts"
#define MAX_ACCOUNTS        64
#define MAX_GRANTS          256         /* phones per account */
#define MAX_SESSIONS        128
#define SESSION_IDLE_S      (8 * 3600)  /* signed out after this long without a request */
#define PBKDF2_ITERATIONS   100000
#define MIN_PASSWORD        8
#define MAX_PASSWORD        128

/* What a user may do on a phone.  The groups match SETTING_GROUPS in html/table_editor.html. */
enum {
    P_OVERVIEW, P_RECORDS, P_DISPLAY, P_FEATURES, P_PRICES, P_NUMBERS, P_LOCAL, P_CARDS, P_COINS, P_ADVANCED, P_COUNT
};
#define P_ALL   ((1u << P_COUNT) - 1)
static const char *perm_names[P_COUNT] = {
    "overview", "records", "display", "features", "prices", "numbers", "local", "cards", "coins", "advanced",
};

/* The group of settings a table belongs to (keep in step with SETTING_GROUPS in the page). */
static int table_group(unsigned id) {
    switch (id) {
        case 0x1d: case 0x1b: case 0x1c: case 0x55: case 0x56:
            return P_DISPLAY;
        case 0x1a: case 0x1e: case 0x3a:
            return P_FEATURES;
        case 0x49: case 0x97:
            return P_PRICES;
        case 0x5c: case 0x3b: case 0x18: case 0x37:
            return P_NUMBERS;
        case 0x86: case 0x16: case 0x87: case 0x17: case 0x5d:
            return P_CARDS;
        case 0x32:
            return P_COINS;
        case 0x96:
        case 0x5a: case 0x5b: case 0xa0: case 0xa1: case 0x9a: case 0x9b:
            return P_LOCAL;
        default:
            /* Local call determination: 0x4a-0x51, compressed 0x65-0x73, double-compressed 0x88-0x95. */
            if ((id >= 0x4a && id <= 0x51) || (id >= 0x65 && id <= 0x73) || (id >= 0x88 && id <= 0x95)) {
                return P_LOCAL;
            }
            return P_ADVANCED;
    }
}

typedef struct {
    char     id[11];
    unsigned perms;
} grant_t;

struct account {
    char     name[33];
    int      admin;
    char     pass[192];     /* pbkdf2-sha256$<iterations>$<salt>$<hash> */
    int      ngrants;
    grant_t  grants[MAX_GRANTS];
    int      fails;         /* wrong passwords in a row */
    time_t   locked_until;
};

typedef struct {
    char   token[33];
    char   user[33];
    time_t last;
} session_t;

static account_t g_accounts[MAX_ACCOUNTS];
static int       g_naccounts;
static char      g_phones_dir[256] = "tables";  /* where each phone has a folder, as mm_manager -t */
static session_t g_sessions[MAX_SESSIONS];
static int       g_listen_any;                  /* -L 0.0.0.0: any address this computer has */

static void random_bytes(uint8_t *out, size_t n) {
#ifdef _WIN32
    for (size_t i = 0; i < n; i += 4) {
        unsigned int v = 0;

        if (rand_s(&v) != 0) {
            v = osal_random32();
        }
        for (size_t k = 0; k < 4 && i + k < n; k++) out[i + k] = (uint8_t)(v >> (8 * k));
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");

    if (f == NULL || fread(out, 1, n, f) != n) {
        for (size_t i = 0; i < n; i++) {
            out[i] ^= (uint8_t)osal_random32();
        }
    }
    if (f) fclose(f);
#endif /* _WIN32 */
}

static void to_hex(char *out, const uint8_t *in, size_t n) {
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", in[i]);
    }
    out[2 * n] = '\0';
}

static int from_hex(uint8_t *out, size_t max, const char *in) {
    size_t n = strlen(in);

    if (n % 2 || n / 2 > max) {
        return -1;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return (int)(n / 2);
}

static void hash_password(const char *password, char *out, size_t len) {
    uint8_t salt[16], dk[MM_SHA256_LEN];
    char    salt_hex[33], dk_hex[65];

    random_bytes(salt, sizeof(salt));
    mm_pbkdf2_sha256(password, strlen(password), salt, sizeof(salt), PBKDF2_ITERATIONS, dk, sizeof(dk));
    to_hex(salt_hex, salt, sizeof(salt));
    to_hex(dk_hex, dk, sizeof(dk));
    snprintf(out, len, "pbkdf2-sha256$%d$%s$%s", PBKDF2_ITERATIONS, salt_hex, dk_hex);
}

static int check_password(const char *password, const char *stored) {
    char     copy[192];
    char    *iter_s, *salt_s, *hash_s;
    uint8_t  salt[64], want[MM_SHA256_LEN], got[MM_SHA256_LEN];
    int      salt_len;
    long     iter;
    unsigned diff = 0;

    copy_str(copy, sizeof(copy), stored);
    if (strncmp(copy, "pbkdf2-sha256$", 14) != 0) {
        return 0;
    }
    iter_s = copy + 14;
    if ((salt_s = strchr(iter_s, '$')) == NULL) return 0;
    *salt_s++ = '\0';
    if ((hash_s = strchr(salt_s, '$')) == NULL) return 0;
    *hash_s++ = '\0';
    iter = strtol(iter_s, NULL, 10);
    salt_len = from_hex(salt, sizeof(salt), salt_s);
    if (iter < 1 || iter > 10000000 || salt_len < 0 || from_hex(want, sizeof(want), hash_s) != (int)sizeof(want)) {
        return 0;
    }
    mm_pbkdf2_sha256(password, strlen(password), salt, (size_t)salt_len, (uint32_t)iter, got, sizeof(got));
    for (size_t i = 0; i < sizeof(got); i++) {
        diff |= (unsigned)(got[i] ^ want[i]);
    }
    return diff == 0;
}

static int account_name_ok(const char *s) {
    size_t n = strlen(s);

    if (n == 0 || n > 32 || s[0] == '.' || s[0] == '-') {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-' || *p == '@')) {
            return 0;
        }
    }
    return 1;
}

static int password_ok(const char *s) {
    size_t n = strlen(s);

    if (n < MIN_PASSWORD || n > MAX_PASSWORD) {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if ((unsigned char)*p < 0x20) {
            return 0;
        }
    }
    return 1;
}

static account_t *find_account(const char *name) {
    for (int i = 0; i < g_naccounts; i++) {
        if (strcmp(g_accounts[i].name, name) == 0) {
            return &g_accounts[i];
        }
    }
    return NULL;
}

static int count_admins(void) {
    int n = 0;

    for (int i = 0; i < g_naccounts; i++) {
        n += g_accounts[i].admin;
    }
    return n;
}

static unsigned parse_perms(const char *s) {
    unsigned perms = 0;
    char     word[24];

    while (*s) {
        size_t n = strcspn(s, ", ");

        if (n > 0 && n < sizeof(word)) {
            memcpy(word, s, n);
            word[n] = '\0';
            if (strcmp(word, "all") == 0) {
                perms = P_ALL;
            }
            for (int i = 0; i < P_COUNT; i++) {
                if (strcmp(word, perm_names[i]) == 0) {
                    perms |= 1u << i;
                }
            }
        }
        s += n;
        s += strspn(s, ", ");
    }
    return perms;
}

/* Parse "<terminal ID> <perm,perm...>" into g.  Returns 0 if it isn't one. */
static int terminal_id_ok(const char *s);

static int parse_grant(const char *s, grant_t *g) {
    if (strlen(s) < 10) {
        return 0;
    }
    memcpy(g->id, s, 10);
    g->id[10] = '\0';
    if (!terminal_id_ok(g->id) || (s[10] != '\0' && s[10] != ' ')) {
        return 0;
    }
    g->perms = parse_perms(s + 10);
    return 1;
}

static void load_accounts(void) {
    size_t     len;
    char      *data = (char *)read_file(ACCOUNTS_FILE, &len);
    char      *line, *next;
    account_t *a = NULL;

    g_naccounts = 0;
    if (data == NULL) {
        return;
    }
    for (line = data; line && *line; line = next) {
        char *eq;

        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        line[strcspn(line, "\r")] = '\0';
        if (line[0] == '#' || (eq = strchr(line, '=')) == NULL) {
            continue;
        }
        *eq++ = '\0';
        if (strcmp(line, "phones_dir") == 0) {
            if (safe_path(eq, 0) && strlen(eq) < sizeof(g_phones_dir)) {
                copy_str(g_phones_dir, sizeof(g_phones_dir), eq);
            }
        } else if (strcmp(line, "user") == 0) {
            a = NULL;
            if (g_naccounts < MAX_ACCOUNTS && account_name_ok(eq) && find_account(eq) == NULL) {
                a = &g_accounts[g_naccounts++];
                memset(a, 0, sizeof(*a));
                copy_str(a->name, sizeof(a->name), eq);
            }
        } else if (a == NULL) {
            continue;
        } else if (strcmp(line, "role") == 0) {
            a->admin = strcmp(eq, "admin") == 0;
        } else if (strcmp(line, "password") == 0) {
            copy_str(a->pass, sizeof(a->pass), eq);
        } else if (strcmp(line, "phone") == 0 && a->ngrants < MAX_GRANTS) {
            if (parse_grant(eq, &a->grants[a->ngrants])) {
                a->ngrants++;
            }
        }
    }
    free(data);
}

static int save_accounts(void) {
    sbuf_t b = { 0 };
    int    rc;

    sb_puts(&b, "# Who may use the mmumblephone control panel (mm_gui). Change them in the control panel:\n"
                "# global -> accounts. Delete this file to turn sign-in off (then only this computer can use it).\n");
    sb_printf(&b, "phones_dir=%s\n", g_phones_dir);
    for (int i = 0; i < g_naccounts; i++) {
        const account_t *a = &g_accounts[i];

        sb_printf(&b, "\nuser=%s\nrole=%s\npassword=%s\n", a->name, a->admin ? "admin" : "user", a->pass);
        for (int k = 0; k < a->ngrants; k++) {
            int first = 1;

            sb_printf(&b, "phone=%s ", a->grants[k].id);
            for (int p = 0; p < P_COUNT; p++) {
                if (a->grants[k].perms & (1u << p)) {
                    sb_printf(&b, "%s%s", first ? "" : ",", perm_names[p]);
                    first = 0;
                }
            }
            sb_puts(&b, "\n");
        }
    }
    {
        char tmp[PATH_LEN];

        snprintf(tmp, sizeof(tmp), "%s.new", ACCOUNTS_FILE);
        rc = write_file(tmp, b.s, b.len);
        if (rc == 0) {
            (void)remove(ACCOUNTS_FILE);    /* rename() won't replace a file on Windows */
            rc = rename(tmp, ACCOUNTS_FILE);
        }
    }
    free(b.s);
#ifndef _WIN32
    (void)chmod(ACCOUNTS_FILE, 0600);
#endif /* _WIN32 */
    return rc;
}

static const char *new_session(const char *user) {
    session_t *s = &g_sessions[0];
    uint8_t    r[16];

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].token[0] == '\0') {
            s = &g_sessions[i];
            break;
        }
        if (g_sessions[i].last < s->last) {
            s = &g_sessions[i];     /* all in use: reuse the one idle longest */
        }
    }
    random_bytes(r, sizeof(r));
    to_hex(s->token, r, sizeof(r));
    copy_str(s->user, sizeof(s->user), user);
    s->last = time(NULL);
    return s->token;
}

static void end_sessions(const char *user, const char *except) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].token[0] && strcmp(g_sessions[i].user, user) == 0 &&
            (except == NULL || strcmp(g_sessions[i].token, except) != 0)) {
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
        }
    }
}

static account_t *session_account(const char *token) {
    time_t now = time(NULL);

    if (strlen(token) != 32) {
        return NULL;
    }
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &g_sessions[i];

        if (s->token[0] && strcmp(s->token, token) == 0) {
            account_t *a = find_account(s->user);

            if (a == NULL || now - s->last > SESSION_IDLE_S) {
                memset(s, 0, sizeof(*s));
                return NULL;
            }
            s->last = now;
            return a;
        }
    }
    return NULL;
}

static int is_admin(const request_t *req) {
    return req->who == NULL || req->who->admin;
}

static unsigned phone_perms(const account_t *a, const char *id) {
    for (int i = 0; i < a->ngrants; i++) {
        if (strcmp(a->grants[i].id, id) == 0) {
            return a->grants[i].perms;
        }
    }
    return 0;
}

/*
 * Split path into the parts below the phones folder: "<phones dir>/<first>/<rest>".
 * Returns 0 if path isn't the phones folder or inside it.  Only plain names are
 * taken (letters, digits, '_', '-', '.', not ending in '.'), so that Windows
 * can't open "4085551234 " or "4085551234." as the folder 4085551234.
 */
static int phones_rel(const char *path, char *first, size_t flen, const char **rest) {
    static char norm[PATH_LEN];
    size_t      n = strlen(g_phones_dir);
    const char *p;

    for (p = path; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.' || *p == '/' || *p == '\\') ||
            (*p == '.' && (p[1] == '\0' || p[1] == '/' || p[1] == '\\'))) {
            return 0;
        }
    }
    copy_str(norm, sizeof(norm), path);
    for (char *c = norm; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    first[0] = '\0';
    *rest = "";
    if (strncmp(norm, g_phones_dir, n) != 0 || (norm[n] != '\0' && norm[n] != '/')) {
        return 0;
    }
    p = norm + n + (norm[n] == '/');
    {
        size_t k = strcspn(p, "/");

        snprintf(first, flen, "%.*s", (int)(k < flen ? k : flen - 1), p);
        *rest = p[k] ? p + k + 1 : "";
    }
    return 1;
}

/*
 * A user may read their own phones' folders and the shared ones (default/,
 * the model folders) that the phones' settings come from.
 */
static int user_may_read(const account_t *a, const char *path) {
    char        first[64];
    const char *rest;

    if (!phones_rel(path, first, sizeof(first), &rest)) {
        return 0;
    }
    return !terminal_id_ok(first) || phone_perms(a, first) != 0;
}

static int keep_readable(const void *ctx, const char *path) {
    return user_may_read((const account_t *)ctx, path);
}

/* A user may change a table in a group they were given, and the overview's files, in their own phones' folders. */
static int user_may_write(const account_t *a, const char *path) {
    char        first[64];
    const char *rest;
    unsigned    perms, id;
    char        tail;

    if (!phones_rel(path, first, sizeof(first), &rest) || !terminal_id_ok(first) || (perms = phone_perms(a, first)) == 0) {
        return 0;
    }
    if (strcmp(rest, "phone.cfg") == 0 || strcmp(rest, "update_pending") == 0) {
        return (perms & (1u << P_OVERVIEW)) != 0;
    }
    if (strlen(rest) == 15 && sscanf(rest, "mm_table_%2x.bi%c", &id, &tail) == 2 && tail == 'n' &&
        isxdigit((unsigned char)rest[9]) && isxdigit((unsigned char)rest[10])) {
        return (perms & (1u << table_group(id))) != 0;
    }
    return 0;
}

static void sb_account(sbuf_t *b, const account_t *a) {
    sb_puts(b, "{\"name\":");
    sb_jstr(b, a->name);
    sb_printf(b, ",\"admin\":%s,\"phones\":[", a->admin ? "true" : "false");
    for (int i = 0; i < a->ngrants; i++) {
        int first = 1;

        sb_printf(b, "%s{\"id\":\"%s\",\"perms\":[", i ? "," : "", a->grants[i].id);
        for (int p = 0; p < P_COUNT; p++) {
            if (a->grants[i].perms & (1u << p)) {
                sb_printf(b, "%s\"%s\"", first ? "" : ",", perm_names[p]);
                first = 0;
            }
        }
        sb_puts(b, "]}");
    }
    sb_puts(b, "]}");
}

/* Split the body into lines in place.  Returns how many. */
static int body_lines(request_t *req, char **lines, int max) {
    int   n = 0;
    char *p = req->body;

    if (strlen(req->body) != req->body_len) {
        return -1;
    }
    while (p && *p && n < max) {
        char *nl = strchr(p, '\n');

        if (nl) *nl = '\0';
        p[strcspn(p, "\r")] = '\0';
        lines[n++] = p;
        p = nl ? nl + 1 : NULL;
    }
    return (p && *p) ? -1 : n;
}

/* GET /api/session: whether sign-in is on, and who is signed in. */
static void api_session(request_t *req) {
    sbuf_t b = { 0 };

    sb_printf(&b, "{\"ok\":true,\"accounts\":%s,\"local\":%s,\"phonesDir\":", g_naccounts ? "true" : "false",
              req->local ? "true" : "false");
    sb_jstr(&b, g_phones_dir);
    sb_puts(&b, ",\"user\":");
    if (req->who) {
        sb_account(&b, req->who);
    } else {
        sb_puts(&b, "null");
    }
    sb_puts(&b, "}");
    respond_json(req, 200, &b);
}

/* POST /api/login: body "<name>\n<password>". */
static void api_login(request_t *req) {
    static const char *dummy = "pbkdf2-sha256$100000$00000000000000000000000000000000$"
                               "0000000000000000000000000000000000000000000000000000000000000000";
    char      *lines[3];
    account_t *a;
    time_t     now = time(NULL);
    sbuf_t     b = { 0 };

    if (g_naccounts == 0) {
        respond_error(req, 409, "there are no accounts yet");
        return;
    }
    if (body_lines(req, lines, 2) != 2) {
        respond_error(req, 400, "send a name and a password");
        return;
    }
    a = find_account(lines[0]);
    if (a && a->locked_until > now) {
        respond_error(req, 429, "too many wrong passwords: wait a minute and try again");
        return;
    }
    if (!check_password(lines[1], a ? a->pass : dummy) || a == NULL) {
        if (a && ++a->fails >= 5) {
            long wait = 30L * (a->fails - 4);

            a->locked_until = now + (wait > 900 ? 900 : wait);
        }
        printf("mm_gui: failed sign-in%s%s\n", a ? " for " : "", a ? a->name : "");
        fflush(stdout);
        respond_error(req, 401, "wrong name or password");
        return;
    }
    a->fails = 0;
    a->locked_until = 0;
    sb_puts(&b, "{\"ok\":true,\"session\":");
    sb_jstr(&b, new_session(a->name));
    sb_puts(&b, ",\"user\":");
    sb_account(&b, a);
    sb_puts(&b, "}");
    respond_json(req, 200, &b);
}

static void api_logout(request_t *req) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].token[0] && strcmp(g_sessions[i].token, req->session) == 0) {
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
        }
    }
    respond_ok(req);
}

/* POST /api/password: body "<current password>\n<new password>".  Signs out the account's other sessions. */
static void api_password(request_t *req) {
    char *lines[3];

    if (req->who == NULL) {
        respond_error(req, 409, "there are no accounts yet");
        return;
    }
    if (body_lines(req, lines, 2) != 2 || !check_password(lines[0], req->who->pass)) {
        respond_error(req, 403, "the current password isn't right");
        return;
    }
    if (!password_ok(lines[1])) {
        respond_error(req, 400, "a password needs 8 to 128 characters");
        return;
    }
    hash_password(lines[1], req->who->pass, sizeof(req->who->pass));
    if (save_accounts() != 0) {
        respond_error(req, 500, strerror(errno));
        return;
    }
    end_sessions(req->who->name, req->session);
    respond_ok(req);
}

/* GET /api/accounts (admins). */
static void api_accounts(request_t *req) {
    sbuf_t b = { 0 };

    sb_puts(&b, "{\"ok\":true,\"phonesDir\":");
    sb_jstr(&b, g_phones_dir);
    sb_puts(&b, ",\"permissions\":[");
    for (int p = 0; p < P_COUNT; p++) {
        sb_printf(&b, "%s\"%s\"", p ? "," : "", perm_names[p]);
    }
    sb_puts(&b, "],\"accounts\":[");
    for (int i = 0; i < g_naccounts; i++) {
        if (i) sb_puts(&b, ",");
        sb_account(&b, &g_accounts[i]);
    }
    sb_puts(&b, "]}");
    respond_json(req, 200, &b);
}

/*
 * POST /api/accounts/save (admins): add or change an account.  The body has
 * one "key=value" per line: name, role (admin or user), password (needed for
 * a new account; otherwise only to change it), and phone=<terminal ID>
 * <perm,perm...> for each phone the account gets, which replace the ones it had.
 * The first account must be an admin, and that admin is signed in straight away.
 */
static void api_accounts_save(request_t *req) {
    char      *lines[MAX_GRANTS + 8];
    int        n = body_lines(req, lines, MAX_GRANTS + 8);
    const char *name = "", *role = "user", *password = NULL;
    grant_t    grants[MAX_GRANTS];
    int        ngrants = 0;
    account_t *a;
    int        first = g_naccounts == 0;
    sbuf_t     b = { 0 };

    if (n < 0) {
        respond_error(req, 400, "too many lines");
        return;
    }
    for (int i = 0; i < n; i++) {
        char *eq = strchr(lines[i], '=');

        if (eq == NULL) {
            continue;
        }
        *eq++ = '\0';
        if (strcmp(lines[i], "name") == 0) {
            name = eq;
        } else if (strcmp(lines[i], "role") == 0) {
            role = eq;
        } else if (strcmp(lines[i], "password") == 0 && eq[0]) {
            password = eq;
        } else if (strcmp(lines[i], "phone") == 0) {
            if (ngrants >= MAX_GRANTS || !parse_grant(eq, &grants[ngrants])) {
                respond_error(req, 400, "a phone is its 10-digit terminal ID and what the account may do there");
                return;
            }
            ngrants++;
        }
    }
    if (!account_name_ok(name)) {
        respond_error(req, 400, "a name is 1 to 32 letters, digits, '.', '_', '-' or '@'");
        return;
    }
    if (strcmp(role, "admin") != 0 && strcmp(role, "user") != 0) {
        respond_error(req, 400, "the role is admin or user");
        return;
    }
    if (password && !password_ok(password)) {
        respond_error(req, 400, "a password needs 8 to 128 characters");
        return;
    }
    a = find_account(name);
    if (a == NULL) {
        if (password == NULL) {
            respond_error(req, 400, "a new account needs a password");
            return;
        }
        if (first && strcmp(role, "admin") != 0) {
            respond_error(req, 400, "the first account must be an admin, so someone can manage the others");
            return;
        }
        if (g_naccounts >= MAX_ACCOUNTS) {
            respond_error(req, 409, "there are already as many accounts as mm_gui can hold");
            return;
        }
        a = &g_accounts[g_naccounts++];
        memset(a, 0, sizeof(*a));
        copy_str(a->name, sizeof(a->name), name);
    } else if (a->admin && strcmp(role, "admin") != 0 && count_admins() == 1) {
        respond_error(req, 409, "this is the only admin: make another admin first");
        return;
    }
    a->admin = strcmp(role, "admin") == 0;
    if (password) {
        hash_password(password, a->pass, sizeof(a->pass));
        a->fails = 0;
        a->locked_until = 0;
        end_sessions(a->name, req->session);
    }
    memcpy(a->grants, grants, sizeof(grants[0]) * (size_t)ngrants);
    a->ngrants = ngrants;
    if (save_accounts() != 0) {
        respond_error(req, 500, strerror(errno));
        load_accounts();
        return;
    }
    printf("mm_gui: account %s saved\n", a->name);
    fflush(stdout);
    sb_puts(&b, "{\"ok\":true");
    if (first) {
        sb_puts(&b, ",\"session\":");
        sb_jstr(&b, new_session(a->name));
    }
    sb_puts(&b, ",\"user\":");
    sb_account(&b, a);
    sb_puts(&b, "}");
    respond_json(req, 200, &b);
}

/*
 * The page's settings: mm_manager's options (shared by the admins, as there is
 * one mm_manager) and each account's own (where it was, theme, hints).  The page
 * also keeps them in the browser, but a browser keeps them per address, and
 * mm_gui's address changes when it moves to the next free port, or between
 * 127.0.0.1 and localhost.  Here they stay with the working folder.
 */
#define PREFS_DIR           ".mm_gui/prefs"
#define MAX_PREFS           (256 * 1024)

/* scope "manager" (admins): mm_manager's options; anything else: this account's own. */
static int prefs_path(request_t *req, char *path, size_t len) {
    char scope[16] = "";

    (void)query_param(req, "scope", scope, sizeof(scope));
    if (strcmp(scope, "manager") == 0) {
        if (!is_admin(req)) {
            return -1;
        }
        snprintf(path, len, "%s/manager.json", PREFS_DIR);
    } else if (req->who) {
        snprintf(path, len, "%s/user-%s.json", PREFS_DIR, req->who->name);
    } else {
        snprintf(path, len, "%s/user.json", PREFS_DIR);
    }
    return 0;
}

/* GET /api/prefs?scope=S: {"ok":true,"prefs":"<the saved JSON, or empty>"}.  PUT saves the body. */
static void api_prefs(request_t *req) {
    char path[PATH_LEN];

    if (prefs_path(req, path, sizeof(path) - 8) != 0) {
        respond_error(req, 403, "only an admin can do that");
        return;
    }
    if (strcmp(req->method, "GET") == 0) {
        size_t len  = 0;
        char  *data = (char *)read_file(path, &len);
        sbuf_t b    = { 0 };

        sb_puts(&b, "{\"ok\":true,\"prefs\":");
        sb_json(&b, data ? data : "", data ? len : 0);
        sb_puts(&b, "}");
        free(data);
        respond_json(req, 200, &b);
    } else if (strcmp(req->method, "PUT") == 0) {
        char tmp[PATH_LEN + 8];

        if (req->body_len > MAX_PREFS || req->body_len == 0 || req->body[0] != '{') {
            respond_error(req, 400, "send the settings as a JSON object");
            return;
        }
        snprintf(tmp, sizeof(tmp), "%s.new", path);
        if (write_file(tmp, req->body, req->body_len) != 0) {
            respond_error(req, 500, strerror(errno));
            return;
        }
        (void)remove(path);     /* rename() won't replace a file on Windows */
        if (rename(tmp, path) != 0) {
            respond_error(req, 500, strerror(errno));
            return;
        }
#ifndef _WIN32
        (void)chmod(path, 0600);
#endif /* _WIN32 */
        respond_ok(req);
    } else {
        respond_error(req, 400, "use GET or PUT");
    }
}

/* An account went: so do its settings. */
static void remove_prefs(const char *name) {
    char path[PATH_LEN];

    snprintf(path, sizeof(path), "%s/user-%s.json", PREFS_DIR, name);
    (void)remove(path);
}

/* POST /api/accounts/delete?name=N (admins).  The last admin stays. */
static void api_accounts_delete(request_t *req) {
    char       name[64];
    account_t *a;

    query_param(req, "name", name, sizeof(name));
    a = find_account(name);
    if (a == NULL) {
        respond_error(req, 404, "no such account");
        return;
    }
    if (a->admin && count_admins() == 1) {
        respond_error(req, 409, "this is the only admin: make another admin first");
        return;
    }
    end_sessions(a->name, NULL);
    remove_prefs(a->name);
    memmove(a, a + 1, sizeof(*a) * (size_t)(g_naccounts - 1 - (a - g_accounts)));
    g_naccounts--;
    if (save_accounts() != 0) {
        respond_error(req, 500, strerror(errno));
        load_accounts();
        return;
    }
    respond_ok(req);
}

/* POST /api/accounts/settings?phones_dir=D (admins): where the phones' folders are (mm_manager -t). */
static void api_accounts_settings(request_t *req) {
    char dir[256];

    if (query_param(req, "phones_dir", dir, sizeof(dir))) {
        for (char *c = dir; *c; c++) {
            if (*c == '\\') *c = '/';
        }
        if (!safe_path(dir, 0)) {
            respond_error(req, 400, "the phones folder must be inside the working folder");
            return;
        }
        copy_str(g_phones_dir, sizeof(g_phones_dir), dir);
    }
    if (g_naccounts && save_accounts() != 0) {
        respond_error(req, 500, strerror(errno));
        return;
    }
    respond_ok(req);
}

/* A phone's terminal ID changed: its accounts follow it. */
static void rename_grants(const char *from, const char *to) {
    int changed = 0;

    for (int i = 0; i < g_naccounts; i++) {
        for (int k = 0; k < g_accounts[i].ngrants; k++) {
            if (strcmp(g_accounts[i].grants[k].id, from) == 0) {
                copy_str(g_accounts[i].grants[k].id, sizeof(g_accounts[i].grants[k].id), to);
                changed = 1;
            }
        }
    }
    if (changed) {
        (void)save_accounts();
    }
}

/* ---------------------------------------------------------------------
 * API handlers
 * --------------------------------------------------------------------- */

static void tool_path(char *out, size_t len, const char *name) {
    char file[128];

    snprintf(file, sizeof(file), "%s" EXE_SUFFIX, name);
    join_path(out, len, g_bindir, file);
}

static void serve_page(request_t *req) {
    size_t len;
    char  *html = (char *)read_file(g_html, &len);
    char  *head;
    sbuf_t b = { 0 };

    if (html == NULL) {
        char msg[PATH_LEN + 128];

        snprintf(msg, sizeof(msg), "mm_gui couldn't read %s. Start mm_gui with -H <path to table_editor.html>.", g_html);
        respond(req, 404, "text/plain; charset=utf-8", msg, strlen(msg));
        return;
    }
    head = strstr(html, "</head>");
    if (head == NULL) {
        head = html;
    }
    sb_add(&b, html, (size_t)(head - html));
    sb_printf(&b, "<meta name=\"mm-gui-token\" content=\"%s\">\n", g_token);
    sb_add(&b, head, len - (size_t)(head - html));
    free(html);
    respond(req, 200, "text/html; charset=utf-8", b.s, b.len);
    free(b.s);
}

static void api_info(request_t *req) {
    sbuf_t b = { 0 };
    char   path[PATH_LEN];
    int    first = 1;
    static const char *scripts[] = {
        "generate_advert_prompts.py", "generate_intl_sbr.py", "generate_lcd.py", "generate_lcd_lata.py",
        "generate_lcd_lir.py", "generate_npa.py",
    };

    tool_path(path, sizeof(path), "mm_manager");
    sb_puts(&b, "{\"ok\":true,\"version\":");
    sb_jstr(&b, CMAKE_VERSION_STR " [" GIT_VERSION_STR "]");
    sb_puts(&b, ",\"platform\":\"" PLATFORM_NAME "\",\"root\":");
    sb_jstr(&b, is_admin(req) ? g_root : "");
    sb_puts(&b, ",\"bindir\":");
    sb_jstr(&b, is_admin(req) ? g_bindir : "");
    sb_printf(&b, ",\"manager\":%s,\"python\":", is_file(path) ? "true" : "false");
    sb_jstr(&b, g_python_version);
    sb_puts(&b, ",\"packages\":[");
    for (size_t i = 0; i < sizeof(python_packages) / sizeof(python_packages[0]); i++) {
        if (i) sb_puts(&b, ",");
        sb_jstr(&b, python_packages[i]);
    }
    sb_puts(&b, "],\"scripts\":[");
    for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); i++) {
        join_path(path, sizeof(path), g_scripts, scripts[i]);
        if (is_file(path)) {
            if (!first) sb_puts(&b, ",");
            sb_jstr(&b, scripts[i]);
            first = 0;
        }
    }
    sb_puts(&b, "]}");
    respond_json(req, 200, &b);
}

static void api_list(request_t *req) {
    char   dir[PATH_LEN], depth_s[8];
    sbuf_t b = { 0 };
    int    count = 0;
    int    depth;

    query_param(req, "dir", dir, sizeof(dir));
    query_param(req, "depth", depth_s, sizeof(depth_s));
    depth = atoi(depth_s);
    if (depth < 0 || depth > 4) depth = 0;
    if (!safe_path(dir, 1)) {
        respond_error(req, 400, "that folder is outside the working folder");
        return;
    }
    if (!is_admin(req) && !user_may_read(req->who, dir)) {
        /* Not one of this user's folders: as far as they can tell, it is empty. */
        sb_puts(&b, "{\"ok\":true,\"exists\":false,\"entries\":[]}");
        respond_json(req, 200, &b);
        return;
    }
    sb_puts(&b, "{\"ok\":true,\"exists\":");
    sb_puts(&b, (dir[0] == '\0' || is_dir(dir)) ? "true" : "false");
    sb_puts(&b, ",\"entries\":[");
    list_dir(&b, dir, depth, &count, is_admin(req) ? NULL : keep_readable, req->who);
    sb_puts(&b, "]}");
    respond_json(req, 200, &b);
}

static void api_file(request_t *req) {
    char path[PATH_LEN];

    query_param(req, "path", path, sizeof(path));
    if (!safe_path(path, 0)) {
        respond_error(req, 400, "that file is outside the working folder");
        return;
    }
    if (!is_admin(req) && (strcmp(req->method, "GET") == 0 ? !user_may_read(req->who, path) : !user_may_write(req->who, path))) {
        respond_error(req, 403, "your account can't change that: ask an admin");
        return;
    }
    if (strcmp(req->method, "GET") == 0) {
        size_t len;
        char  *data = (char *)read_file(path, &len);

        if (data == NULL || !is_file(path)) {
            free(data);
            respond_error(req, 404, "no such file");
            return;
        }
        respond(req, 200, "application/octet-stream", data, len);
        free(data);
    } else if (strcmp(req->method, "PUT") == 0) {
        if (write_file(path, req->body, req->body_len) != 0) {
            respond_error(req, 500, strerror(errno));
            return;
        }
        respond_ok(req);
    } else {
        respond_error(req, 400, "use GET or PUT");
    }
}

static void api_move(request_t *req) {
    char from[PATH_LEN], to[PATH_LEN];

    query_param(req, "from", from, sizeof(from));
    query_param(req, "to", to, sizeof(to));
    if (!safe_path(from, 0) || !safe_path(to, 0)) {
        respond_error(req, 400, "that file is outside the working folder");
        return;
    }
    if (!is_file(from)) {
        respond_error(req, 404, "no such file");
        return;
    }
    if (mkdir_parents(to) != 0) {
        respond_error(req, 500, strerror(errno));
        return;
    }
    if (is_file(to)) {
        (void)remove(to);   /* rename() won't replace a file on Windows */
    }
    if (rename(from, to) != 0) {
        respond_error(req, 500, strerror(errno));
        return;
    }
    respond_ok(req);
}

static void api_delete(request_t *req) {
    char path[PATH_LEN];

    query_param(req, "path", path, sizeof(path));
    if (!safe_path(path, 0) || !is_file(path)) {
        respond_error(req, 404, "no such file");
        return;
    }
    if (!is_admin(req) && !user_may_write(req->who, path)) {
        respond_error(req, 403, "your account can't change that: ask an admin");
        return;
    }
    if (remove(path) != 0) {
        respond_error(req, 500, strerror(errno));
        return;
    }
    respond_ok(req);
}

static int name_ok(const char *s, const char *prefix, const char *suffix) {
    size_t n = strlen(s), p = strlen(prefix), x = strlen(suffix);

    if (n <= p + x || n > 64 || strncmp(s, prefix, p) != 0 || strcmp(s + n - x, suffix) != 0) {
        return 0;
    }
    for (size_t i = 0; i < n - x; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            return 0;
        }
    }
    return 1;
}

/*
 * POST /api/run?job=manager|task.  The body has one argument per line.
 * For the manager job they are mm_manager's arguments.  For the task job,
 * the first line says what to run:
 *   tool <mm_name> <args...>      one of the mm_* programs next to mm_gui
 *   python <generate_x.py> <args> one of the table generator scripts
 *   pip                           install the generators' Python packages
 */
static void api_run(request_t *req) {
    char        name[16];
    char       *lines[MAX_ARGS + 1];
    char       *argv[MAX_ARGS + 8];
    char        prog[PATH_LEN];
    char        script[PATH_LEN];
    int         nlines = 0, argc = 0, first = 0;
    job_t      *job;
    const char *err;
    char       *p = req->body;

    query_param(req, "job", name, sizeof(name));
    job = find_job(name);
    if (job == NULL || job == &jobs[2]) {
        respond_error(req, 400, "unknown job");
        return;
    }
    if (strlen(req->body) != req->body_len) {
        respond_error(req, 400, "arguments can't contain NUL characters");
        return;
    }
    while (p && *p && nlines < MAX_ARGS) {
        char *nl = strchr(p, '\n');

        if (nl) *nl = '\0';
        if (nl && nl > p && nl[-1] == '\r') nl[-1] = '\0';
        lines[nlines++] = p;
        p = nl ? nl + 1 : NULL;
    }
    if (p && *p) {
        respond_error(req, 400, "too many arguments");
        return;
    }

    if (job == &jobs[0]) {
        tool_path(prog, sizeof(prog), "mm_manager");
        argv[argc++] = prog;
    } else if (nlines >= 2 && strcmp(lines[0], "tool") == 0 && name_ok(lines[1], "mm_", "")) {
        tool_path(prog, sizeof(prog), lines[1]);
        argv[argc++] = prog;
        first = 2;
    } else if ((nlines >= 2 && strcmp(lines[0], "python") == 0 && name_ok(lines[1], "generate_", ".py")) ||
               (nlines == 1 && strcmp(lines[0], "pip") == 0)) {
        if (g_python_argc == 0) {
            respond_error(req, 409, "Python 3 wasn't found on this computer. Install it from python.org, then restart mm_gui.");
            return;
        }
        for (int i = 0; i < g_python_argc; i++) {
            argv[argc++] = g_python[i];
        }
        if (strcmp(lines[0], "pip") == 0) {
            argv[argc++] = "-m";
            argv[argc++] = "pip";
            argv[argc++] = "install";
            argv[argc++] = "--user";
            for (size_t i = 0; i < sizeof(python_packages) / sizeof(python_packages[0]); i++) {
                argv[argc++] = (char *)python_packages[i];
            }
            first = nlines;
        } else {
            join_path(script, sizeof(script), g_scripts, lines[1]);
            if (!is_file(script)) {
                respond_error(req, 404, "that script isn't installed");
                return;
            }
            argv[argc++] = script;
            first = 2;
        }
    } else {
        respond_error(req, 400, "mm_gui only runs mm_manager, its tools and the table generators");
        return;
    }
    if (argv[0] == prog && !is_file(prog)) {
        respond_error(req, 404, "that program isn't next to mm_gui");
        return;
    }
    for (int i = first; i < nlines; i++) {
        argv[argc++] = lines[i];
    }
    argv[argc] = NULL;

    err = job_start(job, argv, req->sip_password[0] ? "MM_SIP_PASSWORD" : NULL, req->sip_password);
    if (err) {
        respond_error(req, 409, err);
        return;
    }
    respond_ok(req);
}

static void api_stop(request_t *req) {
    char   name[16];
    job_t *job;

    query_param(req, "job", name, sizeof(name));
    job = find_job(name);
    if (job == NULL) {
        respond_error(req, 400, "unknown job");
        return;
    }
    job_stop(job);
    respond_ok(req);
}

/* GET /api/output?job=...&from=N: output after offset N, and the job's state. */
static void api_output(request_t *req) {
    char     name[16], from_s[24];
    job_t   *job;
    sbuf_t   b = { 0 };
    uint64_t from;

    query_param(req, "job", name, sizeof(name));
    query_param(req, "from", from_s, sizeof(from_s));
    job = find_job(name);
    if (job == NULL) {
        respond_error(req, 400, "unknown job");
        return;
    }
    from = strtoull(from_s, NULL, 10);
    osal_mutex_lock(&job->lock);
    if (from < job->base || from > job->base + job->len) {
        from = job->base;
    }
    sb_printf(&b, "{\"ok\":true,\"running\":%s,\"started\":%s,\"stopping\":%s,\"exitCode\":%d,\"from\":%llu,\"to\":%llu,\"title\":",
              job->running ? "true" : "false", job->started ? "true" : "false", job->stopping ? "true" : "false",
              job->exit_code, (unsigned long long)from, (unsigned long long)(job->base + job->len));
    sb_jstr(&b, job->title);
    sb_puts(&b, ",\"text\":");
    sb_json(&b, job->buf + (from - job->base), (size_t)(job->base + job->len - from));
    osal_mutex_unlock(&job->lock);
    sb_puts(&b, "}");
    respond_json(req, 200, &b);
}

static int table_name_ok(const char *s) {
    if (s[0] == '\0' || strlen(s) > 64) {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return 0;
        }
    }
    return 1;
}

/*
 * GET /api/db: the tables in mm_manager.db and their row counts.
 * GET /api/db?table=T&limit=N&offset=M: rows of one table, newest first.
 * Either one with &terminal=<terminal ID> only counts and returns that terminal's rows,
 * and leaves out the tables that have no TERMINAL_ID column.
 */
static int terminal_id_ok(const char *s);

static void api_db(request_t *req) {
    sqlite3      *db = NULL;
    sqlite3_stmt *st = NULL;
    sbuf_t        b = { 0 };
    char          table[80], limit_s[16], offset_s[16], terminal[16];
    char          sql[320];
    const char   *where;
    int           limit, offset;

    if (query_param(req, "terminal", terminal, sizeof(terminal)) && terminal[0] != '\0') {
        if (!terminal_id_ok(terminal)) {
            respond_error(req, 400, "bad terminal ID");
            return;
        }
        where = " WHERE TERMINAL_ID = ?1";
    } else {
        terminal[0] = '\0';
        where       = "";
    }
    if (!is_admin(req) && (terminal[0] == '\0' || !(phone_perms(req->who, terminal) & (1u << P_RECORDS)))) {
        respond_error(req, 403, "your account can't see those records: ask an admin");
        return;
    }

    if (!is_file(DB_FILE)) {
        sb_puts(&b, "{\"ok\":true,\"exists\":false,\"tables\":[]}");
        respond_json(req, 200, &b);
        return;
    }
    if (sqlite3_open_v2(DB_FILE, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        respond_error(req, 500, sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 2000);

    if (!query_param(req, "table", table, sizeof(table))) {
        int first = 1;

        sb_puts(&b, "{\"ok\":true,\"exists\":true,\"tables\":[");
        if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name",
                               -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char   *name = (const char *)sqlite3_column_text(st, 0);
                sqlite3_stmt *cnt  = NULL;
                long long     rows = -1;

                if (name == NULL || !table_name_ok(name)) {
                    continue;
                }
                snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\"%s", name, where);
                if (sqlite3_prepare_v2(db, sql, -1, &cnt, NULL) != SQLITE_OK) {
                    sqlite3_finalize(cnt);
                    if (terminal[0]) {
                        continue;   /* no TERMINAL_ID column */
                    }
                    cnt = NULL;
                } else {
                    if (terminal[0]) {
                        sqlite3_bind_text(cnt, 1, terminal, -1, SQLITE_STATIC);
                    }
                    if (sqlite3_step(cnt) == SQLITE_ROW) {
                        rows = sqlite3_column_int64(cnt, 0);
                    }
                }
                sqlite3_finalize(cnt);
                sb_puts(&b, first ? "{\"name\":" : ",{\"name\":");
                sb_jstr(&b, name);
                sb_printf(&b, ",\"rows\":%lld}", rows);
                first = 0;
            }
        }
        sqlite3_finalize(st);
        sb_puts(&b, "]}");
        sqlite3_close(db);
        respond_json(req, 200, &b);
        return;
    }

    query_param(req, "limit", limit_s, sizeof(limit_s));
    query_param(req, "offset", offset_s, sizeof(offset_s));
    limit  = atoi(limit_s);
    offset = atoi(offset_s);
    if (limit <= 0 || limit > 1000) limit = 100;
    if (offset < 0) offset = 0;
    if (!table_name_ok(table)) {
        sqlite3_close(db);
        respond_error(req, 400, "bad table name");
        return;
    }
    snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\"%s ORDER BY rowid DESC LIMIT %d OFFSET %d", table, where, limit, offset);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\"%s LIMIT %d OFFSET %d", table, where, limit, offset);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
            respond_error(req, 404, sqlite3_errmsg(db));
            sqlite3_close(db);
            return;
        }
    }
    if (terminal[0]) {
        sqlite3_bind_text(st, 1, terminal, -1, SQLITE_STATIC);
    }
    sb_puts(&b, "{\"ok\":true,\"columns\":[");
    for (int i = 0; i < sqlite3_column_count(st); i++) {
        if (i) sb_puts(&b, ",");
        sb_jstr(&b, sqlite3_column_name(st, i));
    }
    sb_puts(&b, "],\"rows\":[");
    for (int r = 0; sqlite3_step(st) == SQLITE_ROW; r++) {
        sb_puts(&b, r ? ",[" : "[");
        for (int i = 0; i < sqlite3_column_count(st); i++) {
            if (i) sb_puts(&b, ",");
            switch (sqlite3_column_type(st, i)) {
                case SQLITE_INTEGER:
                    sb_printf(&b, "%lld", (long long)sqlite3_column_int64(st, i));
                    break;
                case SQLITE_FLOAT:
                    sb_printf(&b, "%.17g", sqlite3_column_double(st, i));
                    break;
                case SQLITE_TEXT:
                    sb_json(&b, (const char *)sqlite3_column_text(st, i), (size_t)sqlite3_column_bytes(st, i));
                    break;
                case SQLITE_BLOB:
                {
                    const unsigned char *blob = (const unsigned char *)sqlite3_column_blob(st, i);
                    int                  n    = sqlite3_column_bytes(st, i);
                    sbuf_t               hex  = { 0 };

                    for (int k = 0; k < n && k < 48; k++) {
                        sb_printf(&hex, "%s%02x", k ? " " : "", blob[k]);
                    }
                    if (n > 48) sb_printf(&hex, " … (%d bytes)", n);
                    sb_jstr(&b, hex.s ? hex.s : "");
                    free(hex.s);
                    break;
                }
                default:
                    sb_puts(&b, "null");
                    break;
            }
        }
        sb_puts(&b, "]");
    }
    sqlite3_finalize(st);
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\"%s", table, where);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && terminal[0]) {
        sqlite3_bind_text(st, 1, terminal, -1, SQLITE_STATIC);
    }
    if (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
        sb_printf(&b, "],\"total\":%lld}", (long long)sqlite3_column_int64(st, 0));
    } else {
        sb_puts(&b, "],\"total\":null}");
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    respond_json(req, 200, &b);
}

/* A terminal ID, as mm_manager names a terminal's folder: its 10-digit phone number. */
static int terminal_id_ok(const char *s) {
    if (strlen(s) != 10) {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    return 1;
}

/* Delete the files directly in a folder (not its subfolders). */
static void remove_files_in(const char *path) {
#ifdef _WIN32
    char             pattern[PATH_LEN];
    WIN32_FIND_DATAA fd;
    HANDLE           h;

    join_path(pattern, sizeof(pattern), path, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char child[PATH_LEN];

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            join_path(child, sizeof(child), path, fd.cFileName);
            (void)remove(child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR           *d = opendir(path);
    struct dirent *e;

    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char child[PATH_LEN];

        join_path(child, sizeof(child), path, e->d_name);
        if (is_file(child)) {
            (void)remove(child);
        }
    }
    closedir(d);
#endif /* _WIN32 */
}

/*
 * POST /api/rmdir?path=P: delete a terminal's folder (.../<terminal ID>) or a
 * saved configuration (configurations/<name>), and the files in it.  A folder
 * with folders inside is refused (after its files are gone).
 */
static void api_rmdir(request_t *req) {
    char        path[PATH_LEN];
    const char *last;

    query_param(req, "path", path, sizeof(path));
    if (!safe_path(path, 0) || !is_dir(path)) {
        respond_error(req, 404, "no such folder");
        return;
    }
    last = strrchr(path, '/');
    last = last ? last + 1 : path;
    if (!terminal_id_ok(last) &&
        !(strncmp(path, CONFIGS_DIR "/", sizeof(CONFIGS_DIR)) == 0 && strchr(path + sizeof(CONFIGS_DIR), '/') == NULL)) {
        respond_error(req, 400, "only a phone's folder or a saved configuration can be deleted");
        return;
    }
    remove_files_in(path);
    if (do_rmdir(path) != 0) {
        respond_error(req, 500, strerror(errno));
        return;
    }
    respond_ok(req);
}

/*
 * POST /api/phone/rename?dir=D&from=A&to=B: a terminal's ID (its phone number)
 * was changed on the terminal itself.  Rename its folder D/A to D/B, and move its
 * records in mm_manager.db to the new ID.
 */
static void api_phone_rename(request_t *req) {
    char          dir[PATH_LEN], from[16], to[16], src[PATH_LEN], dst[PATH_LEN];
    sqlite3      *db = NULL;
    sqlite3_stmt *st = NULL;
    sbuf_t        b = { 0 };
    long long     moved = 0;

    query_param(req, "dir", dir, sizeof(dir));
    query_param(req, "from", from, sizeof(from));
    query_param(req, "to", to, sizeof(to));
    if (!safe_path(dir, 0) || !terminal_id_ok(from) || !terminal_id_ok(to)) {
        respond_error(req, 400, "a terminal ID is 10 digits");
        return;
    }
    join_path(src, sizeof(src), dir, from);
    join_path(dst, sizeof(dst), dir, to);
    if (is_dir(dst) || is_file(dst)) {
        respond_error(req, 409, "there is already a phone with that ID");
        return;
    }
    if (is_dir(src) && rename(src, dst) != 0) {
        respond_error(req, 500, strerror(errno));
        return;
    }
    rename_grants(from, to);

    if (is_file(DB_FILE) && sqlite3_open_v2(DB_FILE, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
        char   names[64][80];
        int    n = 0;

        sqlite3_busy_timeout(db, 5000);
        if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",
                               -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW && n < 64) {
                const char *name = (const char *)sqlite3_column_text(st, 0);

                if (name != NULL && table_name_ok(name)) {
                    snprintf(names[n++], sizeof(names[0]), "%s", name);
                }
            }
        }
        sqlite3_finalize(st);
        for (int i = 0; i < n; i++) {
            char *sql = sqlite3_mprintf("PRAGMA table_info(\"%w\")", names[i]);
            int   has_id = 0;

            if (sql == NULL) {
                continue;
            }
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW) {
                    const char *col = (const char *)sqlite3_column_text(st, 1);

                    if (col != NULL && strcmp(col, "TERMINAL_ID") == 0) {
                        has_id = 1;
                    }
                }
            }
            sqlite3_finalize(st);
            sqlite3_free(sql);
            if (!has_id) {
                continue;
            }
            sql = sqlite3_mprintf("UPDATE OR IGNORE \"%w\" SET TERMINAL_ID = ?1 WHERE TERMINAL_ID = ?2", names[i]);
            if (sql == NULL) {
                continue;
            }
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, to, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 2, from, -1, SQLITE_STATIC);
                if (sqlite3_step(st) == SQLITE_DONE) {
                    moved += sqlite3_changes(db);
                }
            }
            sqlite3_finalize(st);
            sqlite3_free(sql);
        }
        sqlite3_close(db);
    }
    sb_printf(&b, "{\"ok\":true,\"records\":%lld}", moved);
    respond_json(req, 200, &b);
}

/* ---------------------------------------------------------------------
 * Updates
 *
 * The update center (manager -> updates) lists the releases published on
 * GitHub that are newer than this one, and installs the one picked.  Only
 * newer ones: mm_gui refuses to install an older or the same version.
 *
 * The install runs as its own job, "mm_gui --update <version> <folder>":
 * it downloads this computer's package and SHA256SUMS.txt with curl, checks
 * the package against it, unpacks it with tar, runs the new mm_gui once to
 * be sure it starts on this computer, and only then puts the new files over
 * the installed ones (undoing that if a file can't be replaced).  Tables and
 * config installed with the old version are left as they are: only files
 * they don't have yet are added.  The page then asks mm_gui to restart into
 * the new version, on the same port.
 *
 * Installed with the Windows installer (it has an Uninstall.exe, or Program
 * Files can't be written), the new installer is downloaded and checked
 * instead, and started when the page asks for the restart.
 *
 * MM_UPDATE_API and MM_UPDATE_DOWNLOAD point the update center somewhere
 * else (the tests use a local server).
 * --------------------------------------------------------------------- */

#define UPDATE_REPO         "jxburros/mm_manager"
#define UPDATE_API          "https://api.github.com/repos/" UPDATE_REPO "/releases?per_page=100"
#define UPDATE_DOWNLOAD     "https://github.com/" UPDATE_REPO "/releases/download"
#define UPDATE_DIR          ".mm_gui/update"
#define UPDATE_READY        UPDATE_DIR "/ready"
#define UPDATE_SUMS         "SHA256SUMS.txt"

#if defined(_WIN64)
# define UPDATE_PLATFORM    "win64"
#elif defined(_WIN32)
# define UPDATE_PLATFORM    "win32"
#elif defined(__APPLE__)
# define UPDATE_PLATFORM    "Darwin"
#elif defined(__linux__) && defined(__x86_64__)
# define UPDATE_PLATFORM    "Linux"
#endif
#ifdef UPDATE_PLATFORM
# define UPDATE_PLATFORM_NAME UPDATE_PLATFORM
#else
# define UPDATE_PLATFORM_NAME ""
#endif
#ifdef _WIN32
# define UPDATE_ARCHIVE     ".zip"
#else
# define UPDATE_ARCHIVE     ".tar.gz"
#endif /* _WIN32 */

static int g_restart;               /* restart into the new version once the loop ends */

static int dir_writable(const char *dir);

/* snprintf() for paths and URLs, which may be cut short (they are checked before they are used). */
static void path_printf(char *out, size_t len, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    if (vsnprintf(out, len, fmt, ap) < 0) {
        out[0] = '\0';
    }
    va_end(ap);
}

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);

    return (v && v[0]) ? v : fallback;
}

/*
 * "1.2.3", "1.2" or "1.2.3-rc.1", with or without a leading "v".  Sets n[] and
 * *pre (the part after "-", or NULL).  0 if it isn't a version.
 */
static int parse_version(const char *s, long n[3], const char **pre) {
    char *end;

    if (*s == 'v') {
        s++;
    }
    n[0] = n[1] = n[2] = 0;
    *pre = NULL;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
        n[i] = strtol(s, &end, 10);
        s    = end;
        if (*s != '.') {
            break;
        }
        if (i == 2) {
            return 0;
        }
        s++;
    }
    if (*s == '-') {
        if (*++s == '\0') {
            return 0;
        }
        *pre = s;
        for (; *s; s++) {
            if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') {
                return 0;
            }
        }
    }
    return *s == '\0';
}

/* Semantic version precedence of two pre-release parts ("rc.1" < "rc.2" < "rc.10"; none is highest). */
static int cmp_pre(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return (a == NULL) - (b == NULL);
    }
    for (;;) {
        size_t la = strcspn(a, "."), lb = strcspn(b, ".");
        int    na = la > 0 && strspn(a, "0123456789") >= la, nb = lb > 0 && strspn(b, "0123456789") >= lb;
        int    c;

        if (na && nb) {
            while (la > 1 && *a == '0') { a++; la--; }
            while (lb > 1 && *b == '0') { b++; lb--; }
            c = (la != lb) ? (la < lb ? -1 : 1) : strncmp(a, b, la);
        } else if (na != nb) {
            c = na ? -1 : 1;    /* numbers come before words */
        } else {
            c = strncmp(a, b, la < lb ? la : lb);
            if (c == 0 && la != lb) {
                c = la < lb ? -1 : 1;
            }
        }
        if (c != 0) {
            return c < 0 ? -1 : 1;
        }
        a += la;
        b += lb;
        if (*a == '\0' || *b == '\0') {
            return (*a != '\0') - (*b != '\0');
        }
        a++;
        b++;
    }
}

/* -1, 0 or 1 as a is older than, the same as or newer than b; -2 if either isn't a version. */
static int version_cmp(const char *a, const char *b) {
    long        na[3], nb[3];
    const char *pa, *pb;

    if (!parse_version(a, na, &pa) || !parse_version(b, nb, &pb)) {
        return -2;
    }
    for (int i = 0; i < 3; i++) {
        if (na[i] != nb[i]) {
            return na[i] < nb[i] ? -1 : 1;
        }
    }
    return cmp_pre(pa, pb);
}

/* This computer's package of a version, e.g. mmumblephone-0.6.0-Linux.tar.gz. */
static void update_asset(char *out, size_t len, const char *version, int installer) {
#ifdef UPDATE_PLATFORM
    snprintf(out, len, "mmumblephone-%s-%s%s", version, UPDATE_PLATFORM, installer ? ".exe" : UPDATE_ARCHIVE);
#else
    (void)version;
    (void)installer;
    copy_str(out, len, "");
#endif /* UPDATE_PLATFORM */
}

/*
 * The folder the release package was unpacked or installed into: the one with
 * bin/ (where this mm_gui is) and share/mm_manager/.  NULL if this copy can be
 * updated (and *installer says whether that's through the Windows installer),
 * otherwise why not.
 */
static const char *update_prefix(char *out, size_t len, int *installer) {
    static char why[PATH_LEN + 128];
    char        test[PATH_LEN];
    const char *base = g_bindir;

    *installer = 0;
#ifndef UPDATE_PLATFORM
    (void)out; (void)len; (void)test; (void)base;
    return "there's no release package for this kind of computer, so update it from the source";
#else
    for (const char *c = g_bindir; *c; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    copy_str(out, len, g_bindir);
    dir_name(out);
    join_path(test, sizeof(test), out, "share/mm_manager");
    if (strcmp(base, "bin") != 0 || !is_dir(test)) {
        return "this copy of mm_gui wasn't installed from a release package (it is a build, not in an install's bin folder), "
               "so update it from the source";
    }
# ifdef _WIN32
    join_path(test, sizeof(test), out, "Uninstall.exe");
    if (is_file(test) || !dir_writable(g_bindir)) {
        *installer = 1;
        return NULL;
    }
# endif /* _WIN32 */
    if (!dir_writable(g_bindir)) {
        snprintf(why, sizeof(why), "mm_gui can't write to %s: run it as a user who can, or install the new version yourself", out);
        return why;
    }
    return NULL;
#endif /* UPDATE_PLATFORM */
}

/* What a finished update is waiting for: "restart", "installer <path>", or "" for nothing. */
static void update_ready(char *out, size_t len) {
    size_t n;
    char  *data = (char *)read_file(UPDATE_READY, &n);

    copy_str(out, len, data ? data : "");
    out[strcspn(out, "\r\n")] = '\0';
    free(data);
}

/* Run a program to the end, its output going to ours.  Its exit code, or -1. */
static int run_wait(char **argv) {
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    {
        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;
        sbuf_t              cmd = { 0 };
        DWORD               code = (DWORD)-1;

        for (int i = 0; argv[i]; i++) {
            if (i) sb_add(&cmd, " ", 1);
            win_quote(&cmd, argv[i]);
        }
        memset(&si, 0, sizeof(si));
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        if (!CreateProcessA(NULL, cmd.s, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            printf("couldn't start %s (Windows error %lu)\n", argv[0], GetLastError());
            free(cmd.s);
            return -1;
        }
        free(cmd.s);
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (int)code;
    }
#else
    {
        pid_t pid;
        int   status, rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);

        if (rc != 0) {
            printf("couldn't start %s: %s\n", argv[0], strerror(rc));
            return -1;
        }
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                return -1;
            }
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif /* _WIN32 */
}

/* curl and tar.  On Windows, the ones that come with it (not, say, Git's tar, which can't unzip). */
static void system_tool(char *out, size_t len, const char *name) {
#ifdef _WIN32
    const char *root = getenv("SystemRoot");

    if (root) {
        snprintf(out, len, "%s\\System32\\%s.exe", root, name);
        if (is_file(out)) {
            return;
        }
    }
#endif /* _WIN32 */
    copy_str(out, len, name);
}

static int download(const char *url, const char *to) {
    char  curl[PATH_LEN];
    char *argv[] = { curl, "-fsSL", "--retry", "3", "--connect-timeout", "30", "-o", (char *)to, (char *)url, NULL };

    system_tool(curl, sizeof(curl), "curl");
    printf("downloading %s\n", url);
    if (run_wait(argv) != 0) {
        printf("couldn't download it: is this computer on the internet, and is curl installed?\n");
        return -1;
    }
    return 0;
}

static int sha256_file(const char *path, char hex[2 * MM_SHA256_LEN + 1]) {
    FILE         *f = fopen(path, "rb");
    unsigned char buf[65536];
    uint8_t       sum[MM_SHA256_LEN];
    mm_sha256_t   c;
    size_t        n;

    if (f == NULL) {
        return -1;
    }
    mm_sha256_init(&c);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mm_sha256_update(&c, buf, n);
    }
    fclose(f);
    mm_sha256_final(&c, sum);
    to_hex(hex, sum, sizeof(sum));
    return 0;
}

/* The checksum SHA256SUMS.txt gives for name ("<hex>  <name>" or "<hex> *<name>" lines). */
static int expected_sum(const char *sums, const char *name, char hex[2 * MM_SHA256_LEN + 1]) {
    for (const char *line = sums; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        size_t      n = strcspn(line, " \t");
        const char *file = line + n;
        size_t      flen;

        while (*file == ' ' || *file == '\t' || *file == '*') {
            file++;
        }
        flen = strcspn(file, "\r\n");
        if (n == 2 * MM_SHA256_LEN && flen == strlen(name) && strncmp(file, name, flen) == 0) {
            for (size_t i = 0; i < n; i++) {
                hex[i] = (char)tolower((unsigned char)line[i]);
            }
            hex[n] = '\0';
            return 0;
        }
    }
    return -1;
}

/* Download a release file and check it against the release's SHA256SUMS.txt. */
static int fetch_checked(const char *base, const char *sums, const char *name, const char *to) {
    char url[PATH_LEN], want[2 * MM_SHA256_LEN + 1], got[2 * MM_SHA256_LEN + 1];

    if (expected_sum(sums, name, want) != 0) {
        printf("%s has no checksum for %s, so it won't be installed.\n", UPDATE_SUMS, name);
        return -1;
    }
    path_printf(url, sizeof(url), "%s/%s", base, name);
    if (download(url, to) != 0) {
        return -1;
    }
    if (sha256_file(to, got) != 0 || strcmp(got, want) != 0) {
        printf("%s doesn't match its checksum (got %s, expected %s): it was damaged on the way, so it won't be installed.\n",
               name, got, want);
        (void)remove(to);
        return -1;
    }
    printf("checked %s: SHA-256 %s matches\n", name, got);
    return 0;
}

/* Call fn(ctx, name) for each entry of dir but "." and "..".  Stops at, and returns, the first non-zero. */
typedef int (*entry_fn)(void *ctx, const char *name);

static int for_each_entry(const char *dir, entry_fn fn, void *ctx) {
    int rc = 0;
#ifdef _WIN32
    char             pattern[PATH_LEN];
    WIN32_FIND_DATAA fd;
    HANDLE           h;

    join_path(pattern, sizeof(pattern), dir, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
            rc = fn(ctx, fd.cFileName);
        }
    } while (rc == 0 && FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR           *d = opendir(dir);
    struct dirent *e;

    if (d == NULL) {
        return 0;
    }
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            rc = fn(ctx, e->d_name);
        }
    }
    closedir(d);
#endif /* _WIN32 */
    return rc;
}

/* Delete a folder and everything in it. */
static int rm_tree_entry(void *ctx, const char *name) {
    char path[PATH_LEN];

    join_path(path, sizeof(path), (const char *)ctx, name);
    if (is_dir(path)) {
        (void)for_each_entry(path, rm_tree_entry, path);
        (void)do_rmdir(path);
    } else {
        (void)remove(path);
    }
    return 0;
}

static void rm_tree(const char *dir) {
    char copy[PATH_LEN];

    copy_str(copy, sizeof(copy), dir);
    (void)for_each_entry(copy, rm_tree_entry, copy);
    (void)do_rmdir(copy);
}

/* The new files are put in place one by one; each replaced one is kept as <name>.old until all are. */
typedef struct {
    const char *from;               /* the unpacked package */
    const char *to;                 /* the install folder */
    char        rel[PATH_LEN];      /* where in them we are, "" at the top */
    sbuf_t      done;               /* "<0 or 1 had an old one><target>\n" for each file put in place */
    int         files, kept;
} install_t;

/* Tables and config: the user's own by now, so only files they don't have are added. */
static int keep_existing(const char *rel) {
    return strncmp(rel, "share/mm_manager/tables/", 24) == 0 || strncmp(rel, "share/mm_manager/config/", 24) == 0;
}

static int install_entry(void *ctx, const char *name) {
    install_t *in = (install_t *)ctx;
    char       rel[PATH_LEN], src[PATH_LEN], dst[PATH_LEN], tmp[PATH_LEN + 8], old[PATH_LEN + 8];
    size_t     len;
    unsigned char *data;
    int        had;
    stat_t     st;

    join_path(rel, sizeof(rel), in->rel, name);
    join_path(src, sizeof(src), in->from, rel);
    join_path(dst, sizeof(dst), in->to, rel);
    if (is_dir(src)) {
        char save[PATH_LEN];
        int  rc;

        copy_str(save, sizeof(save), in->rel);
        copy_str(in->rel, sizeof(in->rel), rel);
        rc = for_each_entry(src, install_entry, in);
        copy_str(in->rel, sizeof(in->rel), save);
        return rc;
    }
    if (keep_existing(rel) && is_file(dst)) {
        in->kept++;
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s.new", dst);
    snprintf(old, sizeof(old), "%s.old", dst);
    data = read_file(src, &len);
    if (data == NULL || mkdir_parents(dst) != 0 || write_file(tmp, (const char *)data, len) != 0) {
        printf("couldn't write %s: %s\n", tmp, strerror(errno));
        free(data);
        (void)remove(tmp);
        return -1;
    }
    free(data);
#ifndef _WIN32
    if (do_stat(src, &st) == 0) {
        (void)chmod(tmp, st.st_mode & 0777);
    }
#else
    (void)st;
#endif /* _WIN32 */
    (void)remove(old);
    had = is_file(dst);
    /* A running program can be renamed, not overwritten (on Windows, and its text on Linux). */
    if (had && rename(dst, old) != 0) {
        printf("couldn't replace %s: %s\n", dst, strerror(errno));
        (void)remove(tmp);
        return -1;
    }
    if (rename(tmp, dst) != 0) {
        printf("couldn't replace %s: %s\n", dst, strerror(errno));
        if (had) {
            (void)rename(old, dst);
        }
        (void)remove(tmp);
        return -1;
    }
    sb_printf(&in->done, "%d%s\n", had, dst);
    in->files++;
    return 0;
}

/* Undo the files put in place, or (rollback 0) just tidy away the old ones. */
static void install_finish(install_t *in, int rollback) {
    for (char *line = in->done.s; line && *line;) {
        char *nl = strchr(line, '\n');
        char  old[PATH_LEN + 8];

        *nl = '\0';
        snprintf(old, sizeof(old), "%s.old", line + 1);
        if (rollback) {
            (void)remove(line + 1);
            if (line[0] == '1') {
                (void)rename(old, line + 1);
            }
        } else {
            (void)remove(old);  /* a running program's stays until the next start (update_cleanup) */
        }
        line = nl + 1;
    }
    free(in->done.s);
    in->done.s = NULL;
}

/* Left over from an update: the old copies of programs that were running then. */
static int cleanup_entry(void *ctx, const char *name) {
    char   path[PATH_LEN];
    size_t n = strlen(name);

    if (n > 4 && strcmp(name + n - 4, ".old") == 0) {
        join_path(path, sizeof(path), (const char *)ctx, name);
        (void)remove(path);
    }
    return 0;
}

static void update_cleanup(void) {
    (void)for_each_entry(g_bindir, cleanup_entry, g_bindir);
}

typedef struct {
    const char *in;
    char        found[256];
} find_ctx_t;

static int find_dir_entry(void *ctx, const char *name) {
    find_ctx_t *fc = (find_ctx_t *)ctx;
    char        path[PATH_LEN];

    join_path(path, sizeof(path), fc->in, name);
    if (is_dir(path)) {
        copy_str(fc->found, sizeof(fc->found), name);
        return 1;
    }
    return 0;
}

static int write_ready(const char *what) {
    return write_file(UPDATE_READY, what, strlen(what));
}

/* mm_gui --update <version> <install folder>: run as the "update" job, from the working folder. */
static int update_main(const char *version, const char *prefix) {
    char        base[PATH_LEN], dir[PATH_LEN], sums_path[PATH_LEN], asset[256], file[PATH_LEN];
    char        unpacked[PATH_LEN], top[PATH_LEN], newgui[PATH_LEN], check[PATH_LEN];
    char       *sums;
    size_t      len;
    int         installer = 0;
    const char *why;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Updating mmumblephone %s to %s\n", CMAKE_VERSION_STR, version);
    if (version_cmp(version, CMAKE_VERSION_STR) != 1) {
        printf("%s isn't newer than %s: mmumblephone only updates to newer versions.\n", version, CMAKE_VERSION_STR);
        return 1;
    }
    why = update_prefix(check, sizeof(check), &installer);
    if (why != NULL || strcmp(check, prefix) != 0) {
        printf("This copy can't be updated here: %s\n", why ? why : "the install folder changed");
        return 1;
    }
    (void)remove(UPDATE_READY);
    snprintf(dir, sizeof(dir), "%s/%s", UPDATE_DIR, version);
    rm_tree(dir);
    join_path(sums_path, sizeof(sums_path), dir, UPDATE_SUMS);
    if (mkdir_parents(sums_path) != 0) {
        printf("couldn't make %s: %s\n", dir, strerror(errno));
        return 1;
    }
    snprintf(base, sizeof(base), "%s/v%s", env_or("MM_UPDATE_DOWNLOAD", UPDATE_DOWNLOAD), version);

    printf("\nstep 1 of 4: the release's checksums\n");
    path_printf(file, sizeof(file), "%s/%s", base, UPDATE_SUMS);
    if (download(file, sums_path) != 0 || (sums = (char *)read_file(sums_path, &len)) == NULL) {
        return 1;
    }

    update_asset(asset, sizeof(asset), version, installer);
    printf("\nstep 2 of 4: the package for this computer, %s\n", asset);
    join_path(file, sizeof(file), dir, asset);
    if (fetch_checked(base, sums, asset, file) != 0) {
        free(sums);
        return 1;
    }
    free(sums);

    if (installer) {
        char ready[PATH_LEN + 16];

        absolute(file, sizeof(file));
        snprintf(ready, sizeof(ready), "installer %s\n", file);
        if (write_ready(ready) != 0) {
            printf("couldn't write %s: %s\n", UPDATE_READY, strerror(errno));
            return 1;
        }
        printf("\nThe %s installer is ready. Restarting mm_gui starts it: mm_gui closes, and Windows asks to let the installer\n"
               "make changes. Start the control panel again once it has finished.\n", version);
        return 0;
    }

    printf("\nstep 3 of 4: unpack it and try the new mm_gui\n");
    join_path(unpacked, sizeof(unpacked), dir, "unpacked");
    (void)do_mkdir(unpacked);
    {
        char  tar[PATH_LEN];
        char *argv[] = { tar, "-xf", file, "-C", unpacked, NULL };

        system_tool(tar, sizeof(tar), "tar");
        if (run_wait(argv) != 0) {
            printf("couldn't unpack %s with tar.\n", asset);
            return 1;
        }
    }
    path_printf(top, sizeof(top), "%s/mmumblephone-%s-%s", unpacked, version, UPDATE_PLATFORM_NAME);
    if (!is_dir(top)) {
        /* The package's one top folder, whatever it is called. */
        find_ctx_t fc = { unpacked, "" };

        (void)for_each_entry(unpacked, find_dir_entry, &fc);
        join_path(top, sizeof(top), unpacked, fc.found);
    }
    path_printf(newgui, sizeof(newgui), "%s/bin/mm_gui" EXE_SUFFIX, top);
    if (!is_file(newgui)) {
        printf("%s doesn't have bin/mm_gui" EXE_SUFFIX " where it should be, so it won't be installed.\n", asset);
        return 1;
    }
    {
        char *argv[] = { newgui, "-V", NULL };

        if (run_wait(argv) != 0) {
            printf("The new mm_gui doesn't run on this computer, so nothing was changed.\n");
            return 1;
        }
    }

    printf("\nstep 4 of 4: put the new files in %s\n", prefix);
#ifndef _WIN32
    signal(SIGINT, SIG_IGN);    /* from here it finishes, or puts everything back */
#endif /* _WIN32 */
    {
        install_t in;

        memset(&in, 0, sizeof(in));
        in.from = top;
        in.to   = prefix;
        if (for_each_entry(top, install_entry, &in) != 0) {
            install_finish(&in, 1);
            printf("The update stopped part way, so the files it had changed were put back: %s is still installed.\n",
                   CMAKE_VERSION_STR);
            return 1;
        }
        install_finish(&in, 0);
        printf("%d files installed", in.files);
        if (in.kept) {
            printf(", %d of your own tables and config files kept as they were", in.kept);
        }
        printf(".\n");
    }
    rm_tree(dir);
    if (write_ready("restart\n") != 0) {
        printf("couldn't write %s: %s\n", UPDATE_READY, strerror(errno));
        return 1;
    }
    printf("\nmmumblephone %s is installed. Restart mm_gui to start using it.\n", version);
    return 0;
}

/* GET /api/update (admins): this version, whether and how it can be updated, and where to look for releases. */
static void api_update_info(request_t *req) {
    sbuf_t      b = { 0 };
    char        prefix[PATH_LEN], ready[PATH_LEN], asset[256];
    int         installer;
    const char *why = update_prefix(prefix, sizeof(prefix), &installer);

    update_ready(ready, sizeof(ready));
    update_asset(asset, sizeof(asset), "{version}", installer);
    sb_puts(&b, "{\"ok\":true,\"current\":");
    sb_jstr(&b, CMAKE_VERSION_STR);
    sb_printf(&b, ",\"updatable\":%s,\"installer\":%s,\"why\":", why ? "false" : "true", installer ? "true" : "false");
    sb_jstr(&b, why ? why : "");
    sb_puts(&b, ",\"folder\":");
    sb_jstr(&b, why ? "" : prefix);
    sb_puts(&b, ",\"asset\":");
    sb_jstr(&b, asset);
    sb_puts(&b, ",\"sums\":\"" UPDATE_SUMS "\",\"releases\":");
    sb_jstr(&b, env_or("MM_UPDATE_API", UPDATE_API));
    sb_puts(&b, ",\"page\":\"https://github.com/" UPDATE_REPO "/releases\",\"ready\":");
    sb_jstr(&b, strncmp(ready, "installer ", 10) == 0 ? "installer" : ready);
    sb_puts(&b, "}");
    respond_json(req, 200, &b);
}

static int manager_running(void) {
    int running;

    osal_mutex_lock(&jobs[0].lock);
    running = jobs[0].running;
    osal_mutex_unlock(&jobs[0].lock);
    return running;
}

/* POST /api/update/start?version=X (admins): start installing version X, which must be newer. */
static void api_update_start(request_t *req) {
    char        version[64], prefix[PATH_LEN], self[PATH_LEN];
    char       *argv[5];
    int         installer, cmp;
    const char *why, *err;

    query_param(req, "version", version, sizeof(version));
    cmp = version_cmp(version, CMAKE_VERSION_STR);
    if (cmp == -2) {
        respond_error(req, 400, "that isn't a version number");
        return;
    }
    if (cmp != 1) {
        respond_error(req, 409, "mmumblephone only updates to newer versions: that one isn't newer than this one");
        return;
    }
    if ((why = update_prefix(prefix, sizeof(prefix), &installer)) != NULL) {
        respond_error(req, 409, why);
        return;
    }
    if (manager_running()) {
        respond_error(req, 409, "stop mm_manager first (manager -> run): it is updated too");
        return;
    }
    tool_path(self, sizeof(self), "mm_gui");
    argv[0] = self;
    argv[1] = "--update";
    argv[2] = version;
    argv[3] = prefix;
    argv[4] = NULL;
    if ((err = job_start(&jobs[2], argv, NULL, NULL)) != NULL) {
        respond_error(req, 409, strcmp(err, "it is already running") == 0 ? "an update is already being installed" : err);
        return;
    }
    respond_ok(req);
}

/* POST /api/update/restart (admins): finish an installed update by restarting (or running the installer). */
static void api_update_restart(request_t *req) {
    char ready[PATH_LEN];
    int  running;

    osal_mutex_lock(&jobs[2].lock);
    running = jobs[2].running;
    osal_mutex_unlock(&jobs[2].lock);
    update_ready(ready, sizeof(ready));
    if (running || ready[0] == '\0') {
        respond_error(req, 409, running ? "the update is still being installed" : "there's no installed update waiting for a restart");
        return;
    }
    if (manager_running()) {
        respond_error(req, 409, "stop mm_manager first (manager -> run)");
        return;
    }
    (void)remove(UPDATE_READY);
#ifdef _WIN32
    if (strncmp(ready, "installer ", 10) == 0) {
        if ((INT_PTR)ShellExecuteA(NULL, "open", ready + 10, NULL, NULL, SW_SHOWNORMAL) <= 32) {
            respond_error(req, 500, "couldn't start the installer");
            return;
        }
        respond_ok(req);
        printf("mm_gui: the installer is running: start the control panel again when it has finished.\n");
        g_running = 0;
        return;
    }
#endif /* _WIN32 */
    if (strcmp(ready, "restart") != 0) {
        respond_error(req, 409, "there's no installed update waiting for a restart");
        return;
    }
    respond_ok(req);
    g_restart = 1;
    g_running = 0;
}

/*
 * Start the new mm_gui in our place, with the same options, on the same port
 * (the page reloads from it), without opening another browser window.
 */
static int restart_self(int argc, char *argv[], int html_given) {
    char  self[PATH_LEN], port[16];
    char *args[MAX_ARGS + 8];
    int   n = 0;

    tool_path(self, sizeof(self), "mm_gui");
    snprintf(port, sizeof(port), "%d", g_port);
    args[n++] = self;
    for (int i = 1; i < argc && n < MAX_ARGS; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "-H") == 0) {
            i++;    /* given again below, as absolute paths */
        } else if (strcmp(argv[i], "-n") != 0) {
            args[n++] = argv[i];
        }
    }
    args[n++] = "-p";
    args[n++] = port;
    args[n++] = "-w";
    args[n++] = g_root;
    if (html_given) {
        args[n++] = "-H";
        args[n++] = g_html;
    }
    args[n++] = "-n";
    args[n]   = NULL;
    printf("mm_gui: restarting into the new version.\n");
    fflush(stdout);
#ifdef _WIN32
    {
        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;
        sbuf_t              cmd = { 0 };

        for (int i = 0; args[i]; i++) {
            if (i) sb_add(&cmd, " ", 1);
            win_quote(&cmd, args[i]);
        }
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcessA(NULL, cmd.s, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "mm_gui: couldn't start %s (Windows error %lu): start it again yourself.\n", self, GetLastError());
            free(cmd.s);
            return 1;
        }
        free(cmd.s);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 0;
    }
#else
    execv(self, args);
    fprintf(stderr, "mm_gui: couldn't start %s: %s.  Start it again yourself.\n", self, strerror(errno));
    return 1;
#endif /* _WIN32 */
}

static void handle(request_t *req) {
    int is_get  = strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0;
    int is_post = strcmp(req->method, "POST") == 0;

    /*
     * With accounts, other computers may use the panel (with -L), and nothing
     * works without signing in, so any host name will do.  Without, it is only
     * for this computer.
     */
    if (!(g_naccounts && g_listen_any) && !host_ok(req->host)) {
        respond_error(req, 403, "mm_gui only answers requests for 127.0.0.1 or localhost");
        return;
    }
    if (!req->local && g_naccounts == 0) {
        respond_error(req, 403, "the control panel has no accounts yet, so it only answers the computer it runs on: "
                      "add an admin account there (global -> accounts) to use it from other computers");
        return;
    }
    if (strcmp(req->path, "/") == 0 || strcmp(req->path, "/index.html") == 0 || strcmp(req->path, "/table_editor.html") == 0) {
        if (!is_get) {
            respond_error(req, 400, "use GET");
            return;
        }
        serve_page(req);
        return;
    }
    if (strncmp(req->path, "/api/", 5) != 0) {
        respond_error(req, 404, "not found");
        return;
    }
    if (strcmp(req->token, g_token) != 0) {
        respond_error(req, 403, "missing or stale token: reload the page");
        return;
    }
    if (g_naccounts) {
        req->who = session_account(req->session);
    }
    if (strcmp(req->path, "/api/session") == 0 && is_get) {
        api_session(req);
        return;
    }
    if (strcmp(req->path, "/api/login") == 0 && is_post) {
        api_login(req);
        return;
    }
    if (g_naccounts && req->who == NULL) {
        respond_error(req, 401, "sign in first");
        return;
    }
    if (!is_admin(req) && !(strcmp(req->path, "/api/info") == 0 || strcmp(req->path, "/api/list") == 0 ||
                            strcmp(req->path, "/api/file") == 0 || strcmp(req->path, "/api/delete") == 0 ||
                            strcmp(req->path, "/api/db") == 0 || strcmp(req->path, "/api/logout") == 0 ||
                            strcmp(req->path, "/api/password") == 0 || strcmp(req->path, "/api/prefs") == 0)) {
        respond_error(req, 403, "only an admin can do that");
        return;
    }
    if (strcmp(req->path, "/api/logout") == 0 && is_post) {
        api_logout(req);
    } else if (strcmp(req->path, "/api/password") == 0 && is_post) {
        api_password(req);
    } else if (strcmp(req->path, "/api/accounts") == 0 && is_get) {
        api_accounts(req);
    } else if (strcmp(req->path, "/api/accounts/save") == 0 && is_post) {
        api_accounts_save(req);
    } else if (strcmp(req->path, "/api/accounts/delete") == 0 && is_post) {
        api_accounts_delete(req);
    } else if (strcmp(req->path, "/api/accounts/settings") == 0 && is_post) {
        api_accounts_settings(req);
    } else if (strcmp(req->path, "/api/prefs") == 0) {
        api_prefs(req);
    } else if (strcmp(req->path, "/api/update") == 0 && is_get) {
        api_update_info(req);
    } else if (strcmp(req->path, "/api/update/start") == 0 && is_post) {
        api_update_start(req);
    } else if (strcmp(req->path, "/api/update/restart") == 0 && is_post) {
        api_update_restart(req);
    } else if (strcmp(req->path, "/api/info") == 0 && is_get) {
        api_info(req);
    } else if (strcmp(req->path, "/api/list") == 0 && is_get) {
        api_list(req);
    } else if (strcmp(req->path, "/api/file") == 0) {
        api_file(req);
    } else if (strcmp(req->path, "/api/move") == 0 && is_post) {
        api_move(req);
    } else if (strcmp(req->path, "/api/delete") == 0 && is_post) {
        api_delete(req);
    } else if (strcmp(req->path, "/api/run") == 0 && is_post) {
        api_run(req);
    } else if (strcmp(req->path, "/api/stop") == 0 && is_post) {
        api_stop(req);
    } else if (strcmp(req->path, "/api/output") == 0 && is_get) {
        api_output(req);
    } else if (strcmp(req->path, "/api/db") == 0 && is_get) {
        api_db(req);
    } else if (strcmp(req->path, "/api/rmdir") == 0 && is_post) {
        api_rmdir(req);
    } else if (strcmp(req->path, "/api/phone/rename") == 0 && is_post) {
        api_phone_rename(req);
    } else if (strcmp(req->path, "/api/quit") == 0 && is_post) {
        respond_ok(req);
        g_running = 0;
    } else {
        respond_error(req, 404, "unknown API call");
    }
}

/* ---------------------------------------------------------------------
 * Start-up
 * --------------------------------------------------------------------- */

static void make_token(void) {
    uint8_t r[16] = { 0 };

    random_bytes(r, sizeof(r));
    to_hex(g_token, r, sizeof(r));
}

static int dir_writable(const char *dir) {
    char  test[PATH_LEN];
    FILE *f;

    join_path(test, sizeof(test), dir, ".mm_gui_write_test");
    if ((f = fopen(test, "wb")) == NULL) {
        return 0;
    }
    fclose(f);
    (void)remove(test);
    return 1;
}

/* Documents\mmumblephone on Windows, ~/mmumblephone elsewhere. */
static void user_data_dir(char *out, size_t len) {
#ifdef _WIN32
    char docs[MAX_PATH];

    if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs) == S_OK) {
        join_path(out, len, docs, "mmumblephone");
        return;
    }
    join_path(out, len, getenv("USERPROFILE") ? getenv("USERPROFILE") : ".", "mmumblephone");
#else
    const char *home = getenv("HOME");

    join_path(out, len, (home && home[0]) ? home : ".", "mmumblephone");
#endif /* _WIN32 */
}

/* Copy the files under src into dst, keeping any that dst already has.  html/ stays behind. */
static void copy_tree(const char *src, const char *dst) {
    char from[PATH_LEN], to[PATH_LEN];

    if (!is_dir(dst)) {
        (void)do_mkdir(dst);
    }
#ifdef _WIN32
    {
        char             pattern[PATH_LEN];
        WIN32_FIND_DATAA fd;
        HANDLE           h;

        join_path(pattern, sizeof(pattern), src, "*");
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            const char *name = fd.cFileName;
#else
    {
        DIR           *d = opendir(src);
        struct dirent *e;

        if (d == NULL) {
            return;
        }
        while ((e = readdir(d)) != NULL) {
            const char *name = e->d_name;
#endif /* _WIN32 */
            if (name[0] == '.' || strcmp(name, "html") == 0) {
                continue;
            }
            join_path(from, sizeof(from), src, name);
            join_path(to, sizeof(to), dst, name);
            if (is_dir(from)) {
                copy_tree(from, to);
            } else if (!is_file(to)) {
                size_t         len;
                unsigned char *data = read_file(from, &len);

                if (data != NULL) {
                    (void)write_file(to, (const char *)data, len);
                    free(data);
                }
            }
#ifdef _WIN32
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
        }
        closedir(d);
    }
#endif /* _WIN32 */
}

/* Pick the first candidate folder containing marker. */
static int pick_dir(char *out, size_t len, const char **candidates, int n, const char *marker) {
    for (int i = 0; i < n; i++) {
        char test[PATH_LEN];

        if (candidates[i] == NULL || candidates[i][0] == '\0') {
            continue;
        }
        join_path(test, sizeof(test), candidates[i], marker);
        if (is_dir(test) || is_file(test)) {
            snprintf(out, len, "%s", candidates[i]);
            absolute(out, len);
            return 1;
        }
    }
    return 0;
}

static void open_browser(const char *url) {
#if defined(_WIN32)
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    char *argv[3];
    pid_t pid;

# ifdef __APPLE__
    argv[0] = "open";
# else
    argv[0] = "xdg-open";
# endif /* __APPLE__ */
    argv[1] = (char *)url;
    argv[2] = NULL;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0) {
        (void)waitpid(pid, NULL, 0);    /* xdg-open and open return straight away */
    }
#endif /* _WIN32 */
}

#ifdef _WIN32
static BOOL WINAPI on_signal(DWORD type) {
    (void)type;
    g_running = 0;
    return TRUE;
}
#else
static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}
#endif /* _WIN32 */

static void usage(const char *name, FILE *stream) {
    fprintf(stream,
            "usage: %s [-p <port>] [-w <working folder>] [-H <table_editor.html>] [-L <address>] [-n] [-V]\n"
            "\t-p <port> - Local port for the control panel (default: %d, or the next free one).\n"
            "\t-L <address> - Also answer other computers on this address (0.0.0.0 for all of this computer's\n"
            "\t               addresses).  They must sign in, so an admin account must be added first.  The\n"
            "\t               connection isn't encrypted: use it only on a network you trust, or through a VPN.\n"
            "\t-w <folder> - Working folder with tables/ and config/ (default: found automatically).\n"
            "\t-H <file> - The table_editor.html page to serve (default: found automatically).\n"
            "\t-n - Don't open a web browser.\n"
            "\t-V - Print the version and exit.\n"
            "\t-h - This help.\n",
            name, DEFAULT_PORT);
}

int main(int argc, char *argv[]) {
    char          self[PATH_LEN], up[PATH_LEN], share[PATH_LEN], cwd[PATH_LEN] = "";
    char          url[128];
    const char   *opt_root = NULL, *opt_html = NULL;
    int           open_it = 1, port_given = 0;
    osal_socket_t lsock = OSAL_INVALID_SOCKET;
    struct sockaddr_in addr;
    uint32_t      listen_addr = htonl(INADDR_LOOPBACK);
    int           html_given = 0;

    if (argc == 4 && strcmp(argv[1], "--update") == 0) {
        /* The update center's job: see update_main(). */
        exe_dir(g_bindir, sizeof(g_bindir), argv[0]);
        absolute(g_bindir, sizeof(g_bindir));
        return update_main(argv[2], argv[3]);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
            port_given = 1;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            opt_root = argv[++i];
        } else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            opt_html = argv[++i];
            html_given = 1;
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            copy_str(g_listen, sizeof(g_listen), argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0) {
            open_it = 0;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0], stdout);
            return 0;
        } else if (strcmp(argv[i], "-V") == 0) {
            printf("mmumblephone control panel %s\n", CMAKE_VERSION_STR);
            return 0;
        } else {
            usage(argv[0], stderr);
            return -EINVAL;
        }
    }
    if (g_port <= 0 || g_port > 65535) {
        fprintf(stderr, "mm_gui: bad port.\n");
        return -EINVAL;
    }
    if (g_listen[0]) {
        struct in_addr a;

        if (inet_pton(AF_INET, g_listen, &a) != 1) {
            fprintf(stderr, "mm_gui: -L takes an IPv4 address of this computer, such as 192.168.1.10, or 0.0.0.0 for all of them.\n");
            return -EINVAL;
        }
        listen_addr = a.s_addr;
        g_listen_any = 1;
        if (listen_addr == htonl(INADDR_ANY)) {
            g_listen[0] = '\0';    /* no one name to check the Host header against */
        }
    }

    printf("mmumblephone control panel %s [%s]\n", CMAKE_VERSION_STR, GIT_VERSION_STR);

    exe_dir(self, sizeof(self), argv[0]);
    absolute(self, sizeof(self));
    snprintf(g_bindir, sizeof(g_bindir), "%s", self);
    join_path(up, sizeof(up), self, "..");
    join_path(share, sizeof(share), self, "../share/mm_manager");
    if (do_getcwd(cwd, sizeof(cwd)) == NULL) {
        cwd[0] = '\0';
    }

    /* The working folder: where mm_manager finds tables/, config/ and keeps mm_manager.db. */
    if (opt_root) {
        snprintf(g_root, sizeof(g_root), "%s", opt_root);
        if (!is_dir(g_root) && do_mkdir(g_root) != 0) {
            fprintf(stderr, "mm_gui: can't use working folder %s: %s\n", g_root, strerror(errno));
            return -ENOENT;
        }
        absolute(g_root, sizeof(g_root));
    } else {
        const char *roots[] = { cwd, share, up, self, MM_SOURCE_DIR };

        if (!pick_dir(g_root, sizeof(g_root), roots, 5, "tables") &&
            !pick_dir(g_root, sizeof(g_root), roots, 5, "config")) {
            snprintf(g_root, sizeof(g_root), "%s", cwd[0] ? cwd : self);
        }

        /*
         * Installed somewhere read-only, like Program Files: work in a folder of the
         * user's own, starting from a copy of the installed tables and config.
         */
        if (!dir_writable(g_root)) {
            char mine[PATH_LEN], marker[PATH_LEN];

            user_data_dir(mine, sizeof(mine));
            join_path(marker, sizeof(marker), mine, "tables");
            if (!is_dir(marker)) {
                printf("Copying the starting tables and config to %s\n", mine);
                copy_tree(g_root, mine);
            }
            if (!is_dir(mine) || !dir_writable(mine)) {
                fprintf(stderr, "mm_gui: can't write to %s or %s.  Choose a working folder with -w.\n", g_root, mine);
                return -EACCES;
            }
            copy_str(g_root, sizeof(g_root), mine);
            absolute(g_root, sizeof(g_root));
        }
    }

    if (opt_html) {
        snprintf(g_html, sizeof(g_html), "%s", opt_html);
        absolute(g_html, sizeof(g_html));
    } else {
        char        root_html[PATH_LEN], share_html[PATH_LEN], up_html[PATH_LEN], src_html[PATH_LEN] = "";
        const char *htmls[5];

        join_path(root_html, sizeof(root_html), g_root, "html");
        join_path(share_html, sizeof(share_html), share, "html");
        join_path(up_html, sizeof(up_html), up, "html");
        if (MM_SOURCE_DIR[0]) join_path(src_html, sizeof(src_html), MM_SOURCE_DIR, "html");
        htmls[0] = root_html; htmls[1] = share_html; htmls[2] = up_html; htmls[3] = src_html; htmls[4] = self;
        if (pick_dir(up_html, sizeof(up_html), htmls, 5, "table_editor.html")) {
            join_path(g_html, sizeof(g_html), up_html, "table_editor.html");
        } else {
            join_path(g_html, sizeof(g_html), root_html, "table_editor.html");
        }
    }

    {
        const char *dirs[] = { self, up, g_root, MM_SOURCE_DIR };

        if (!pick_dir(g_scripts, sizeof(g_scripts), dirs, 4, "generate_npa.py")) {
            snprintf(g_scripts, sizeof(g_scripts), "%s", self);
        }
    }

    if (do_chdir(g_root) != 0) {
        fprintf(stderr, "mm_gui: can't change to %s: %s\n", g_root, strerror(errno));
        return -ENOENT;
    }

    /* Children inherit these: unbuffered output, so the log keeps up. */
#ifdef _WIN32
    SetEnvironmentVariableA("MM_GUI", "1");
    SetEnvironmentVariableA("PYTHONUNBUFFERED", "1");
    SetConsoleCtrlHandler(on_signal, TRUE);
#else
    setenv("MM_GUI", "1", 1);
    setenv("PYTHONUNBUFFERED", "1", 1);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif /* _WIN32 */

    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
        osal_mutex_init(&jobs[i].lock);
        jobs[i].buf = (char *)malloc(JOB_BUF_MAX);
        if (jobs[i].buf == NULL) {
            fprintf(stderr, "mm_gui: out of memory.\n");
            return -ENOMEM;
        }
    }
    update_cleanup();
    make_token();
    find_python();
    load_accounts();

    if (osal_net_init() != 0) {
        return -EIO;
    }
    for (int tries = 0; tries < (port_given ? 1 : 20); tries++, g_port++) {
        int one = 1;

        lsock = socket(AF_INET, SOCK_STREAM, 0);
        if (lsock == OSAL_INVALID_SOCKET) {
            break;
        }
#ifdef _WIN32
        SetHandleInformation((HANDLE)lsock, HANDLE_FLAG_INHERIT, 0);
        (void)one;
        /* Windows lets two programs share a port with SO_REUSEADDR, so ask for it exclusively. */
        setsockopt(lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#else
        fcntl(lsock, F_SETFD, FD_CLOEXEC);
        setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#endif /* _WIN32 */
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = listen_addr;
        addr.sin_port        = htons((uint16_t)g_port);
        if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(lsock, 16) == 0) {
            break;
        }
        osal_close_socket(lsock);
        lsock = OSAL_INVALID_SOCKET;
    }
    if (lsock == OSAL_INVALID_SOCKET) {
        fprintf(stderr, "mm_gui: couldn't listen on %s:%d.  Try another port with -p.\n",
                g_listen_any ? (g_listen[0] ? g_listen : "0.0.0.0") : "127.0.0.1", g_port);
        return -EADDRINUSE;
    }

    snprintf(url, sizeof(url), "http://%s:%d/", g_listen[0] ? g_listen : "127.0.0.1", g_port);
    printf("Working folder: %s\n", g_root);
    printf("Programs:       %s\n", g_bindir);
    printf("Page:           %s%s\n", g_html, is_file(g_html) ? "" : " (not found!)");
    printf("Python:         %s\n", g_python_version[0] ? g_python_version : "not found (only needed for the table generators)");
    printf("Accounts:       %s\n", g_naccounts ? "on: everyone signs in" : "none: anyone on this computer can use the control panel");
    if (g_listen_any) {
        printf("Other computers: on port %d of %s.%s\n", g_port, g_listen[0] ? g_listen : "every address of this computer",
               g_naccounts ? "" : "  Add an admin account first: until then, only this computer is answered.");
        printf("                 The connection isn't encrypted, so only do this on a network you trust.\n");
    }
    printf("\nThe control panel is at %s\n", url);
    printf("Leave this window open while you use it.  Close it (or press ^C) to quit.\n");
    fflush(stdout);
    if (open_it) {
        open_browser(url);
    }

    while (g_running) {
        fd_set         rd;
        struct timeval tv = { 0, 500000 };
        request_t      req;
        osal_socket_t  s;
        int            status;

        jobs_tick();
        FD_ZERO(&rd);
        FD_SET(lsock, &rd);
        if (select((int)lsock + 1, &rd, NULL, NULL, &tv) <= 0) {
            continue;
        }
        {
            struct sockaddr_in peer, self_addr;
            osal_socklen_t     plen = sizeof(peer), slen = sizeof(self_addr);

            memset(&req, 0, sizeof(req));
            s = accept(lsock, (struct sockaddr *)&peer, &plen);
            if (s == OSAL_INVALID_SOCKET) {
                continue;
            }
            /* From this computer: a loopback address, or the address it was sent to (one of ours). */
            req.local = (ntohl(peer.sin_addr.s_addr) >> 24) == 127 ||
                        (getsockname(s, (struct sockaddr *)&self_addr, &slen) == 0 &&
                         self_addr.sin_addr.s_addr == peer.sin_addr.s_addr);
        }
#ifdef _WIN32
        {
            DWORD timeout = 10000;

            SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
        }
#else
        {
            struct timeval timeout = { 10, 0 };

            fcntl(s, F_SETFD, FD_CLOEXEC);
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
        }
#endif /* _WIN32 */
        req.sock = s;
        status = read_request(&req);
        if (status == 0) {
            handle(&req);
        } else if (status > 0) {
            snprintf(req.method, sizeof(req.method), "GET");
            respond_error(&req, status, "bad request");
        }
        free(req.body);
        osal_close_socket(s);
    }

    printf("\nmm_gui: shutting down.\n");
    osal_close_socket(lsock);
    for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
        job_stop(&jobs[i]);
    }
    for (int wait = 0; wait < 2 * STOP_GRACE_MS / 100; wait++) {
        int busy = 0;

        jobs_tick();
        for (size_t i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
            osal_mutex_lock(&jobs[i].lock);
            busy |= jobs[i].running;
            osal_mutex_unlock(&jobs[i].lock);
        }
        if (!busy) {
            break;
        }
        osal_sleep_ms(100);
    }
    if (g_restart) {
        return restart_self(argc, argv, html_given);
    }
    return 0;
}
