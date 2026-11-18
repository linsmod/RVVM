# todo：userland supervisor

> 计划层。目标形态与不变量见 `design-userland-supervisor.md`。
> 依赖：无（纯外部编排，不改 RVVM 代码）。语言 Python 3.9+ 仅标准库。

## S1 daemon 核心

**优先级：高。** **已完成（2026-11-18）**：`tools/supervisor/supervisor.py`，验收全绿
（`design-userland-supervisor.md` §8）；执行记录见 `task-userland-supervisor.md`。
实施中设计修订：隔离方案从 memfs 改为 per-instance rootfs 目录（memfs guest 退出码 1，
登记 issues），exec 走 core 的 ssh 端点（vp 退役）。

要做：

- `tools/supervisor/supervisor.py`：SQLite（WAL）schema（instances/events/meta）、
  状态机（starting/running/stopped/crashed/failed/orphan）、reconcile（启动时 + list 时）、
  端口分配（`meta` 记高水位，避开活 `.core`）、HTTP API（design §5）、token 鉴权
- 子进程管理：`rvvm_ash --serve --port N [--memfs]`，DETACHED + cwd=release，
  `RVVM_LOG_FILE` 指到 `runtime/supervisor/<port>.log`

验收：API 可起停实例，reconcile 三态正确（design §8.4）。

## S2 CLI

**优先级：高。** S1 之后。**已完成（2026-11-18）**：`tools/supervisor/us.py`，
design §8 六条验收线全绿。

要做：

- `tools/supervisor/us.py`：thin HTTP client，`list/start/stop/rm/logs/exec/health`
- 配置来源：`--url/--token` > 环境变量 > 默认 127.0.0.1:7911

验收：CLI 走完 design §8 全部五条验收线。

## S3 web 后台（预览）

**优先级：中。** S1/S2 稳定后进行。**v1 已完成（2026-11-18）**：
`tools/supervisor/web/index.html` 单文件页面（无构建链、零依赖），由 daemon 在 `GET /`
直接服务；实例表 + 起停/删除 + 日志面板（3s 轮询）+ 一次性命令框，复用现有 API，
静态页不经 token（同源），API 仍按 §5 鉴权。

- v2（未动工）：交互终端（xterm.js + websocket 桥到实例 ssh 端点）。
  两条路：加 `websockets`/`aiohttp` 依赖（届时单独确认），或 stdlib 实现最小
  RFC6455 握手。**前置**：tinysshnoneauthd 换带键鉴权（网页终端等于给无鉴权
  shell 包皮，不能先开）。

## 全局验收

- design §8 五条全绿
- `tools/INDEX.md`、`docs/INDEX.md`、`workflows/` 登记齐
- 不新增任何 RVVM 宿主源码改动

## 风险

- Windows 下 pid 活性判定需 ctypes（`os.kill(pid,0)` 在 Windows 语义不同），
  Linux 直接 `os.kill(pid,0)`
- `vp exec-out` 对长命令/交互命令的行为未验证，S2 先只支持一次性命令
- 多实例并发启动对 CRT 号空间的影响：fd-ownership 已私有化，但两实例同机
  属首次真实并发，S1 验收时观察 shared/fd 告警
