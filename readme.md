# 使用 trash-cli + LD_PRELOAD 来删除文件

针对 **Ubuntu 开发环境** 构建的高可靠误删防护方案

---

## 🛠️ 方案摘要：底层劫持与逻辑重定向防护

### 1. 方案目标

解决 VS Code 在 SSH 模式下删除文件不经过回收站、绕过 Shell 别名（Alias）的痛点。通过底层注入，强制系统将物理删除（`unlink`）指令重定向为移动到回收站（`trash-put`）。

### 2. 核心组件

- **`trash-cli`**：作为后端支撑，负责管理 `~/.local/share/Trash` 下的元数据（`.trashinfo`）与文件存储。
- **`trash_intercept_v2.so`**：基于 `LD_PRELOAD` 技术编写的动态链接库，作为“系统调用防火墙”拦截删除指令。
- 基于 LD_PRELOAD 系统劫持 sys unlink 和 unlinkat , 当用户目录使用 rm 删除文件使，强制改用 trash put 回收站模式


---

### 3. 拦截逻辑三要素 (Logic Rules)

该方案通过 C 语言实现了精准的“架构级”过滤，避免对系统造成副作用：

| 维度           | 过滤规则                  | 目的                                                                |
| -------------- | ------------------------- | ------------------------------------------------------------------- |
| **身份识别**   | `UID >= 1000`             | 仅保护人类用户（如 `lane`），放过 `root` 和系统服务的日常清理。     |
| **路径校准**   | `realpath()` 解析         | 将相对路径（`./file`）统一转为绝对路径，确保拦截判定无死角。        |
| **前缀保护**   | `startswith("/home/")`    | 核心防护范围锁定在用户HOME目录，不干涉系统分区（`/usr`, `/bin` 等）。 |
| **白名单排除** | `.git/`, `logs/`, `/tmp/` | 对高频变动的开发元数据和日志不进行劫持，防止回收站爆炸。            |

---

### 4. 关键实现点

- **原子性替代**：拦截 `unlink` 和 `unlinkat` 两个关键 C 函数，覆盖了从简单的 `rm` 到复杂的 `rm -rf` 所有调用路径。
- **透明化执行**：使用 `> /dev/null 2>&1` 屏蔽 `trash-put` 的输出，确保 VS Code 的进程间通信（IPC）不会因为意外的文本输出而报错。
- **内存安全**：在 C 代码中严格管理 `realpath` 产生的堆内存申请与释放，防止长期运行产生内存泄漏。

---

### 5. 部署路径

1. **安装依赖**：
   - `sudo apt install trash-cli`
2. **编译库文件**：
   `gcc -fPIC -shared -o ~/libtrash_v2.so trash_intercept.c -ldl`
3. **VS Code 环境变量注入**：
   在 `settings.json` 的 `terminal.integrated.env.linux` 中添加 `LD_PRELOAD` 路径。
4. **服务重启**：
   通过 `Remote-SSH: Kill VS Code Server on Host...` 强制让 VS Code 服务加载新的环境变量。

---

### 6. 维护与自救建议

- **查看回收站**：使用 `trash-list` 指令。
- **恢复文件**：使用 `trash-restore` 指令（按索引号交互恢复）。
- **风险边界**：本方案属于“应用层劫持”。对于静态编译（Static Link）或直接使用原始系统调用的二进制程序无效，但在 VS Code 这种基于 Node.js/Glibc 的环境下具有极高的防御效力。
- **更多参考**：
  - [trash-cli 官方文档](https://github.com/andreafrancia/trash-cli)
  - [LD_PRELOAD 技术详解](https://man7.org/linux/man-pages/man8/ld.so.8.html)
---
