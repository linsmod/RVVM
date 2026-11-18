# task: 宿主号私有化（fd 所有权）

> 执行记录与恢复点。计划见 `todo-fd-ownership.md`，目标形态见 `design-fd-ownership.md`。
> 分支：`fd-ownership`（2026-11-18 从 `android-abi-host` 切出）。

## 起点状态

- 已落地：`host_fd_refs` 引用计数 + `anchor_gen` 判定（提交 `fc4ba9d`）
- 未实现：P1 / P2 / P3 全部
- 阻断现象：`ssh -p 7900 "echo HELLO"` 无输出，`sys_write(guest fd=1) = -9 (EBADF)`
- 环境限制：`tools/session_e2e.ps1` 在本机 22 项全 FAIL，`git stash` 后基线一致，**不能作为回归依据**；
  唯一有区分力的信号是 ssh 会话输出

## 恢复方式

1. `git switch fd-ownership`，`git log --oneline -5` 看进度
2. 读本文件「进度」一节的最后一条
3. 构建：`mingw32-make bin system-assets pack-system`
4. 复现：清 `runtime/rootfs` 与 `runtime/run/*.log` → `--serve` → `ssh -p 7900 ... "echo HELLO"`

## P1：fork 私有化

### 改动点

行号是 2026-11-18 起点快照，改动后会漂移，按函数名定位。

| 位置 | 动作 |
|---|---|
| `rvvm_fd_entry_t`（~1432） | 删 `bool shared;` 与注释 |
| `userland_fds_write`（~7523） | 签名删 `bool shared` 参数 |
| 同上（7545 / 7562 / 7574） | 删赋值与 trace 字段 |
| `sys_fcntl` setfl（~7745） | 调用删参 |
| `userland_fd_close`（~8007） | `fd_drop` trace 删 shared 显示 |
| 同上（~8036） | 条件去掉 `!ctx->fds[fd].shared &&` |
| 同上（8062-8068） | 删整个 `else if (shared)` 分支 |
| 同上（~8071） | `fds_write` 调用删参 |
| dup 路径（~8603） | 调用删参 |
| dup2 路径（~8736） | 调用删参 |
| `userland_fd_table_inherit`（8788-8792） | 删「父槽自己是 shared 就跳过」 |
| 同上（8806-8822） | `dup` 失败改为不写槽（子侧该 fd 无效），删除 shared 兜底 |

### 语义变化

`dup` 失败时子进程看不到该 fd，而不是看到一个「别关它」的共享号。与 Linux fork 的 EMFILE 行为一致：继承要么成功，要么没有这个描述符。

### 验证

```powershell
mingw32-make bin system-assets pack-system
Remove-Item -Recurse -Force release.windows.x86_64/runtime/rootfs -ErrorAction SilentlyContinue
Start-Process .\release.windows.x86_64\rvvm_ash_x86_64.exe -ArgumentList '--serve' -RedirectStandardError "$env:TEMP\p1.err" -WindowStyle Hidden
Start-Sleep 25
ssh -p 7900 -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL 127.0.0.1 "echo HELLO"
```

判据：客户端收到 `HELLO`。失败时按 `wf-debug-userland-fd.md` 的四步顺序读 `$env:TEMP\p1.err`。

## P2：锚号独立号段 + socket 移出宿主 poll

见 `todo-fd-ownership.md`。开工前先 `rg "win_socket_|wsock_fd_get"` 全量过一遍，补全调用点清单。

## P3：删除剩余防御机制

见 `todo-fd-ownership.md`。P1、P2 稳定后执行。

## 进度

- [ ] P1 fork 私有化
- [ ] P2 锚号独立 + poll 归类
- [ ] P3 删除 `host_fd_refs` 与 stale 判定

（每步完成后在此追加一行：日期、提交 hash、实际验证结果，包括失败。）
