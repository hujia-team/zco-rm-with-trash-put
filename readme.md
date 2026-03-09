# 使用 LD_PRELOAD 底层劫持来防止文件误删

针对 **Ubuntu 开发环境** 构建的高可靠误删防护方案（当前版本：`v2.8-20260309`）

---

## 🛠️ 方案摘要：底层劫持与逻辑重定向防护

### 1. 方案目标

解决 VS Code 在 SSH 模式下删除文件不经过回收站、绕过 Shell 别名（Alias）的痛点。通过底层注入，强制系统将物理删除（`unlink`）指令重定向为移动到回收站（兼容 FreeDesktop Trash 规范）。

### 2. 核心组件

- **`trash_intercept_v2.so`**：基于 `LD_PRELOAD` 技术编写的动态链接库，作为"系统调用防火墙"拦截删除指令。
- **`fast_trash_put`**：内置的轻量级回收站实现，直接通过 `rename` 系统调用将文件移入 `~/.local/share/Trash/`，并写入标准 `.trashinfo` 元数据，无需调用外部 `trash-put` 二进制。兼容 `trash-list` / `trash-restore` 等工具。

---

### 3. 拦截逻辑五要素 (Logic Rules)

| 维度           | 过滤规则                                                    | 目的                                                                |
| -------------- | ----------------------------------------------------------- | ------------------------------------------------------------------- |
| **身份识别**   | `UID >= 1000`                                               | 仅保护人类用户，放过 `root` 和系统服务的日常清理。                  |
| **路径解析**   | 自定义 `normalize_path` + `get_full_path_at`                | 将相对路径统一转为绝对路径，避免符号链接追踪引发意外行为。          |
| **前缀保护**   | `startswith("/home/")`                                      | 核心防护范围锁定在用户 HOME 目录，不干涉系统分区（`/usr`, `/bin`）。 |
| **硬链接豁免** | `st_nlink > 1`                                              | 跳过有多个硬链接的文件，避免回收站破坏引用计数语义。                |
| **排除模式**   | `"/.", "/logs/", "/tmp/", "/dist/", "/__pycache__/"`        | 对 `.git`、日志、临时文件、构建产物、Python 缓存不进行劫持，防止回收站爆炸。 |

---

### 4. 关键实现点

- **原子性替代**：拦截 `unlink` 和 `unlinkat` 两个关键 C 函数，覆盖从简单的 `rm` 到复杂的 `rm -rf` 所有调用路径。
- **零外部依赖执行**：`fast_trash_put` 直接调用 `rename` + 写文件，不 fork 子进程、不调用 `trash-put` 二进制，消除了进程调度延迟和 IPC 输出污染问题。
- **线程安全**：使用 `__thread int in_hook` 线程局部变量防止递归重入，在并发环境下安全运行。
- **内存安全**：路径解析使用 `strdup` + 手动 `free`，避免长期运行产生内存泄漏。
- **调试模式**：编译时加入 `-DM_FLAG_ZCO_DEBUG` 标志，可将每次拦截决策的详细信息追加写入 `/tmp/zco_trash_debug.log`。

---

## 📦 依赖说明

### 系统依赖
- **操作系统**：Linux (Ubuntu/Debian 系列)
- **编译器**：gcc (支持 C99 标准)
- **动态链接库**：libdl (通常系统自带)

### 可选依赖
- **trash-cli**：用于查看和恢复回收站文件（`trash-list` / `trash-restore`）
  - 安装命令：`sudo apt update && sudo apt install trash-cli`

---

## 🚀 安装说明

### 方式一：自动安装（推荐）

使用 Makefile 一键安装到 zsh 环境：

```bash
make install-zshrc
```

该命令会自动完成以下操作：
1. 检查并安装 trash-cli 依赖
2. 编译生成 `libtrash_intercept_v2.so`
3. 将库文件复制到 `~/.local/lib/zco/`
4. 自动配置 `~/.zshrc` 添加 `LD_PRELOAD` 环境变量

安装完成后，执行以下命令使配置立即生效：
```bash
export LD_PRELOAD=~/.local/lib/zco/libtrash_intercept_v2.so
```

或重新打开终端。

### 方式二：手动安装

1. **编译库文件**：
   ```bash
   make gcc-v2
   # 或手动执行：
   # gcc -fPIC -shared -o libtrash_intercept_v2.so trash_intercept_v2.c -ldl
   ```

2. **部署库文件**：
   ```bash
   mkdir -p ~/.local/lib/zco
   cp libtrash_intercept_v2.so ~/.local/lib/zco/
   ```

3. **配置环境变量**：

   在 `~/.zshrc` 或 `~/.bashrc` 中添加：
   ```bash
   export LD_PRELOAD=~/.local/lib/zco/libtrash_intercept_v2.so
   ```

4. **使配置生效**：
   ```bash
   source ~/.zshrc  # 或 source ~/.bashrc
   ```

### VS Code Remote SSH 配置

如需在 VS Code Remote SSH 环境中使用，需在 VS Code 的 `settings.json` 中添加：

```json
{
  "terminal.integrated.env.linux": {
    "LD_PRELOAD": "/home/你的用户名/.local/lib/zco/libtrash_intercept_v2.so"
  }
}
```

配置后通过 `Remote-SSH: Kill VS Code Server on Host...` 重启 VS Code 服务器。

---

## 🔧 验证安装

安装完成后，可以通过以下方式验证：

```bash
# 1. 检查环境变量
echo $LD_PRELOAD

# 2. 测试删除功能
touch test_file.txt
rm test_file.txt

# 3. 查看回收站
trash-list
```

如果 `test_file.txt` 出现在回收站列表中，说明安装成功。

---

## 🐞 调试模式

编译时加入 `-DM_FLAG_ZCO_DEBUG` 标志可开启调试日志：

```bash
gcc -fPIC -shared -DM_FLAG_ZCO_DEBUG -o libtrash_intercept_v2.so trash_intercept_v2.c -ldl
```

日志写入 `/tmp/zco_trash_debug.log`，格式示例：

```
[19:00:00][PID:1234][code] UNLINK: raw=./file.txt
  -> ABS: /home/user/project/file.txt
  -> STAT: Inode=12345, nlink=1
  => DECISION: TRASHED (Reason: Success)
----------------------------------
```

---

## 📝 维护与自救建议

- **查看回收站**：使用 `trash-list` 指令。
- **恢复文件**：使用 `trash-restore` 指令（按索引号交互恢复）。
- **风险边界**：本方案属于"应用层劫持"。对于静态编译（Static Link）或直接使用原始系统调用的二进制程序无效，但在 VS Code 这种基于 Node.js/Glibc 的环境下具有极高的防御效力。
- **更多参考**：
  - [trash-cli 官方文档](https://github.com/andreafrancia/trash-cli)
  - [LD_PRELOAD 技术详解](https://man7.org/linux/man-pages/man8/ld.so.8.html)

---

## 📄 License: MIT License
