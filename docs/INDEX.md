# docs 索引

前缀约定见 `../AGENTS.md`（`arch-` 现状 / `design-` 设计 / `fix-` 修复史 / `todo-` 计划 / `task-` 单次任务）。
`hist-lessons.md` 是 AGENTS「问题跟踪与处理」条款点名的固定文件名，专门放可复用教训。

## docs/ 下的文档

| 文件 | 类型 | 内容 |
|---|---|---|
| `arch-userland.md` | 现状 | guest 侧：代码地图、命名空间与挂表行、`/proc` 支持面与约束、合成 fd/进程/会话。最后核对 2026-11-17 |
| `arch-userland-fd.md` | 现状 | fd 映射面：guest 号 / 宿主号 / 锚号三层编号，槽字段，fork/close/execve 各自对宿主句柄做什么，读写信道路径，vpssh 时序案例，以及共享 CRT 表的已知约束。最后核对 2026-11-18 |
| `design-fd-ownership.md` | 设计 | **不是现状**：宿主号按 address space 私有的所有权模型——号私有、对象共享、不变量、改造前后对比、两阶段落地顺序。冻结于 2026-11-18 |
| `todo-fd-ownership.md` | 待办 | P1 fork 私有化 / P2 锚号独立 + poll 归类 / P3 删除剩余防御机制，含优先级、依赖与验收标准 |
| `task-fd-ownership.md` | 执行 | fd 所有权改造的恢复点：起点状态、P1 精确改动点、验证命令、进度勾选。分支 `fd-ownership` |
| `design-userland-supervisor.md` | 设计 | **不是现状**：userland supervisor（实例管理 daemon + SQLite + CLI）的架构、事实源与 reconcile、API 面、验收线。冻结于 2026-11-18 |
| `todo-userland-supervisor.md` | 待办 | S1 daemon / S2 CLI（已完成）/ S3 web 后台，含风险与全局验收 |
| `task-userland-supervisor.md` | 执行 | supervisor 预览的执行记录：交付物、验收结果、过程坑与遗留 |
| `task-host-info-exposure.md` | 执行 | 宿主信息暴露审计与 8 次提交的批量修复：P0 hostfs/supervisor 鉴权、P1 元数据/构建路径，含恢复点与测试配方 |
| `fix-host-info-exposure.md` | 修复史 | 上述修复的现象/根因/修法表/验证/遗留（C1–C8 commit） |
| `fix-fcntl-record-locks.md` | 修复史 | SQLite unix VFS 的 `F_GETLK/F_SETLK/F_SETLKW`：emulator 锁表、fork/close/byte-range、`RVVM_TRACE=fd` 与 direct `rvvm_user` 探针 |
| `test-timing.md` | 测试 | 关键测试的实测耗时（supervisor selftest 33.5s；fs/hostfs selftest PASS） |
| `arch-virtpass.md` | 现状 | 宿主侧：VirtPass 抽象层与 ABI 跟随规则、代码位置、bundle `--app` 启动面、console attach、vpsessiond 日志、日志 sink + ring。最后核对 2026-11-17（源自原根目录 `DEV.md`） |
| `hist-lessons.md` | 教训 | 大 C 单文件改动、Windows 构建/运行、合成 VFS、外部工具的数据质量反噬、调试方法、测试纪律、设计推演、内存泄漏排查方法（源自原根目录 `dev_debug_notes.md`） |
| `fix-userland-hart-registration.md` | 修复史 | userland hart 未入 `machine->harts` → 每次 fork 漏 16 MiB RVJIT 堆：现象 / 根因 / 修法 / 验证 |
| `fix-userland-pty-req.md` | 修复史 | guest ioctl 请求号符号扩展（TIOCGPTN 永不命中）+ pty fd 的 stat 身份不一致（ttyname 判 ENODEV）：现象 / 根因 / 修法 / 验证 |
| `fix-userland-tty-cc.md` | 修复史 | 空闲提示符 ^C 无反应、连按两次会话卡死：TCGETS 不报 `c_cc`（busybox lineedit 靠 VINTR 认键）+ 信号 handler 的 longjmp 让 `sig_inflight` 永久占住（poll 永远 EINTR） |
| `fix-userland-sendfile-tmpfs.md` | 修复史 | `cp`/`cat` 到 `/tmp` 静默丢开头 32K：sendfile 不认 memfs fd，消耗一个 chunk 后失败、调用方从错位继续 |
| `fix-userland-kill-family.md` | 修复史 | `kill(pid)` 只查自己的注册表 → 子进程杀不了父/祖先（`timeout` 看门狗失效）；reparent 后家族根丢失 |
| `fix-userland-orphan-lifetime.md` | 修复史 | 孤儿进程从 `/proc` 消失且 `kill` 不可达（父 ctx 销毁时唯一记录被释放）；被杀孤儿永久 `Z`；异常 trap 只结束线程 → 父 `wait4` 永久阻塞、回溯无上限刷日志 |
| `INDEX.md` | 索引 | 本文件 |

## 仓库根目录的文档（历史位置，未迁入 docs/）

`.gitignore` 历史上只白名单了源码目录，这些文档一直放在根目录，且都**未被 git 跟踪**；保持原位以免打散引用。

| 文件 | 类型 | 内容 |
|---|---|---|
| `../README.md` | 上游 | RVVM 上游说明（RISC-V VM/emulator、librvvm、构建与运行、TODO） |
| `../handover.md` | 设计+落地记录 | userland 命名空间 / rootfs / app 隔离：Step 1-6 落地状态与迁移面。**早于挂表行模型**，现状以 `arch-userland.md` 与 `src/core/rvvm_user.h` 为准 |
| `../ash_wsl_stateful_design.md` | 设计 | WSL 形态的有状态会话/命名空间设计（体量大，属方案档案） |
| `../handover_WSL_GAP.md` | 设计差距 | 上述设计相对现状的缺口清单 |
| `../遗留问题.txt` | 原始记录 | 未归档的问题笔记，条目已登记到 `../issues/INDEX.md` |

## 已拆分（原根目录文件，内容已迁走）

| 原文件 | 去向 |
|---|---|
| `DEV.md` | 现状部分 → `arch-virtpass.md`；`/proc` 支持面与合成 fd 约束 → `arch-userland.md`；构建/运行/非交互驱动 → `../workflows/wf-build-and-test-win32.md`；已知缺陷 → `../issues/INDEX.md`；修复史 → `fix-*.md` |
| `dev_debug_notes.md` | 教训与方法 → `hist-lessons.md`；RVJIT 堆泄漏 → `fix-userland-hart-registration.md`；命令速查 → `../workflows/wf-build-and-test-win32.md` |

## 待补（按需创建）

- `arch-virtpass-abis.md`：VirtPass 的 NDK/EGL/GLES ABI 面与 stub 生成流程（`tools/gen_gl_abi.py`、`axis_a_by_abi.ps1`）。
- `fix-*.md`：按需追加修复条目。
