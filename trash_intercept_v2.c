#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>

const char ZCO_TRASH_VERSION_V2_8[] = "v2.8-20260309";

typedef int (*orig_unlink_t)(const char *);
typedef int (*orig_unlinkat_t)(int, const char *, int);
typedef int (*orig_rename_t)(const char *, const char *);

static orig_unlink_t   real_unlink   = NULL;
static orig_unlinkat_t real_unlinkat = NULL;
static orig_rename_t   real_rename   = NULL;

static __thread int in_hook = 0;

// --- 配置区 ---
const uid_t PROTECTED_UID_GE = 1000;
const char *PROTECTED_PREFIX = "/home/";
const char *EXCLUDE_PATTERNS[] = { 
    "/.", "/logs/", "/tmp/", "/dist/", "/__pycache__/", NULL 
};

// --- M_FLAG_ZCO_DEBUG 注入逻辑 ---
#ifdef M_FLAG_ZCO_DEBUG
void write_debug_log(const char *action, const char *pathname, const char *abs_path, struct stat *st, int intercepted, const char *reason) {
    FILE *fp = fopen("/tmp/zco_trash_debug.log", "a");
    if (!fp) return;

    char exe_path[PATH_MAX] = {0};
    readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
    char *proc_name = basename(exe_path);

    time_t now = time(NULL);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    fprintf(fp, "[%s][PID:%d][%s] %s: raw=%s\n", time_str, getpid(), proc_name, action, pathname);
    if (abs_path) fprintf(fp, "  -> ABS: %s\n", abs_path);
    if (st) fprintf(fp, "  -> STAT: Inode=%lu, nlink=%lu\n", (unsigned long)st->st_ino, (unsigned long)st->st_nlink);
    fprintf(fp, "  => DECISION: %s (Reason: %s)\n----------------------------------\n", 
            intercepted ? "TRASHED" : "PASSED", reason ? reason : "N/A");
    fclose(fp);
}
#define DEBUG_LOG(a, p, abs, s, i, r) write_debug_log(a, p, abs, s, i, r)
#else
#define DEBUG_LOG(a, p, abs, s, i, r) ((void)0)
#endif

__attribute__((constructor))
static void init_trash_interceptor() {
    real_unlink   = (orig_unlink_t)dlsym(RTLD_NEXT, "unlink");
    real_unlinkat = (orig_unlinkat_t)dlsym(RTLD_NEXT, "unlinkat");
    real_rename   = (orig_rename_t)dlsym(RTLD_NEXT, "rename");
}

// --- 路径处理逻辑 ---
void normalize_path(char *path) {
    if (!path || *path == '\0') return;
    char *stack[PATH_MAX / 2];
    int top = 0, is_absolute = (path[0] == '/');
    char *saveptr, *dup = strdup(path);
    char *token = strtok_r(dup, "/", &saveptr);
    while (token != NULL) {
        if (strcmp(token, "..") == 0) { if (top > 0) top--; }
        else if (strcmp(token, ".") != 0) { stack[top++] = token; }
        token = strtok_r(NULL, "/", &saveptr);
    }
    char *dst = path;
    if (is_absolute) *dst++ = '/';
    for (int i = 0; i < top; i++) {
        size_t len = strlen(stack[i]);
        memcpy(dst, stack[i], len);
        dst += len;
        if (i < top - 1) *dst++ = '/';
    }
    *dst = '\0';
    if (is_absolute && top == 0) { path[0] = '/'; path[1] = '\0'; }
    free(dup);
}

char *get_full_path_at(int dirfd, const char *pathname) {
    if (!pathname) return NULL;
    if (pathname[0] == '/') {
        char *res = strdup(pathname);
        normalize_path(res);
        return res;
    }
    char base_path[PATH_MAX] = {0};
    if (dirfd == AT_FDCWD) {
        if (!getcwd(base_path, sizeof(base_path))) return NULL;
    } else {
        char proc_fd_path[64];
        snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d", dirfd);
        ssize_t len = readlink(proc_fd_path, base_path, sizeof(base_path) - 1);
        if (len == -1) return NULL;
        base_path[len] = '\0';
    }
    char *full_path = malloc(PATH_MAX);
    snprintf(full_path, PATH_MAX, "%s/%s", base_path, pathname);
    normalize_path(full_path);
    return full_path;
}

// --- 核心逻辑 ---
int fast_trash_put(const char *abs_path) {
    const char *home = getenv("HOME");
    if (!home || !real_rename) return -1;
    char trash_files_dir[PATH_MAX], trash_info_dir[PATH_MAX];
    snprintf(trash_files_dir, PATH_MAX, "%s/.local/share/Trash/files", home);
    snprintf(trash_info_dir, PATH_MAX, "%s/.local/share/Trash/info", home);

    const char *filename = strrchr(abs_path, '/');
    filename = (filename) ? filename + 1 : abs_path;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long unique_id = (long long)tv.tv_sec * 1000000 + tv.tv_usec;

    char dest_path[PATH_MAX], info_path[PATH_MAX];
    snprintf(dest_path, PATH_MAX, "%s/%s_%lld", trash_files_dir, filename, unique_id);
    snprintf(info_path, PATH_MAX, "%s/%s_%lld.trashinfo", trash_info_dir, filename, unique_id);

    FILE *fp = fopen(info_path, "w");
    if (fp) {
        time_t t = time(NULL);
        char date_buf[64];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", localtime(&t));
        fprintf(fp, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", abs_path, date_buf);
        fclose(fp);
    }
    if (real_rename(abs_path, dest_path) == 0) return 0;
    if (real_unlink) real_unlink(info_path);
    return -1;
}

int handle_interception(int dirfd, const char *pathname, const char *action) {
    if (in_hook || !pathname) return -1;

    char *abs_path = get_full_path_at(dirfd, pathname);
    if (!abs_path) return -1;

    int intercepted = 0;
    const char *reason = "Passed filters";
    struct stat st;

    if (fstatat(dirfd, pathname, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        // 1. 基础检查
        if (getuid() < PROTECTED_UID_GE) reason = "UID below 1000";
        else if (strncmp(abs_path, PROTECTED_PREFIX, strlen(PROTECTED_PREFIX)) != 0) reason = "Not in home";
        else if (st.st_nlink > 1) reason = "Hardlink count > 1";
        else {
            // 2. EXCLUDE_PATTERNS 检查 (回归补回)
            int excluded = 0;
            for (int i = 0; EXCLUDE_PATTERNS[i] != NULL; i++) {
                if (strstr(abs_path, EXCLUDE_PATTERNS[i])) {
                    excluded = 1;
                    reason = "Matched EXCLUDE_PATTERNS";
                    break;
                }
            }
            // 3. 执行拦截
            if (!excluded) {
                in_hook = 1;
                if (fast_trash_put(abs_path) == 0) {
                    intercepted = 1;
                    reason = "Success";
                } else {
                    reason = "fast_trash_put failed";
                }
                in_hook = 0;
            }
        }
        DEBUG_LOG(action, pathname, abs_path, &st, intercepted, reason);
    } else {
        DEBUG_LOG(action, pathname, abs_path, NULL, 0, "fstatat failed");
    }

    free(abs_path);
    return intercepted ? 0 : -1;
}

int unlink(const char *pathname) {
    if (handle_interception(AT_FDCWD, pathname, "UNLINK") == 0) return 0;
    return real_unlink(pathname);
}

int unlinkat(int dirfd, const char *pathname, int flags) {
    if (!(flags & AT_REMOVEDIR)) {
        if (handle_interception(dirfd, pathname, "UNLINKAT") == 0) return 0;
    }
    return real_unlinkat(dirfd, pathname, flags);
}
