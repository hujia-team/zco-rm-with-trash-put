#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>
#include <errno.h>

// --- 配置区 ---
const uid_t PROTECTED_UID_GE = 1000;
const char *PROTECTED_PREFIX = "/home/";
const char *EXCLUDE_PATTERNS[] = { "/.git/", "/logs/", "/tmp/", "/.build/", "/dist/", "/__pycache__/", NULL };

// --- 路径规范化 (Python os.path.abspath 模拟) ---
void normalize_path(char *path) {
    char *stack[PATH_MAX / 2];
    int top = 0;
    char *saveptr;
    char *dup = strdup(path);
    char *token = strtok_r(dup, "/", &saveptr);

    while (token != NULL) {
        if (strcmp(token, "..") == 0) { if (top > 0) top--; }
        else if (strcmp(token, ".") != 0) { stack[top++] = token; }
        token = strtok_r(NULL, "/", &saveptr);
    }

    char *dst = path;
    if (path[0] == '/') *dst++ = '/';
    for (int i = 0; i < top; i++) {
        size_t len = strlen(stack[i]);
        memcpy(dst, stack[i], len);
        dst += len;
        if (i < top - 1) *dst++ = '/';
    }
    *dst = '\0';
    if (path[0] == '/' && top == 0) strcpy(path, "/");
    free(dup);
}

char *get_abspath(const char *pathname) {
    if (!pathname) return NULL;
    char *full_path = malloc(PATH_MAX);
    if (pathname[0] == '/') {
        strncpy(full_path, pathname, PATH_MAX);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) snprintf(full_path, PATH_MAX, "%s/%s", cwd, pathname);
        else { free(full_path); return NULL; }
    }
    normalize_path(full_path);
    return full_path;
}

// --- 核心：高性能原生回收站逻辑 ---
int fast_trash_put(const char *abs_path) {
    const char *home = getenv("HOME");
    if (!home) return -1;

    char trash_files_dir[PATH_MAX], trash_info_dir[PATH_MAX];
    snprintf(trash_files_dir, PATH_MAX, "%s/.local/share/Trash/files", home);
    snprintf(trash_info_dir, PATH_MAX, "%s/.local/share/Trash/info", home);

    // 1. 获取文件名并生成唯一标识（增加微秒级随机，防止高频构建重名）
    const char *filename = strrchr(abs_path, '/');
    filename = (filename) ? filename + 1 : abs_path;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long unique_id = (long long)tv.tv_sec * 1000000 + tv.tv_usec;

    char dest_path[PATH_MAX], info_path[PATH_MAX];
    snprintf(dest_path, PATH_MAX, "%s/%s_%lld", trash_files_dir, filename, unique_id);
    snprintf(info_path, PATH_MAX, "%s/%s_%lld.trashinfo", trash_info_dir, filename, unique_id);

    // 2. 写入 .trashinfo (符合 FreeDesktop 标准，trash-cli 可读)
    FILE *fp = fopen(info_path, "w");
    if (fp) {
        time_t t = time(NULL);
        char date_buf[64];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", localtime(&t));
        fprintf(fp, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", abs_path, date_buf);
        fclose(fp);
    }

    // 3. 执行原子移动 (同一分区耗时与 unlink 持平)
    if (rename(abs_path, dest_path) == 0) return 0;

    // 如果失败（如跨分区 EXDEV），删除生成的 info 并由后续 original_unlink 处理
    unlink(info_path); 
    return -1;
}

int should_intercept(const char *pathname) {
    if (!pathname || getuid() < PROTECTED_UID_GE) return 0;
    char *abs_path = get_abspath(pathname);
    if (!abs_path) return 0;

    int intercept = 0;
    if (strncmp(abs_path, PROTECTED_PREFIX, strlen(PROTECTED_PREFIX)) == 0) {
        intercept = 1;
        for (int i = 0; EXCLUDE_PATTERNS[i] != NULL; i++) {
            if (strstr(abs_path, EXCLUDE_PATTERNS[i])) { intercept = 0; break; }
        }
    }
    free(abs_path);
    return intercept;
}

// --- 拦截器接口 ---
int unlink(const char *pathname) {
    int (*orig)(const char *) = dlsym(RTLD_NEXT, "unlink");
    if (should_intercept(pathname)) {
        if (fast_trash_put(pathname) == 0) return 0;
    }
    return orig(pathname);
}

int unlinkat(int dirfd, const char *pathname, int flags) {
    int (*orig)(int, const char *, int) = dlsym(RTLD_NEXT, "unlinkat");
    // 如果设置了 AT_REMOVEDIR，unlinkat 行为等同于 rmdir，逻辑一致
    if (should_intercept(pathname)) {
        if (fast_trash_put(pathname) == 0) return 0;
    }
    return orig(dirfd, pathname, flags);
}
