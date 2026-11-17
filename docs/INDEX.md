# docs 索引

前缀约定见 `../AGENTS.md`（`arch-` 现状 / `design-` 设计 / `fix-` 修复史 / `todo-` 计划 / `task-` 单次任务）。

## docs/ 下的文档

| 文件 | 类型 | 内容 |
|---|---|---|
| `arch-userland.md` | 现状 | userland 模拟 + VirtPass 宿主：代码地图、命名空间/挂表、fd 与会话、启动方式、日志不变量、构建与验证入口。最后核对 2026-11-17 |
| `INDEX.md` | 索引 | 本文件 |

## 仓库根目录的文档（历史位置，未迁入 docs/）

`.gitignore` 历史上只白名单了源码目录，这些文档一直放在根目录；保持原位以免打散引用。

| 文件 | 类型 | 内容 |
|---|---|---|
| `../README.md` | 上游 | RVVM 上游说明（RISC-V VM/emulator、librvvm、构建与运行、TODO） |
| `../DEV.md` | 现状 | 构建脚本入口、Makefile 关系、VirtPass 层次、端到端测试与非交互驱动方式、`/proc` 支持面、vpsessiond 日志、日志 sink/ring |
| `../dev_debug_notes.md` | 经验 | 以 procfs/`ps` 实现为例的开发调试经验：大 C 单文件改动、Windows 构建/运行坑、合成 VFS 的 fd 语义、内存泄漏排查方法、命令速查 |
| `../handover.md` | 设计+落地记录 | userland 命名空间 / rootfs / app 隔离：Step 1-6 落地状态与迁移面。**早于挂表行模型**，现状以 `arch-userland.md` 与 `src/core/rvvm_user.h` 为准 |
| `../ash_wsl_stateful_design.md` | 设计 | WSL 形态的有状态会话/命名空间设计（体量大，属方案档案） |
| `../handover_WSL_GAP.md` | 设计差距 | 上述设计相对现状的缺口清单 |
| `../遗留问题.txt` | 原始记录 | 未归档的问题笔记，条目已登记到 `../issues/INDEX.md` |

## 待补（按需创建）

- `arch-virtpass-abis.md`：VirtPass 的 NDK/EGL/GLES ABI 面与 stub 生成流程（`tools/gen_gl_abi.py`、`axis_a_by_abi.ps1`）。
- `fix-*.md`：按需追加修复条目。
