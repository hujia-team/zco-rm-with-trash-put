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

// --- 1. 版本与符号定义 ---
const char ZCO_TRASH_VERSION_V2_4[] = "v2.4-20260309";

typedef int (*orig_unlink_t)(const char *);
typedef int (*orig_unlinkat_t)(int, const char *, int);
typedef int (*orig_rename_t)(const char *, const char *);

static orig_unlink_t   real_unlink   = NULL;
static orig_unlinkat_t real_unlinkat = NULL;
static orig_rename_t   real_rename   = NULL;

// 线程局部变量：防止 Hook 内部操作再次触发 Hook 导致死循环
static __thread int in_hook = 0;

// --- 2. 库加载初始化 ---
__attribute__((constructor))
static void init_trash_interceptor() {
    real_unlink   = (orig_unlink_t)dlsym(RTLD_NEXT, "unlink");
    real_unlinkat = (orig_unlinkat_t)dlsym(RTLD_NEXT, "unlinkat");
    real_rename   = (orig_rename_t)dlsym(RTLD_NEXT, "rename");
}

// --- 3. 配置区 ---
const uid_t PROTECTED_UID_GE = 1000;
const char *PROTECTED_PREFIX = "/home/";
const char *EXCLUDE_PATTERNS[] = { 
    "/.", "/logs/", "/tmp/", "/dist/", "/__pycache__/", "/.venv/", "/.build/", NULL 
};

// --- 4. 路径处理核心逻辑 ---

// 模拟 Python os.path.normpath: 处理 . 和 .. 但不解析符号链接
void normalize_path(char *path) {
    if (!path || *path == '\0') return;

    char *stack[PATH_MAX / 2];
    int top = 0;
    char *saveptr;
    int is_absolute = (path[0] == '/');
    
    char *dup = strdup(path);
    char *token = strtok_r(dup, "/", &saveptr);

    while (token != NULL) {
        if (strcmp(token, "..") == 0) {
            if (top > 0) top--;
        } else if (strcmp(token, ".") != 0) {
            stack[top++] = token;
        }
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
    
    // 处理根目录边界情况
    if (is_absolute && top == 0) {
        path[0] = '/';
        path[1] = '\0';
    }
    free(dup);
}

// 获取绝对路径并规范化
char *get_abspath(const char *pathname) {
    if (!pathname) return NULL;
    char *full_path = malloc(PATH_MAX);
    if (pathname[0] == '/') {
        strncpy(full_path, pathname, PATH_MAX);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(full_path, PATH_MAX, "%s/%s", cwd, pathname);
        } else {
            free(full_path);
            return NULL;
        }
    }
    normalize_path(full_path);
    return full_path;
}

// 核心判断逻辑：判断是否拦截并返回解析好的 abs_path
char* check_and_get_target_abs(const char *pathname) {
    if (!pathname || getuid() < PROTECTED_UID_GE) return NULL;

    char *abs_path = get_abspath(pathname);
    if (!abs_path) return NULL;

    // 检查前缀
    if (strncmp(abs_path, PROTECTED_PREFIX, strlen(PROTECTED_PREFIX)) == 0) {
        // 检查白名单排除项
        for (int i = 0; EXCLUDE_PATTERNS[i] != NULL; i++) {
            if (strstr(abs_path, EXCLUDE_PATTERNS[i])) {
                free(abs_path);
                return NULL;
            }
        }
        return abs_path; // 命中拦截条件，返回绝对路径
    }

    free(abs_path);
    return NULL;
}

// --- 5. 执行逻辑：高性能原子移动 ---
int fast_trash_put(const char *abs_path) {
    const char *home = getenv("HOME");
    if (!home || !real_rename) return -1;

    char trash_files_dir[PATH_MAX], trash_info_dir[PATH_MAX];
    snprintf(trash_files_dir, PATH_MAX, "%s/.local/share/Trash/files", home);
    snprintf(trash_info_dir, PATH_MAX, "%s/.local/share/Trash/info", home);

    // 获取纯文件名用于生成唯一 ID
    const char *filename = strrchr(abs_path, '/');
    filename = (filename) ? filename + 1 : abs_path;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long unique_id = (long long)tv.tv_sec * 1000000 + tv.tv_usec;

    char dest_path[PATH_MAX], info_path[PATH_MAX];
    snprintf(dest_path, PATH_MAX, "%s/%s_%lld", trash_files_dir, filename, unique_id);
    snprintf(info_path, PATH_MAX, "%s/%s_%lld.trashinfo", trash_info_dir, filename, unique_id);

    // 1. 写入 .trashinfo 元数据 (使用已经解析好的 abs_path)
    FILE *fp = fopen(info_path, "w");
    if (fp) {
        time_t t = time(NULL);
        char date_buf[64];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", localtime(&t));
        fprintf(fp, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", abs_path, date_buf);
        fclose(fp);
    }

    // 2. 执行原子移动 (使用 abs_path 避免相对路径失效)
    if (real_rename(abs_path, dest_path) == 0) {
        return 0;
    }

    // 3. 失败清理
    if (real_unlink) real_unlink(info_path);
    return -1;
}

// --- 6. 系统调用拦截入口 ---

int unlink(const char *pathname) {
    char *target_abs = NULL;
    // 只有当不在 Hook 递归中，且命中拦截路径时才处理
    if (!in_hook && (target_abs = check_and_get_target_abs(pathname))) {
        in_hook = 1;
        int ret = fast_trash_put(target_abs);
        in_hook = 0;
        free(target_abs);
        if (ret == 0) return 0;
    }
    return real_unlink(pathname);
}

int unlinkat(int dirfd, const char *pathname, int flags) {
    char *target_abs = NULL;
    // 处理逻辑同 unlink
    if (!in_hook && (target_abs = check_and_get_target_abs(pathname))) {
        in_hook = 1;
        int ret = fast_trash_put(target_abs);
        in_hook = 0;
        free(target_abs);
        if (ret == 0) return 0;
    }
    return real_unlinkat(dirfd, pathname, flags);
}
