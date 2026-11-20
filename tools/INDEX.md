# tools 索引

`tools/` 是受 Git 管理的脚本目录（构建入口 `build_virtpass.ps1` 在仓库根）。
所有脚本假定从仓库根执行、产物在 `release.windows.x86_64/`。

## 端到端驱动（跑真的 guest，按 run 隔离）

| 脚本 | 内容 | 备注 |
|---|---|---|
| `ash_e2e.ps1` | 一个 `ash --serve`（= 一个 run）+ 多个 `ash` 客户端 | 会话/控制台基础链路 |
| `session_e2e.ps1` | `vpsessiond` 作 run root + 多个客户端；含日志落盘断言 | `-Multi` 第二个 session 的共享文件检查仍 FAIL，见 `../issues/INDEX.md` |
| `ash_drive.ps1` | 驱动 ash core 的辅助脚本 | 被 e2e 复用 |
| `ash_sock.ps1` | 指向某个端口上 core 的宿主端 | 辅助 |
| `jobctl_e2e.ps1` | 作业控制时序：`-Scenario {sigint,sigtstp,fg-resume,fg-again,bg,killpg,killpg-cont,wait-bg,wait-int}` / `all`；`-NoJit` 走解释器路径 | 管道复现不了「命令运行中按键」，必须用它 |
| `procfs_e2e.ps1` | `ps`/`top`/`/proc` 端到端（22 checks） | |
| `mount_e2e.ps1` | `mount(2)`/`umount2(2)`/`/proc/mounts` | |
| `memfs_e2e.ps1` | tmpfs 挂载真的提供字节 | |
| `memfs_root_e2e.ps1` | 无 rootfs 的 run：bundle 落进 run 自己的内存 fs | |
| `openssh_e2e.ps1` | guest sshd + guest ssh 客户端，一个 run | socketpair/fork/close 最重的用户 |
| `sshd_crash_e2e.ps1` | core 在 sshd privsep 会话中自尽的回归 | |
| `android_console.ps1` | 从 PC 驱动 Android 宿主里 guest 的控制台（loopback 字节管道，adbd 分帧） | 需 adb 与已启动的 APK |

## 宿主侧自测（不开 guest）

| 脚本 | 内容 |
|---|---|
| `memfs_selftest.ps1` | `rvvm_memfs` 自身检查（纯数据结构） |
| `hostfs_selftest.ps1` | `rvvm_hostfs` 对真实宿主目录的检查 |
| `fs_selftest.ps1` | 同一组操作分别问两种 fs（`rvvm_fs` 抽象的一致性） |
| `pgt_smoke.ps1` | 覆盖经翻译指针写入 guest RAM 的 syscall 路径 |
| `pgt_mem_probe.ps1` | 采样 guest run 持有 N 个长生命周期子进程时的内存 |
| `pgt_mem_probe_selftest.ps1` | 证明上面那个探针自己的检查会触发 |

## ABI / 构建门禁

| 脚本 | 内容 |
|---|---|
| `axis_a_by_abi.ps1` | 核对 syscall 出参分类与 dispatch 内那张表一致 |
| `axis_a_by_abi_selftest.ps1` | 变异测试：证明该门禁**能**失败（从没失败过的门禁没有信息量） |
| `axis_android_sources.ps1` | NDK 构建与 make 构建编译的是否同一批 RVVM 源码 |
| `axis_android_sources_selftest.ps1` | 上述门禁的自测 |

## 管理面（userland as service 预览）

| 脚本 | 内容 |
|---|---|
| `supervisor/supervisor.py` | userland supervisor daemon：`rvvm_ash --serve` 实例的起停/清单/日志/审计，SQLite 状态机 + reconcile；含 ws 终端桥（stdlib RFC6455 → `ssh -tt`）与实例密钥管理；**持久卷**（`<data-root>/volumes/<label>-<uuid>` → `--data-dir` → guest `/persist`，`<dir>.lock` 单写者，`--data-root` 默认 `%LOCALAPPDATA%\rvvm-supervisor`）。**仅标准库**。API/WS 始终要求 token（无则自动生成到 `runtime/supervisor/token`），API 不回绝对宿主路径。设计与用法见 `../docs/design-userland-supervisor.md` §4.3 |
| `supervisor/us.py` | 上面的 CLI：`us list/start --data NAME/stop/rm --purge/logs/exec/volumes/health`（thin HTTP client），token 从 flag/env/token 文件解析 |
| `supervisor/ws_smoke.py` | ws 终端桥冒烟：`python ws_smoke.py <实例端口> [token]`，握手 → 发命令 → 收 pty 回显 → `WS-BRIDGE-OK`；无 token 时退出 2 |
| `supervisor/selftest.py` | supervisor 安全自测（独立测试 release + 临时 daemon/DB/端口）：无 token 401、有 token 200、API 无绝对宿主路径/endpoint、默认拒绝 guest hostfs；约 10s |
| `supervisor/vm.py` | `vm`：像 `bash` 一样用实例——`vm` 进交互 shell，`vm -c "cmd"` 一次性执行（退出码透传），`vm --list` / `--port N` / `--start`；ssh 直连实例端点，token/key 与 `us.py` 同一套解析。`vm.cmd` 是挂 PATH 的包装（本机已装到 `C:\Users\linswin\.local\bin\vm.cmd`） |

Agent 侧的完整用法（起实例、`us` 各子命令、ssh 的两种拼法、交互式 pty 测试配方、已知坑）
在仓库里有一份：`skills/userland-vm/SKILL.md`；本机同时安装到
`c:/Users/linswin/.agents/skills/userland-vm/SKILL.md` 供 harness 直接加载，两份保持一致。

## 打包 / 生成 / 安装

| 脚本 | 内容 |
|---|---|
| `pack_apps.py` | 打 `apps.tar.gz`（`apps/<id>.vapp`） |
| `pack_system.py` | 打 system 层（源 basename → guest 路径） |
| `install_file.py` | `install -m MODE SRC DST` 的跨平台替代 |
| `gen_gl_abi.py` | 生成 `include/virtpass/vp_gl.h`、`src/virtpass/vp_gl_stub.c` 与 win32 dispatch 表 |
| `gen_stub_notimpl.py` | 生成 SDK 的「未实现」桩源码 |
| `audit_gl_ptr.py` | 审计 GL 指针分类 |

## 约束与坑

- **`*_e2e.ps1` 不能并行**：脚本按进程名 `Stop-Process -Force`，并发会互相误杀（曾把 `test_std` 杀成 `EXIT=-1`）。
- guest sample 的 cwd 必须是 `release.*` 目录（相对路径按进程 cwd 解析）。
- 断言抓脚本自己的 `PASS/FAIL`/`checks` 行；`jobctl_e2e` 按 scenario 输出，没有统一 `PASS` 字样。
- `jobctl_e2e` 偶发 `SEND-FAILED ... 管道正在被关闭`：guest 已退出后脚本仍写 stdin 的时序噪音，判据看 guest 输出。
