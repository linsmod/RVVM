# 工作流：guest/宿主边界的安全批量修复

## 适用

guest 能读到宿主路径/pid/磁盘/时间，或 guest 默认拥有宿主能力（hostfs、supervisor 控制、
共享目录写入）需要批量收敛；改动跨 core / win32 host / supervisor，但每一项都能独立验证。

## 前置

- 先完成只读审计：guest 内从 `/`、`/proc`、`/var`、`/run`、二进制 `strings`、API、
  `mount` 五个面取证；不要只信代码注释。
- 把 guest 当不可信用户态程序：宿主能力必须显式授予，默认关闭。
- 当前用户实例/daemon 不碰；测试用独立 release 树、独立 daemon 端口、独立 DB/rootfs。

## 步骤

1. 列 P0（能破坏宿主/控制其他实例）、P1（宿主元数据）、P2（构建指纹）。
2. 每项写清：现象、根因（文件/函数）、修法、验证命令、是否破坏兼容。
3. 按「P0 → P1 → 构建/文档」拆 commit；每 commit 独立可构建/回退。
4. 写 `docs/task-<name>.md`，记录 commit 计划、恢复点、测试配方；中断后按它恢复。
5. 每项实现后：
   - 构建 host：`mingw32-make BUILDDIR=<temp> bin -j4`
   - 独立测试 release：复制 `rvvm_ash` + bundle junction，起 `--bind 127.0.0.1:7912`
   - 跑 `tools/supervisor/selftest.py --release <test tree>`，按需扩展检查。
6. 收尾：`fs_selftest`、`hostfs_selftest`、`test_std`；核对只有用户实例在跑；
   清理测试树/daemon/rootfs；更新 `docs/arch-*`、`docs/fix-*`、`docs/task-*`、
   `tools/INDEX.md`、本索引；时间记进 `docs/test-timing.md`。

## 测试配方（本次实际使用）

```powershell
# 构建 host
mingw32-make BUILDDIR=C:/Users/.../rvvm-fix/build bin -j4
# 测试树：rvvm_ash + bundle junction
New-Item -ItemType Junction -Path <rel>\bundle -Target <release.test>\bundle
# 隔离 daemon（新代码自动生成 token）
python tools/supervisor/supervisor.py --release <rel> --db <tmp>\sup.db --bind 127.0.0.1:7912
# 一键自测
python tools/supervisor/selftest.py --release <rel>
```

## 经验

- 先只读取证，再改；`rootfs.modes` 这类「文件里的路径」和 `mount`/API 这类「能力」要分开处理。
- `rvvm_user` 的 fork 会复制 context：新的 per-run 策略/时间基准要在
  `userland_child_create()` 里继承，否则 `busybox mount` 等子进程看不到。
- 时间 rebase 不只改 `/proc/uptime`：futex `WAIT_BITSET`、`clock_nanosleep TIMER_ABSTIME`
  的绝对 deadline 也要加 base，否则 test_std 的 futex 段会回归。
- `statfs` 的 root 在 host-backed 时 provider 是 `RVVM_MOUNT_HOSTFS`，必须用
  `mount_row_is_root()` 区分，不能只看 provider。
- `getdents64` 的 `d_ino` 和 `stat` 的 `st_ino` 必须用同一个 hash，否则 `ls -i` 与 `stat`
  对不上；只 hash ino，不混 host dev，避免 stat/statx 编码差异。
- 测试 release 树不要命名为 `release.*`（daemon 自动发现会捡到）；用 `-Target win32`
  构建的树要带 bundle。

## 退出

P0/P1 已提交且自测通过；遗留（Linux-only 分支、Android、需重打 bundle、仓库外技能文档）
已登记 `issues/INDEX.md`，本工作流结束。
