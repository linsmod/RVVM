# docs 索引

前缀约定见 `../AGENTS.md`（`arch-` 现状 / `design-` 设计 / `fix-` 修复史 / `todo-` 计划 / `task-` 单次任务）。
`hist-lessons.md` 是 AGENTS「问题跟踪与处理」条款点名的固定文件名，专门放可复用教训。

## docs/ 下的文档

| 文件 | 类型 | 内容 |
|---|---|---|
| `arch-userland.md` | 现状 | guest 侧：代码地图、命名空间与挂表行、`/proc` 支持面与约束、合成 fd/进程/会话。最后核对 2026-11-17 |
| `arch-virtpass.md` | 现状 | 宿主侧：VirtPass 抽象层与 ABI 跟随规则、代码位置、bundle `--app` 启动面、console attach、vpsessiond 日志、日志 sink + ring。最后核对 2026-11-17（源自原根目录 `DEV.md`） |
| `hist-lessons.md` | 教训 | 大 C 单文件改动、Windows 构建/运行、合成 VFS、外部工具的数据质量反噬、调试方法、测试纪律、设计推演、内存泄漏排查方法（源自原根目录 `dev_debug_notes.md`） |
| `fix-userland-hart-registration.md` | 修复史 | userland hart 未入 `machine->harts` → 每次 fork 漏 16 MiB RVJIT 堆：现象 / 根因 / 修法 / 验证 |
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
