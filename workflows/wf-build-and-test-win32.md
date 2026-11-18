# wf-build-and-test-win32

Windows 上构建 VirtPass 宿主 + userland 部分，并用端到端驱动验证。

## 适用条件

- 改动落在 `src/core/rvvm_user*.{c,h}`、`src/core/rvvm_{fs,memfs,hostfs}*`、`src/util/utils.c`、
  `src/virtpass/win32-host/*`、`include/`、`src/virtpass/guest-samples/*`。
- 需要「真的跑起 guest 并观察行为」（不是纯数据结构改动，后者用 `tools/memfs_selftest.ps1` 更快）。

不适用：Android 侧（`-Target apk` + `adb`，见 `../quick_cmds.txt`，注意其 `--assets` 已过期）、
WSL 形态的有状态会话（见 `../ash_wsl_stateful_design.md`）。

## 前置

- PowerShell Core（`pwsh`）。**不要用 heredoc**（`<<`）。
- 工具链由 `build_virtpass.ps1` 自己定位（mingw32-make 等），不需要预先配 PATH。
- 当前工作区必须没有正在运行的 `rvvm_winhost_x86_64.exe` / `rvvm_ash_x86_64.exe`
  —— Windows 上运行中的 exe 被锁，relink 会报 `Permission denied`。

## 命令

```powershell
# 1. 先清掉残留实例（这是重新构建前的固定动作，不是可选项）
Get-Process rvvm_winhost_x86_64,rvvm_ash_x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force

# 2. 构建（唯一入口；规则都在 Makefile + project.mk 里）
pwsh ./build_virtpass.ps1 -Target win32 -Jobs 8
#    -Target all|win32|apk|android|assets|dist|jni|sdk|clean
#    -Clean 只在需要时加；额外 make 参数直接跟在后面（如 USE_RVJIT=0）

# 3. 只编单个目标（省时）
mingw32-make release.windows.x86_64/rvvm_winhost_x86_64.exe -j8

# 4. 跑：guest 只作为 bundle 里的 app 包启动
./release.windows.x86_64/rvvm_winhost_x86_64.exe --app test_cli
printf 'ls /\nexit\n' | ./release.windows.x86_64/rvvm_winhost_x86_64.exe --app test_cli

# 4b. 会话模式：起 core 监听 7900，再用 ssh 连（tinyssh 链路）
#     清 rootfs 与 run 下的日志，否则上一轮的会话状态会干扰观察
Remove-Item -Recurse -Force release.windows.x86_64/runtime/rootfs -ErrorAction SilentlyContinue
Remove-Item release.windows.x86_64/runtime/run/*.log -ErrorAction SilentlyContinue
Start-Process .\release.windows.x86_64\rvvm_ash_x86_64.exe -ArgumentList '--serve' `
  -RedirectStandardError "$env:TEMP\core.err" -WindowStyle Hidden
Start-Sleep 25
ssh -p 7900 -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL 127.0.0.1 "echo HELLO"

# 5. 诊断开关（trace 走 stderr，不是 guest stdout）
$env:RVVM_TRACE='path'          # 或 'fd' | 'sys' | 'all'
$env:RVVM_VERBOSE='1'           # loglevel 提升到 INFO
$env:RVVM_LOG_FILE="$env:TEMP\run.log"
$env:RVVM_LOG_RING_DUMP="$env:TEMP\ring.log"

# 6. 内存看增量（commit=泄漏指标，ws=驻留）
$m = Get-Process rvvm_ash_x86_64
'{0:N1} MiB commit, {1:N1} MiB ws' -f ($m.PrivateMemorySize64/1MB), ($m.WorkingSet64/1MB)
```

guest 是 bundle 里的 app 包（`apps.tar.gz -> apps/<id>.vapp`，入口与参数取自 `meta.json`）；
`--guest` / `--direct` / `RVVM_ASH_SHELL` 这类按路径跑 ELF 的门已不存在。现状见 `../docs/arch-virtpass.md`。

## 非交互驱动

`rvvm_winhost` 是 console 应用，stdin 会泵进 guest 控制台（等 guest 真正启动后才开始读），
guest stdout = 进程 stdout，guest 退出码 = 进程退出码，所以普通场景用管道即可。

**例外**：需要「命令运行中按键」时序的作业控制（`^C`/`^Z`/`jobs`/`fg`）管道复现不了，
用 `tools/jobctl_e2e.ps1 -Scenario {sigint,sigtstp,fg-resume,fg-again,bg,killpg,killpg-cont,wait-bg,wait-int}`。
该驱动是 **output-event driven**：增量读 socket，用 `Expect-Pattern`/`Expect-Newlines`/`Expect-Offset`/`Expect-Quiet`
推进，取代固定 sleep；裸 `sleep`/`wait` 无输出时用 `Expect-Quiet` settle。`-ShowRate` 打印每秒到达字符数直方图，
失败时 dump 换行 offset。旧的固定间隔 `Process.StandardInput` 模式已弃用。

## 验证

按「越靠近改动越小」的顺序串行跑（**这些脚本彼此不能并行**）：

| 改动面 | 驱动 |
|---|---|
| procfs / `ps` / `top` | `pwsh ./tools/procfs_e2e.ps1` |
| 挂表 / `mount(2)` | `pwsh ./tools/mount_e2e.ps1` |
| memfs 字节路径 | `pwsh ./tools/memfs_e2e.ps1`、`tools/memfs_root_e2e.ps1` |
| 会话 / 控制台 | `pwsh ./tools/session_e2e.ps1`、`tools/ash_e2e.ps1`（**2026-11-18 当前离线环境 22 项全 FAIL，`git stash` 后基线同样，见 `../issues/INDEX.md`**） |
| 作业控制（信号/时序） | `pwsh ./tools/jobctl_e2e.ps1 -Scenario all`（`-NoJit` 走解释器） |
| socket/fork 重载 | `pwsh ./tools/openssh_e2e.ps1` |
| fs 抽象层 | `pwsh ./tools/fs_selftest.ps1`；纯结构用 `memfs_selftest.ps1` |
| 翻译指针写 guest RAM | `pwsh ./tools/pgt_smoke.ps1` |

全览与各脚本用途见 `../tools/INDEX.md`。

## 坑（都是实际踩过的）

- relink 报 `Permission denied` ⇒ 有 exe 在跑，先 `--shutdown` / `Stop-Process`。
- `runtime/ash-core.lock` 被残留 core 持有 ⇒ guest sample 退出码 `-1`、无输出。先想环境（锁、端口、残留进程），再想代码。
- guest 相对路径相对**进程 cwd**，不是 exe 目录 ⇒ 用工具的 `workdir` / 绝对路径。
- 脚本按进程名 `Stop-Process` ⇒ 不能并发，也不要和手工跑的 ash 用例混在一起。
- `-c` 客户端会连到**已存在**的 core，环境变量传不进去 ⇒ 要 trace 就显式起一个带 trace 的 core 并把 stderr 重定向到文件。
- trace 在 stderr，不在 guest stdout（guest 自己的输出才是 stdout）。
- host 侧只准用 `rvvm_log()`/`RVVM_LOGx()`，写 stdout 会污染 e2e 正在读的 transcript。
- `--serve` 起 core 前不清理 `runtime/rootfs` ⇒ 读到上一轮残留的会话状态；清理 `runtime/run/*.log` 才能看新一轮日志。
- e2e 大面积 FAIL 且 console / daemon log 全空 ⇒ 先怀疑 core 根本没起来。用 `git stash` 跑一次基线对照，能区分「自己的改动」与「环境问题」，不要直接改代码。

## 退出条件

- 受影响的驱动全绿，且失败不在「已知问题」清单里（`../issues/INDEX.md`）。
- 没有新增的残留进程 / 锁文件 / runtime 目录垃圾。
- 清理一次性脚本与调试输出，登记新工具到 `../tools/INDEX.md`。

## 执行经验与改进意见

- 2026-11-17 bootstrap：本文命令来自 `build_virtpass.ps1` 的 `.SYNOPSIS` 与原根目录 `DEV.md`（已拆入
  `../docs/arch-virtpass.md` 与本文件）、`../docs/hist-lessons.md`；
  **本轮未实际执行构建或 e2e**，首次执行后请把实际耗时与失败点补进来。
- 2026-11-18 实测（fd 所有权相关改动）：`mingw32-make bin`、`mingw32-make system-assets pack-system` 可用；
  `--serve` + `ssh -p 7900` 能复现会话链路；`pwsh ./tools/session_e2e.ps1` 在本环境 22 项全 FAIL，
  与 `git stash` 后的基线完全一致（core 未起来）——**该脚本在本环境不具备回归能力**，已登记 `../issues/INDEX.md`。
  耗时未记录：端到端未能跑通。
- 待补：把这些驱动收进一个 `test-all` 目标（AGENTS「测试」条款要求），并记录到 `../docs/test-timing.md`。
- 会话链路的 fd 层定位方法见 `wf-debug-userland-fd.md`，不要在本文件里重复。
