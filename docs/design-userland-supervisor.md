# 设计：userland supervisor（实例管理服务）

> **不是现状，尚未实现。** 冻结于 2026-11-18。
> 这是 userland-as-service 的第一层：先把「一台机器上的 N 个 userland 实例」管起来，
> web 后台与云编排是它之后的客户端，不在本文范围。

## 1. 要解决的问题

`rvvm_ash --serve --port N` 已经是一个可实例化的 core（端口即实例 ID），
但围绕它的编排散落在人手与 e2e 脚本里：谁起过、占哪个端口、日志在哪、
崩了要不要收、rootfs 是不是干净的——没有一处集中视图。

## 2. 决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 架构 | 常驻 supervisor daemon + SQLite + CLI | daemon 单例持有端口表与状态机；DB 记期望与元数据；CLI 是 API 的第一个消费者，web 后台之后只是第三个客户端 |
| 语言 | Python 3.9+，**仅标准库**（http.server / sqlite3 / subprocess / ctypes） | 预览零第三方依赖（装依赖需逐次确认）；API 稳定后 web 阶段再考虑 FastAPI/websocket |
| 放置 | `tools/supervisor/`（受 Git 管理，不进 librvvm 构建） | 管理面是外部编排，不污染宿主源码 |
| 隔离 | **每实例一个 rootfs 目录**：`runtime/roots/<port>-<uuid>`，经 rvvm_ash 新增的 `--rootfs <dir>` 传入 | rootfs 锁原为一树一 core；锁与 mode-store 随目录收放后，同树多实例互不争用（2026-11-18 实测两实例并发 ssh 全通）。`--memfs` 方案搁置：guest 启动即退（退出码 1），登记 `issues/INDEX.md` |
| 命令执行 | core 自己的 ssh 端点（`vpshhd → tinysshnoneauthd`）：daemon 内 `ssh -p <port> 127.0.0.1 "<cmd>"` | vpssh 链已验证（fd-ownership 全程用它回归）；vp 客户端从管理面退役 |
| 鉴权 | 静态 token（`SUPERVISOR_TOKEN` 环境变量），默认只绑 127.0.0.1 | 预览底线；guest 的 ssh 面是 tinysshnoneauthd（设计即无鉴权），绝不可因 supervisor 暴露到公网 |

## 3. 事实源：DB 不做 liveness

磁盘上已有注册表：core 启动后写 `runtime/cores/<port>.core`（pid + 程序），
退出自删，崩溃残留靠 pid 活性区分回收（`ash_client.c` 的既定语义）。

- **DB 记**：期望状态（desired）、实例元数据（owner/label/rootfs/日志路径）、
  端口分配、事件审计
- **真相在磁盘 + pid**：`.core` 存在且 pid 活 ⇔ running

### 3.1 reconcile（daemon 启动时 + 每次 list 时）

| DB | 磁盘 | 结论 |
|---|---|---|
| desired=running | 有 `.core` 且 pid 活 | running |
| desired=running | 无 `.core` 或 pid 死 | **crashed**：回收端口，保留日志与审计 |
| desired=stopped | 有 `.core` 且 pid 活 | **orphan**：登记上报，预览版不自动杀 |
| 无记录 | 有 `.core` | **orphan**：同上（daemon 重启不丢实例） |
| desired=stopped | 无 | stopped（纯历史记录） |

### 3.2 状态机

```
desired:  running | stopped
actual:   starting ──(10s 内出现 .core 且 pid 活)──> running
          starting ──(超时)──────────────────────> failed
          running  ──(core 正常退出)─────────────> stopped
          running  ──(core 异常消失)─────────────> crashed（reconcile 时发现）
          stopped  ──(start)─────────────────────> starting
```

启动宽限 10s：对齐 `ASH_STARTING_GRACE_MS`——`.core` 写在 guest 启动之后、
bind 之前，把「还在启动」误报成「僵死」会让运维去杀健康 core。

## 4. 部署面（对 RVVM 既有原语的映射）

| 需求 | 原语 | 出处 |
|---|---|---|
| 实例化 | `rvvm_ash --serve --port N --rootfs <dir> --run-dir <run>` | `ash_main.c`（端口即 ID）；`--rootfs`/`--run-dir` 为本次新增（rootfs 锁/mode-store 与 `/run/vpsessiond` 随实例目录收放） |
| 实例清单 | `runtime/cores/*.core` | `ash_client.c`（自注册/自删/残回收） |
| 日志 | `RVVM_LOG_FILE=<supervisor>/<port>.log`、`RVVM_LOG_RING_DUMP` 同目录 | utils.h log ring |
| 终端/命令 | `ssh -p N 127.0.0.1 "<cmd>"`（vpssh 链：vpshhd → tinysshnoneauthd） | 客户端任意 ssh |
| 探活 | pid 活性 + tcp connect 端口 | |

宿主改动（最小面）：`ash_main.c` 解析 `--rootfs`/`--run-dir`、`ash_client.c` 锁与
mode-store 按目录收放、`win32_cmdpost_bridge.c` 按 run 目录 mount `/run/vpsessiond`。
默认路径（无 `--rootfs`/`--run-dir`）行为不变，`runtime/ash-core.lock` 历史名保留。
2026-11-19 起 supervisor 每实例传独立 `--run-dir`，ready/auth.pub/日志不再共享。

daemon 启动子进程时 cwd 设为 release 目录（guest 相对路径按进程 cwd 解析，
`tools/INDEX.md` 既定约束），stderr 重定向到实例日志文件。

### 4.1 release 树的选取（2026-11-19 收紧）

一个 release 树要被 daemon 接受，必须**同时**具备 `rvvm_ash`（或 `.exe`）和
`bundle/{rootfs,system,apps}.tar.gz`——后者是 core 启动时解包的东西。只有 `bin`
的构建树（`make BUILDDIR=... bin` 出来的试验目录）不再能被自动发现：

- `--release` 显式指定时不满足就立刻退出，并列出缺什么；
- 自动发现时逐个检查，不合格的跳过并在日志里报出原因；
- 多个合格候选（例如 `release.windows.*` 与 `release.test.*`）取 **`rvvm_ash` 最新的**
  那个，并打印全部候选与最终选择。

此前是「按名字排序取第一个有 `rvvm_ash` 的目录」，于是 `release.cc2.windows.x86_64`
这种只编了 `bin` 的试验目录会被选中，实例起来 25 秒后变 `failed`，而原因（缺 bundle）
在界面上完全不可见。**每个 release 树各自持有 `runtime/supervisor/supervisor.db`**，
所以选错树还等于换了一套实例状态——这一条也写进了启动日志。

daemon 单例：绑定前先 `GET /api/health` 探测目标端口，已有 daemon 应答就退出并报出
对方的 version/release。`SO_REUSEADDR` 在 Windows 上允许第二个进程绑正在 LISTEN 的
端口，两个 daemon 各持一份 DB 会表现为 `stop` 时好时坏、两次 `list` 结果不同。

### 4.2 就绪汇报（2026-11-19）

`running` 曾经等于「core 注册表里有这一行且 pid 活着」——那是 host 侧 core 起来，
guest 还没 boot，第一次 ssh 仍可能被拒。现在由**真正监听的那个进程**汇报：

| 环节 | 行号/文件 | 行为 |
|---|---|---|
| guest | `src/virtpass/guest-samples/vpshhd.c` | `listen()` 成功后写 `/run/vpsessiond/<port>.ready`（tmp + rename 原子落盘，内容 `pid=` / `port=`），监听器退出时删除 |
| 宿主可见性 | 每实例一个目录 | `/run/vpsessiond` 是 host 的 `<release>/runtime/run/<port>`（该实例的 auth.pub、ready、vpshhd.log 都走它），daemon 直接看到文件出现；实例之间互不可见、不可写 |
| daemon 启动 | `Supervisor.wait_ready()` | spawn 前先删掉同端口的旧标记；`start()` 等到标记出现才置 `running`（上限仍是 `STARTING_GRACE_S`） |
| 兼容旧 guest | `READY_FALLBACK_S = 8` | live core 8 秒内没有标记、但 TCP 能连上 → 按就绪处理并**打日志**说明用了 TCP 兜底 |
| 失败区分 | `start()` | core 先死 → `core exited before sshd was ready`；一直没就绪 → `sshd not ready within 25s`，两者都带实例日志路径 |
| 观测 | `GET /api/instances` | 新增 `ssh_ready`（`actual == running` 且标记存在） |

实测（2026-11-19，本机）：`us start` 1.35 s 返回，返回后**立即** `us exec` 成功；
旧行为是 1.8 s 返回但 2.3 s 才连得上。杀掉 core 后 1 秒内即判 `failed` 并给出正确 note。

## 5. API 面（v1 + v2 终端，CLI 与 web 共用）

| 方法 | 路径 | 语义 |
|---|---|---|
| GET | `/api/instances` | 列表（含 actual/desired/pid/端口/label/rootfs basename/启动时间），内嵌 reconcile 结果；不返回绝对宿主路径 |
| POST | `/api/instances` | `{label?, port?}` → 分配端口、建 `roots/<port>-<uuid>` 目录并启动，同步等宽限内结果 |
| POST | `/api/instances/{port}/start` | stopped/crashed → starting |
| POST | `/api/instances/{port}/stop` | 终止 core（按 `.core` 的 pid）；强杀残留的注册文件由 daemon 回收 |
| DELETE | `/api/instances/{port}` | 确保停止后删除记录并清掉实例的 rootfs 目录（保留日志与审计） |
| GET | `/api/instances/{port}/log` | 实例日志（支持 `?tail=N`） |
| POST | `/api/instances/{port}/exec` | `{cmd}` → `ssh -p N`，同步返回 stdout/stderr/退出码 |
| GET | `/api/health` | daemon 版本、release basename、core 摘要（pid/shell/alive，无 endpoint 路径） |
| GET | `/ws/{port}` | **v2 交互终端**：stdlib 最小 RFC6455 握手后，字节泵桥到 `ssh -tt -i <daemon密钥> -p N` |
| GET | `/`、`/vendor/*` | web UI（单文件页 + xterm.js v6/fit vendor，免构建链） |

鉴权：`Authorization: Bearer <SUPERVISOR_TOKEN>`，**任何来源都要求 token**
（2026-11-19 修订：loopback 不是安全边界，guest 的 socket 就是宿主 socket，
guest 访问 127.0.0.1 与宿主本机无区别）。未显式给 token 时 daemon 自动生成并写入
`<release>/runtime/supervisor/token`（0600），WebSocket 走 `?token=`。

## 6. CLI（`us`）

```
us [--url U] [--token T] [--token-file F] <command>
  us list                     实例表
  us start [--memfs] [label]  起实例（打印端口）
  us stop <port>              停实例
  us rm <port>                删记录
  us logs <port> [-n N]       日志尾随
  us exec <port> <command...> 在实例里跑一条命令
  us health                   daemon 自检
```

CLI 是薄客户端：只调 HTTP API，不碰 DB、不 spawn 进程。

## 7. 明确不做（预览范围外）

账号体系 / 多租户 / 计费 / 容器编排 / 多机调度。
（websocket 终端已于 v2 落地，见 §5；带键鉴权已于 vpshhd 的 auth.pub 模式落地——
每个实例的 run 目录由 daemon 写入自己的 `auth.pub`，无 `auth.pub` 时保持 noneauth，
dev 行为不变。）

## 8. 验收线（2026-11-18 实测全绿）

1. `us start` → 宽限内端口 tcp 可达 ✓
2. `us exec <port> ls /` 返回 guest 输出（ssh 通道）✓
3. `us stop`/`us rm` 后端口释放、`.core` 回收、rootfs 目录清理 ✓
4. **daemon 重启后 reconcile：running/crashed/orphan 三态正确保留，新实例端口不撞** ✓
5. 两个实例并发（各自 `roots/<port>-<uuid>`）互不影响，ssh 各自可达 ✓
6. 外部强杀 core → 下次 list 检出 `crashed` ✓；事件审计链完整可溯 ✓
