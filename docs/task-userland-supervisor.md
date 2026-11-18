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
