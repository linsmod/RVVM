# task: userland supervisor（S1 daemon + S2 CLI）

> 执行记录与恢复点。设计见 `design-userland-supervisor.md`，计划见 `todo-userland-supervisor.md`。
> 分支：`android-abi-host`（fd-ownership 已并入）。

## 起点

- 2026-11-18：用户要做 userland-as-service 预览，定案 supervisor + SQLite + CLI，语言 Python 仅标准库
- 协议收敛决议（对外面统一 tinyssh）用户决定**保持现状**，后续必要时再动

## S1/S2 交付（本轮完成）

- `tools/supervisor/supervisor.py`：daemon（SQLite WAL、状态机、reconcile、HTTP API、token、
  事件审计表）
- `tools/supervisor/us.py`：CLI（list/start/stop/rm/logs/exec/health，thin HTTP client）
- `tools/supervisor/web/index.html`：web UI v1（S3 第一刀，无依赖）：daemon 在 `GET /`
  服务单文件静态页——实例表轮询、起停/删除、日志面板、一次性命令框；API 鉴权不变
- 宿主最小改动：rvvm_ash 新增 `--rootfs <dir>`（`ash_main.c` 解析、`ash_client.c` 锁与
  mode-store 随目录收放、`win32_cmdpost_bridge.c` mount dest 覆盖）；默认路径行为不变

## 关键实现事实

- `.core` 注册文件格式：`key=value` 行（`pid=`/`shell=`/`endpoint=tcp:127.0.0.1:<port>`）
- `ash_rootfs_lock` 原为一树一 core、且 `--memfs` 不绕锁 → per-instance 目录方案才有
  `--rootfs` 改动；锁文件落在 rootfs 目录内（默认路径保留 `runtime/ash-core.lock` 历史名）
- exec 走 core 的 ssh 端点：`ssh -p N -o BatchMode=yes -o UserKnownHostsFile=NUL 127.0.0.1 <cmd>`
  （Windows NUL / POSIX /dev/null），vp 从管理面退役
- 启动宽限 25s（注册文件在 guest 起机后即写，rootfs 首次解包约 20s）
- Windows 强杀（TerminateProcess）绕过 core 的清理 handler：stop() 对自己杀的 core 回收残留
  `.core`（与 C 侧 stale 回收同一语义）

## 验收（design §8，全绿）

1. `us start` → running + tcp 可达（rootfs 模式）
2. `us exec 7910 "ls /"` → 真实 guest 输出（ssh 通道）
3. `us rm` → 端口释放、`.core` 回收、`roots/<port>-<uuid>` 目录清理
4. 外部杀 core → list 检出 `crashed`；daemon 重启 reconcile 三态保留、端口不撞
5. 两实例并发（各自 rootfs 目录）ssh 各自可达（`A1`/`B2` 双通）
6. 事件审计链完整（start/running/crashed/orphan/stopped/deleted 全程可溯）

## 过程坑（都已修）

- 换 schema 后旧 DB 文件被旧 daemon 锁着没删净 → `no column named rootfs`；停 daemon 再删
- 强杀后 `.core` 残留 → stop() 误报 500；已修（自回收）
- `start()` 中 Popen 成功、DB insert 失败会漏孤儿 core → 已加护栏（记录失败即终止 core）
- 孤儿实例的 rootfs 目录 DB 无归属，`rm` 清不掉（orphan 行 rootfs=None）→ 预览接受，
  手工清理；改进方向：reconcile 从 core 侧无从得知目录，需 core 注册表记 rootfs（后议）

## 遗留

- `--memfs` guest 退出码 1：`issues/INDEX.md` NEW，阻塞 memfs 隔离形态
- run 目录（`runtime/run`）按树共享，实例日志文件名若不按端口区分会互写——预览未触发，
  上量前要核对（vpshhd/tinysshd 的落盘名）
- S3 web 后台未动工（挂本 API，另立项）

## 恢复方式

1. 构建：`mingw32-make bin CFLAGS=-g`
2. 起 daemon：`Start-Process python -ArgumentList 'tools/supervisor/supervisor.py','--bind','127.0.0.1:7911'`
3. `python tools/supervisor/us.py health` → list/start/exec/rm

## S3 v2：交互终端（2026-11-18 第二轮）

- **鉴权前置已落地**：vpshhd 见 run 目录有 `auth.pub` 即自种 `~/.ssh/authorized_keys`
  并以 `tinysshd`（公钥模式）拉起；daemon 生成 `id_ed25519` 并投递公钥；无 `auth.pub`
  时保持 noneauth（dev 行为不变）。双向验证：带密钥 `auth-ok`、无密钥
  `Permission denied (publickey)`。
- **daemon ws 桥**：stdlib 最小 RFC6455（`GET /ws/<port>` → `ssh -tt -i <key>` 字节泵）；
  前端 `web/vendor/`（xterm.js v6 + fit，免构建链），detail 面板移出表格重渲染范围。
- **顺带修掉两个 RVVM 既有缺陷**（阻塞 pty-req，详见 `fix-userland-pty-req.md`）：
  ioctl 请求号符号扩展（case 29 截断为 uint32）；pty fd 的 fstat 身份（st_dev/st_ino
  与同名路径 stat 一致）。修复前 **vpssh 链的 pty-req 从未成功过**。
- 验收：`ws_smoke.py` `WS-BRIDGE-OK`（pty 回显/行规程/求值）；页面与 vendor 资源全 200；
  `fs_selftest` PASS；openssh/sshd_crash e2e 与 stash 基线逐项一致（既登记的环境问题）。

### 过程坑（本轮新增）

- **双 daemon 脑裂**：两个 `supervisor.py` 同时声称绑 7911（SO_REUSEADDR 语义），请求按
  连接分落到两个进程、两个 DB——`rm` 时好时坏（RemoteDisconnected）。托盘 daemon 还持有
  删库前的旧文件句柄（Windows 上打开着的文件删不掉），一直答旧库。处置：只留托盘登记的
  一个；已登记 issues（daemon 应检测端口已占即退出）。
- **watch 包装吞输出**：多命令拼接的输出只剩第一行，且后续命令实际都执行了（一次起了
  4 个实例）。判据看落盘文件，一条命令一个文件。

## 本机部署（traycmd 托盘，2026-11-18）

daemon 已注册为托盘命令（webui-demo 实例在 daemon 换手后由 reconcile 无缝保留）：

- 托盘「网站」分区「userland supervisor」（proj rvvm，short id `7aec2859`），
  URL 固定 `http://127.0.0.1:7911/`（仅本机可访问，非公网）
- 启动：托盘点菜单项，或 `quickcmd run-job 7aec2859`；停止：托盘「结束任务」或
  `quickcmd kill <实例ID>`；日志：`quickcmd logs 7aec2859`
- daemon 自身跨手是安全的：实例真值在 `runtime/cores/*.core` + pid，新 daemon 起来
  即 reconcile（本次验证：webui-demo 保留为 running）
