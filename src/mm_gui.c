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
#include <sys/types.h>
#include <sys/stat.h>

#include "mm_osal.h"
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

/* List a folder as JSON, descending up to depth levels. */
static void list_dir(sbuf_t *out, const char *rel, int depth, int *count) {
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
            sb_puts(out, (*count)++ ? ",{\"path\":" : "{\"path\":");
            sb_jstr(out, child);
            sb_printf(out, ",\"dir\":%s,\"size\":%llu,\"mtime\":%lld}", dir ? "true" : "false",
                      (unsigned long long)size, (long long)mtime);
            if (dir && depth > 0) {
                list_dir(out, child, depth - 1, count);
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
            if (do_stat(child, &st) != 0) {
                continue;
            }
            dir = S_ISDIR_(st.st_mode);
            sb_puts(out, (*count)++ ? ",{\"path\":" : "{\"path\":");
            sb_jstr(out, child);
            sb_printf(out, ",\"dir\":%s,\"size\":%llu,\"mtime\":%lld}", dir ? "true" : "false",
                      (unsigned long long)st.st_size, (long long)st.st_mtime);
            if (dir && depth > 0) {
                list_dir(out, child, depth - 1, count);
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

static job_t jobs[2] = { { .name = "manager" }, { .name = "task" } };

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

typedef struct {
    osal_socket_t sock;
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
    const char *reason = (status == 200) ? "OK" : (status == 400) ? "Bad Request" : (status == 403) ? "Forbidden" :
                         (status == 404) ? "Not Found" : (status == 409) ? "Conflict" : (status == 413) ? "Payload Too Large" :
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

/* Only accept requests addressed to this computer by name (stops DNS rebinding). */
static int host_ok(const char *host) {
    char ok[3][64];

    snprintf(ok[0], sizeof(ok[0]), "127.0.0.1:%d", g_port);
    snprintf(ok[1], sizeof(ok[1]), "localhost:%d", g_port);
    snprintf(ok[2], sizeof(ok[2]), "[::1]:%d", g_port);
    for (int i = 0; i < 3; i++) {
        if (strcmp(host, ok[i]) == 0) {
            return 1;
        }
    }
    return 0;
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
    sb_jstr(&b, g_root);
    sb_puts(&b, ",\"bindir\":");
    sb_jstr(&b, g_bindir);
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
    sb_puts(&b, "{\"ok\":true,\"exists\":");
    sb_puts(&b, (dir[0] == '\0' || is_dir(dir)) ? "true" : "false");
    sb_puts(&b, ",\"entries\":[");
    list_dir(&b, dir, depth, &count);
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
    if (job == NULL) {
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
 */
static void api_db(request_t *req) {
    sqlite3      *db = NULL;
    sqlite3_stmt *st = NULL;
    sbuf_t        b = { 0 };
    char          table[80], limit_s[16], offset_s[16];
    char          sql[256];
    int           limit, offset;

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
                snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\"", name);
                if (sqlite3_prepare_v2(db, sql, -1, &cnt, NULL) == SQLITE_OK && sqlite3_step(cnt) == SQLITE_ROW) {
                    rows = sqlite3_column_int64(cnt, 0);
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
    snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\" ORDER BY rowid DESC LIMIT %d OFFSET %d", table, limit, offset);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\" LIMIT %d OFFSET %d", table, limit, offset);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
            respond_error(req, 404, sqlite3_errmsg(db));
            sqlite3_close(db);
            return;
        }
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
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\"", table);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
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
            char sql[256];
            int  has_id = 0;

            snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", names[i]);
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW) {
                    const char *col = (const char *)sqlite3_column_text(st, 1);

                    if (col != NULL && strcmp(col, "TERMINAL_ID") == 0) {
                        has_id = 1;
                    }
                }
            }
            sqlite3_finalize(st);
            if (!has_id) {
                continue;
            }
            snprintf(sql, sizeof(sql), "UPDATE OR IGNORE \"%s\" SET TERMINAL_ID = ?1 WHERE TERMINAL_ID = ?2", names[i]);
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, to, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 2, from, -1, SQLITE_STATIC);
                if (sqlite3_step(st) == SQLITE_DONE) {
                    moved += sqlite3_changes(db);
                }
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }
    sb_printf(&b, "{\"ok\":true,\"records\":%lld}", moved);
    respond_json(req, 200, &b);
}

static void handle(request_t *req) {
    int is_get  = strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0;
    int is_post = strcmp(req->method, "POST") == 0;

    if (!host_ok(req->host)) {
        respond_error(req, 403, "mm_gui only answers requests for 127.0.0.1 or localhost");
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
    if (strcmp(req->path, "/api/info") == 0 && is_get) {
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
    uint32_t r[4] = { 0 };

#ifdef _WIN32
    for (int i = 0; i < 4; i++) {
        unsigned int v = 0;

        if (rand_s(&v) != 0) {
            v = osal_random32();
        }
        r[i] = v;
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");

    if (f == NULL || fread(r, sizeof(r), 1, f) != 1) {
        for (int i = 0; i < 4; i++) {
            r[i] ^= osal_random32();
        }
    }
    if (f) fclose(f);
#endif /* _WIN32 */
    snprintf(g_token, sizeof(g_token), "%08x%08x%08x%08x", r[0], r[1], r[2], r[3]);
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
            "usage: %s [-p <port>] [-w <working folder>] [-H <table_editor.html>] [-n]\n"
            "\t-p <port> - Local port for the control panel (default: %d, or the next free one).\n"
            "\t-w <folder> - Working folder with tables/ and config/ (default: found automatically).\n"
            "\t-H <file> - The table_editor.html page to serve (default: found automatically).\n"
            "\t-n - Don't open a web browser.\n"
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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
            port_given = 1;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            opt_root = argv[++i];
        } else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            opt_html = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0) {
            open_it = 0;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0], stdout);
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
    make_token();
    find_python();

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
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons((uint16_t)g_port);
        if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(lsock, 16) == 0) {
            break;
        }
        osal_close_socket(lsock);
        lsock = OSAL_INVALID_SOCKET;
    }
    if (lsock == OSAL_INVALID_SOCKET) {
        fprintf(stderr, "mm_gui: couldn't listen on 127.0.0.1:%d.  Try another port with -p.\n", g_port);
        return -EADDRINUSE;
    }

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g_port);
    printf("Working folder: %s\n", g_root);
    printf("Programs:       %s\n", g_bindir);
    printf("Page:           %s%s\n", g_html, is_file(g_html) ? "" : " (not found!)");
    printf("Python:         %s\n", g_python_version[0] ? g_python_version : "not found (only needed for the table generators)");
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
        s = accept(lsock, NULL, NULL);
        if (s == OSAL_INVALID_SOCKET) {
            continue;
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
        memset(&req, 0, sizeof(req));
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
    return 0;
}
