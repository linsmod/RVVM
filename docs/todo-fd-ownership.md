# 待办：宿主号私有化（fd 所有权）

> 计划层文档。本文件只写要做什么、优先级、依赖与验收，不记录执行过程。
> 目标形态与不变量见 `design-fd-ownership.md`；问题证据见 `../issues/fork-shared-crt-fd-space.md`。

## 总目标

每个 address space 的槽独占它引用的宿主号；共享只发生在对象层（inode / pty / SOCKET）。完成后「号会不会被别人关掉」不再需要回答，以下四类代码全部消失：`shared` 字段、`host_fd_refs` 计数表、`userland_fd_host` 的 stale 判定、按数字猜锚类型的分支。

## P1：fork 时私有化

**优先级：高。** 做完这一步，tinyssh 会话的阻断点应当消失。

要做：

- `userland_fd_table_inherit`：普通 HOST 槽一律 `dup()`；socket 槽一律 `win_socket_dup()`；`dup` 失败把子槽标为无效（`used=false`），不再退化成 `shared=true`
- 删除 `rvvm_fd_entry_t.shared` 及全部分支（`inherit`、`userland_fd_close`、`fds_write`）
- `userland_fd_close`：普通号直接 `close()`，删除「归还别人的引用」路径

依赖：无，可在当前分支直接做。

验收：

- 父进程 `close()` 不影响已 fork 的子进程；重复 close 不误伤
- `execve` 之后 `write(1)` / `write(2)` 不再 EBADF
- `ssh -p 7900 "echo HELLO"` 输出到达客户端

## P2：锚号独立号段 + socket 移出宿主 poll

**优先级：中。** P1 之后剩下的是「按数字猜类型」的隐患。

要做：

- `win_socket.c`：新增 `ANCHOR_BASE`；`wsock_fds[]` / `wsock_fds_gen[]` 改按锚号索引；新增 `wsock_nul[]` 保存各自的 NUL fd
- `win_socket_alloc_anchor` 对外返回锚号，NUL fd 只在内部保存；`win_socket_free_anchor` / `wsock_anchor_release` 负责关它
- `wsock_fd_get` / `win_socket_is_fd` 改为区间 + 表项判定
- `sys_poll`：socket 槽归入自有就绪，用 `WSAPoll` / `select` 对 SOCKET 批量判定；只有 CRT 号进宿主 `poll()`
- `userland_fd_close`：只按 `backend` 分派，删除 `anchor_gen` 猜测分支

依赖：P1（避免 fork 与号段两处同时改动，回归难以定位）。

验收：

- 会话正常输出；`fstat` 对 socket 仍报 `S_IFSOCK`
- 误把锚号传给 CRT API 的场景立即失败，不再静默写进 NUL
- `/proc/<pid>/fd/<n>` 仍答路径

## P3：删除剩余防御机制

**优先级：低。** P1、P2 稳定后执行。

要做：

- 删除 `host_fd_refs` / `host_fd_acquire` / `host_fd_release` / `host_fd_counted` 与配套自旋锁
- `userland_fd_host` 删除 stale 判定，退化为查表
- `anchor_gen` 退化为诊断字段或整体删除

依赖：P1、P2。

验收：全量回归通过；`rg "host_fd_ref|anchor_gen"` 无残留调用点。

## 全局验收

- `pwsh ./tools/session_e2e.ps1` 通过——**当前离线环境跑不起来，需换环境**，见 `../issues/INDEX.md` 的环境条目
- 无新增 `rvvm_warn`，尤其不得出现 `anchor N given back while M slot(s) still named it`

## 风险

- `sys_poll` 换通道后若出现忙等，`slice` 的计算（`rvvm_user.c` ~10675）需要重新标定
- 把锚号当 CRT fd 传出去的调用点**未穷举**；P2 开工前先用 `rg "win_socket_|wsock_fd_get"` 全量过一遍
- 端到端验证依赖能正常启动 core 的桌面环境，当前离线环境无法完成
