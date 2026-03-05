#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h> // 为了使用 PATH_MAX
#include <time.h>

// --- 配置区 ---
const uid_t PROTECTED_UID_GE = 1000;
const char *PROTECTED_PREFIX = "/home/";

const char *EXCLUDE_PATTERNS[] = {
    "/.",
    "/.git/",
    "/logs/",
    "/tmp/",
    "/lib/",
    "/bin/",
    NULL
};

// 获取日志文件路径
const char* get_log_path(void) {
    const char* log_path = getenv("ZCO_TRASH_LOG");
    if (log_path && strlen(log_path) > 0) {
        return log_path;
    }
    static char default_path[PATH_MAX];
    const char* home = getenv("HOME");
    if (home) {
        snprintf(default_path, sizeof(default_path), "%s/.zco-trash-log", home);
    } else {
        snprintf(default_path, sizeof(default_path), "/tmp/.zco-trash-log");
    }
    return default_path;
}

// 记录日志
void log_trash_failure(const char* pathname, int ret) {
    const char* log_path = get_log_path();
    FILE* fp = fopen(log_path, "a");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    char cwd[PATH_MAX];
    const char* current_dir = getcwd(cwd, sizeof(cwd)) ? cwd : "unknown";

    fprintf(fp, "[%s] PWD: %s , DEST: %s , RET: %d\n", time_str, current_dir, pathname, ret);
    fclose(fp);
}

// 核心判断逻辑
int should_intercept(const char *pathname) {
    if (!pathname) return 0;

    // 1. 快速检查 UID (性能优先，如果不满足直接退出)
    if (getuid() < PROTECTED_UID_GE) return 0;

    // 2. 将路径转换为绝对路径
    char *abs_path = realpath(pathname, NULL);
    if (!abs_path) {
        // 如果 realpath 失败（文件可能已经被删或权限不足），不劫持，走原程序逻辑
        return 0;
    }

    int intercept = 0;

    // 3. 检查绝对路径前缀
    if (strncmp(abs_path, PROTECTED_PREFIX, strlen(PROTECTED_PREFIX)) == 0) {
        intercept = 1; // 初步锁定在 /home/ 下

        // 4. 检查排除项 (如 .git, logs)
        for (int i = 0; EXCLUDE_PATTERNS[i] != NULL; i++) {
            if (strstr(abs_path, EXCLUDE_PATTERNS[i]) != NULL) {
                intercept = 0; // 命中排除项，放弃劫持
                break;
            }
        }
    }

    // 记得释放 realpath 分配的内存！
    free(abs_path);
    return intercept;
}

int do_trash(const char *pathname) {
    char command[2048];
    // 使用单引号包裹路径以支持带空格和特殊字符的文件名
    snprintf(command, sizeof(command), "/usr/bin/trash-put '%s' > /dev/null 2>&1", pathname);
    int ret = system(command);
    return ret;
}

// 拦截 unlink
int unlink(const char *pathname) {
    int (*original_unlink)(const char *) = dlsym(RTLD_NEXT, "unlink");
    if (should_intercept(pathname)) {
        int ret = do_trash(pathname);
        if (ret == 0) {
            return ret;
        } else {
            log_trash_failure(pathname, ret);
        }
    }
    return original_unlink(pathname);
}

// 拦截 unlinkat
int unlinkat(int dirfd, const char *pathname, int flags) {
    int (*original_unlinkat)(int, const char *, int) = dlsym(RTLD_NEXT, "unlinkat");

    // unlinkat 的特殊性在于 pathname 可能是相对于 dirfd 的
    // 但 realpath(pathname, NULL) 会自动处理相对于当前工作目录的路径
    // 如果是基于 dirfd 的复杂相对路径，realpath 可能会失败，此时我们保守起见不劫持
    if (should_intercept(pathname)) {
        int ret = do_trash(pathname);
        if (ret == 0) {
            return ret;
        } else {
            log_trash_failure(pathname, ret);
        }
    }

    return original_unlinkat(dirfd, pathname, flags);
}
