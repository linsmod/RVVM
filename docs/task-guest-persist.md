# 任务：guest 持久目录（`/persist`）与 supervisor 数据卷

> 执行层文档。2026-11-20，分支 `android-abi-host`，起点 commit `dbe39a2`。
> 设计结论落在 `design-userland-supervisor.md` §4.3，现状落在 `arch-userland.md` §2。

## 1. 要解决的问题

core 会被定期销毁：guest 的 `/` 每次启动由 bundle 重建，`/tmp` 是内存文件系统。于是
guest 的数据库没有落脚处——重启即空。

结论（用户定）：**每实例一个宿主目录**，core 用 `--data-dir` 挂到 `/persist`；
目录名 **`<label>-<uuid8>`**，因为 label 在实例停止后即可复用，只按 label 命名会让
"同名重建"拿到上一个实例的库。

## 2. 改动清单

| 文件 | 改动 |
|---|---|
| `src/virtpass/win32-host/ash_main.c` | 解析 `--data-dir <dir>` / `--data-dir=`，用法文本，下传 `ash_serve()` |
| `src/virtpass/win32-host/ash_core.h` | `ash_serve()` 加 `data_dir` 形参并说明语义 |
| `src/virtpass/win32-host/ash_client.c` | `ash_data_dir_override`；`win32_host_set_data_dir()`；与 `--run-dir` 同一时机生效 |
| `src/virtpass/win32-host/win32_cmdpost_bridge.{c,h}` | `g_data_dir` + setter；`win32_host_start_guest()` 里 `rvvm_user_mount_hostfs(machine, "/persist", dir)`；**挂不上就 `rvvm_user_free()` 并拒绝启动**；rootfs 里补一个空 `persist` 目录（挂表行不是目录项，否则 `ls /` 看不到） |
| `tools/supervisor/supervisor.py` | `instances.data` 列 + `ALTER TABLE` 迁移；`default_data_root()`；`new_data_dir()`（label-uuid）、`claim_data_dir()`/`release_data_dir()`（`<dir>.lock`）、`volumes()`；`start(..., data=)`；`public_instance` 把 `data` 变 basename；`GET /api/volumes`；`DELETE ?purge=1`；`--data-root`；**顺带修好 `POST /api/instances/{port}/start`**（原来 `row["memfs"]` 列不存在 + `INSERT` 主键冲突，重启必失败） |
| `tools/supervisor/us.py` | `us start --data NAME`、`us volumes`、`us rm --purge`，`us list` 增加 DATA 列 |
| `tools/supervisor/selftest.py` | 测试 daemon 传 `--data-root <tmp>`（测试数据不落在用户数据目录）；新增 8 项卷用例 |

## 3. 验证（本机实测）

手测（`release.windows.x86_64`，独立 `--run-dir`/`--data-dir`）：

1. `rvvm_ash --serve --port 7924 --rootfs <tmp> --run-dir <tmp> --data-dir <tmp>` →
   guest `mount | grep persist` 得 `hostfs on /persist type hostfs (rw,relatime)`；写 `/persist/x`
   落到宿主目录。
2. 杀 core、换端口重启、同一 `--data-dir` → `cat /persist/x` 得 `hello`。
3. 全新 rootfs 且**不带** `--data-dir` → `ls -d /persist` = `No such file or directory`（向后兼容）。

supervisor（独立测试 daemon：7913、临时 db/data-root）：

4. `us start --data web web` → `list` 显示 `web-25aba26a`；`us volumes` 显示锁与占用。
5. `us stop 7910` → 卷变 idle；再 `us start --data web web`（同名 label）→ 新端口、**新目录**
   `web-0e78980d`，guest `/persist` 为空（不继承旧数据）。
6. `POST /api/instances/7910/start` 重启旧实例 → 复用 `web-25aba26a`，`cat /persist/x` 仍是 `hello`。
7. `us rm 7912` 保留目录并打印名字；`us rm 7910 --purge` 删除目录，`volumes` 只剩保留的那个。

`python tools/supervisor/selftest.py --release release.windows.x86_64`：**PASS，0 failure**，
新增用例全绿（`/persist` 挂载、可写、`data` 是 label-uuid 名字、`volumes` 报告 locked、
`rm` 保留目录、`rm --purge` 删除目录）。

## 4. 过程中踩到的两个坑

1. **selftest 的 `instance 7940 sees its own ready marker` 失败**，与本次改动无关：
   `release.windows.x86_64/bundle/system.tar.gz` 是 2026-11-19 00:05 打的，里面的 `/sbin/vpshhd`
   **不含 ready 标记代码**（字符串 `vpshhd: ready` 不存在），于是 core 一直走 TCP 兜底、
   guest 里没有 marker。重打 `system.tar.gz`（`mingw32-make pack-system`）后 PASS。
   这是 `issues/INDEX.md` 已登记的 `[build] [bundle]` 过期项，本次顺手验证了修法。
2. `POST /api/instances/{port}/start` 原本是坏的（见 §2 表），做"重启复用目录"时才发现。

## 5. 恢复点

| 步骤 | 状态 |
|---|---|
| core `--data-dir` + 手测 | 完成 |
| supervisor 卷（命名/锁/生命周期/API/CLI） | 完成 |
| selftest 卷用例 + 重打 bundle | 完成 |
| 文档（design §4.3、arch-userland §2、本文件、test-timing、tools/INDEX） | 完成 |

## 6. 遗留（登记在 `issues/` 或 `todo-`）

- 硬配额：不做，只能 `us volumes` 报告占用；`df` 仍是合成的 64 MiB 预算。
- `/persist` 上的 `chmod` 不持久（hostfs 语义，`arch-userland.md` §2）。
- WAL 仍不可用：`mmap(MAP_SHARED)` 返回 `ENODEV`，见 `task-mmap-shared-wal.md`（P1 页表 / P2 共享页）。
- 跨 core 共用一个库仍不支持（SQLite 记录锁不跨 core）。
- 配额巡检、`us prune`（清理无 DB 行的卷）尚未实现。
