# 经验教训（可复用）

来源：原根目录 `dev_debug_notes.md`（以 procfs/`ps` 实现为例的一轮实战记录），2026-11-17 迁入。
本文只写**教训与方法**；「现在是什么」在 `arch-userland.md` / `arch-virtpass.md`，
单次修复的条目在 `fix-*.md`。

## 1. 大型 C 单文件（~12k 行）改动

- **新增 `static` 函数前先查名字**。想加「给 procfs fd 加引用」，写成 `userland_proc_ref(int)`，
  与已存在的 `userland_proc_ref(rvvm_process_t*)` 重名 → `error: conflicting types`。
  大文件里 `userland_<领域>_<动词>` 极易撞车；成族的新函数用更细前缀（最终是
  `userland_proc_fd_ref/release`）。动手前 `grep -n "函数名"`。
- **项目自定义常量不一定跟 libc 一致**。标准是 `EACCES`，本树定义 `UAPI_EACCESS`（多一个 S）。
  用 `UAPI_*` 前先 `grep "#define UAPI_E"`，别凭 libc 记忆。
- **批量替换要带词边界**：

  ```powershell
  (Get-Content src/core/rvvm_user.c -Raw) -replace 'UAPI_EACCES\b','UAPI_EACCESS' |
      Set-Content -NoNewline src/core/rvvm_user.c
  ```

  `\b` 保证不会把已经正确的 `UAPI_EACCESS` 二次误伤成 `UAPI_EACESSS`。
- **整文件重写后必须验编码/BOM/行尾**：查首字节无 BOM、`git diff --stat` 没出现「整文件重写」。
  能用结构化编辑工具就别整文件重写。
- **编辑大文件用唯一锚点**：带上下文的短锚点最稳；模式化代码（多个 `else if`）要连前后独有注释一起选。

## 2. Windows 构建 / 运行环境

- **重新链接报 `Permission denied` = 有 exe 在跑**：
  `ld.exe: cannot open output file release.windows.x86_64\rvvm_ash_x86_64.exe: Permission denied`。
  先 `--shutdown`，再 `Get-Process ... | Stop-Process -Force`。**每次重新构建前先确认没有残留实例**，
  尤其是 autostart 的 detached core。
- **残留 core 占 `runtime/ash-core.lock`**，`--direct` 起不来、guest sample 退出码 `-1`。
  `rvvm_ash_x86_64.exe --list` 会列 `stale port=... (reclaimed)`。
  看到 sample「`EXIT=-1` / 无输出」，先想环境（锁、端口、残留进程），再想代码。
- **guest 相对路径相对进程 cwd，不是 exe 目录**：把工作目录设到 release 目录（用工具的 `workdir`，
  不要 `cd`），或写绝对路径。

## 3. 模拟虚拟文件系统

- **只实现 `read` 不够**：`cat /proc/100/cmdline` 正常但 `od -An -c` 报 `read error`，
  因为 busybox `od` 走 `readv`（部分工具走 `pread`），未分派就把合成号当 host fd 交给宿主 → `EBADF`。
  规则（现状要求）见 `arch-userland.md` §4。
- **合成 fd 的 close / fork 继承 / dup 必须全接**：fork 时默认逻辑 `dup()` 失败后标记 `shared`
  让父子共享同一个合成 fd —— 对**有状态**对象（缓冲、引用计数）会 double-free 或泄漏。
- **先想清楚对象被 fork 继承后属于谁**，再决定表的生命周期：带状态、带引用计数的合成对象用全局表最省心
  （第一版放 `rvvm_userland_t`，子进程的 `proc_fds[]` 里没有那个号，于是关不掉、引用计数永不归零）。
- **列出来的名字必须能打开 / stat**：列了 `maps`/`task` 却打不开，`ls -l /proc/<pid>` 就报错。
- **不存在的对象要在所有入口拒绝**：只在 `open` 查 pid 存在性不够，`stat`/`readlink`/`faccessat`
  都会「假成功」。

## 4. 对接外部工具时的「数据质量」反噬

现成工具会把你「没填 / 填 0」的字段**派生**成错误语义：

- `top` 的 `STAT` 出现 `W`（swapped）← `vsz/rss` 全填 0，busybox 据此判定换出。给合理的非零 vsize 即消失。
- `{vpsessiond.exe} argv...` ← `comm` 与 `argv[0]` 不一致时 busybox 自己加的 `{}`，不是 bug。
- `sysinfo(179)` 被 `#ifdef __linux__` 挡掉 ← busybox `ps` 一启动就调，Windows 上返回 ENOSYS。
  trace 里 `Unknown syscall 179!` 直接点出来。

## 5. 调试方法

- **先观测再动手**：`RVVM_TRACE=path` 看 guest 到底 `open` 了什么。定位「`ps` 为什么没结果」时，
  trace 显示只 `openat("/proc")`、读 `/etc/passwd`、调 `sysinfo`，没有 `/proc/<pid>/...`
  —— 因为当时 `/proc` 里只有 `mounts`。
- **`-c` 客户端会连到已存在的 core，环境变量传不进去**。要 trace 就 `--shutdown` 停干净，
  或显式起一个带 trace 的 core 并把 stderr 重定向到文件：

  ```powershell
  $env:RVVM_TRACE='path'
  Start-Process .\release.windows.x86_64\rvvm_ash_x86_64.exe `
      -ArgumentList '--serve','--port','7950' `
      -RedirectStandardError "$env:TEMP\core.err" -NoNewWindow
  ```

  `--direct` 的 guest 输出进窗口，stdout 可能为空；**trace 在 stderr**。
- **隔离变量：拿不经过新路径的输入做对照**。`head -c 100 /proc/self/stat` 报 `Bad address`，
  换 `/etc/passwd` 同样报错 ⇒ 是既存问题，不是新代码的锅。
- **测试脚本自己会按进程名杀进程**：`tools/*_e2e.ps1` 首尾常做
  `Get-Process -Name rvvm_ash_x86_64 | Stop-Process -Force` ⇒ 这些脚本不能并行，
  也不能和别的 ash 用例并行（曾把 `test_std` 杀成 `EXIT=-1`，改串行即恢复）。

## 6. 测试

- 断言要抓脚本**自己的** `PASS`/`FAIL`/`checks` 行；有些脚本只回显 guest 输出、靠正则判定
  （`jobctl_e2e` 按 scenario 输出，没有统一 `PASS` 字样）。
- `jobctl_e2e` 偶发 `SEND-FAILED ... 管道正在被关闭`：guest 已退出后脚本还在写 stdin 的时序噪音，
  判据看 guest 输出（`Stopped`/`fg`/`RESUME`/`exit`）。
- 顺序：先 e2e（`ash_e2e`/`session_e2e`/`procfs_e2e`），再 guest sample（`test_*`）；两类串行，
  sample 的 cwd 在 release 目录。
- 新增能力配一个专用 e2e（本次 `tools/procfs_e2e.ps1`，22 checks），比手工敲命令更可回归。

## 7. 设计推演省下的 debug

`ps` 自己是被 shell fork 出来再 exec 的子进程。若枚举「当前 ctx 的 `procs`」，只会看到 `ps` 自己一行。
先画数据流（记录同时登记在父/子两个 registry、子进程是独立 `child_ctx`）就能推出：
必须从 family root 沿 `child_ctx` DFS 并按 pid 去重枚举。**这类推演放在写代码前，比写完再对着
「只显示自己」debug 便宜得多。**

## 8. 内存占用与泄漏排查方法

1. **先算理论值**：guest RAM（`USERLAND_MEM_SIZE` ≈ 1 GiB，Win 上 `VirtualAlloc(MEM_COMMIT|MEM_RESERVE)`，
   commit 很大是预期的）+ JIT cache（`RVVM_OPT_JIT_CACHE` 16 MiB）+ 其他 ≈ 13 MiB ≈ 1053 MiB。
   实测 `PrivateMemorySize64` = 1053.6 MiB → 匹配，没泄漏。
2. **看增量不看绝对值**：反复 `rvvm_ash -c "true"` 做 churn，每次后测 commit。
   泄漏形态是 `1053.6 → +16.2, +16.1, +16.1, ...` 单调且 idle 不回收。
3. **用开关做对照实验**：`RVVM_NOJIT=1` 起 core 后 churn 完全平（1037.5→1037.6）
   ⇒ 泄漏就是 16 MiB 的 RVJIT 堆，一步定位到具体资源。
4. **指标选对**：commit（`PrivateMemorySize64`）看泄漏，WorkingSet 看实际驻留。
5. 顺序：先证明「有泄漏」（churn 单调）→ 再对照定位资源 → 最后读代码找释放路径。

该轮查出的具体缺陷与修法：`fix-userland-hart-registration.md`。
