# 修复史：SQLite 的 fcntl 记录锁（F_GETLK/F_SETLK/F_SETLKW）

修复日期：2026-11-19
现象来源：客户报告 guest 内 SQLite 起不来，`disk I/O error`；根因指向
`rvvm_user.c` 只处理了 `F_GETFD/SETFD/GETFL/SETFL/DUPFD`，没有 POSIX 记录锁。

## 现象

- guest 里的 SQLite（unix VFS）打开数据库后，在 `fcntl(F_SETLK/F_GETLK)` 上拿不到
  BSD/POSIX 文件锁，返回错误/`disk I/O error`。
- 原实现的 `fcntl64` 对未知命令落到 `fcntl(host_fd, cmd, a2)`：`a2` 是 **guest 地址**，
  不是宿主 `struct flock*`；Windows 上也没有宿主 `fcntl`。即使 Linux 宿主能碰巧工作，
  所有 guest 进程共用一个宿主进程，宿主记录锁会全部归到同一个 owner，
  跨 guest 进程的 SQLite 锁语义也是错的。

## 修法

`src/core/rvvm_user.c` 新增 emulator-owned 锁表：

- 键：`(owner ctx, file dev+ino, byte range, type)`；host 文件用宿主 `fstat` 的
  `(st_dev, st_ino)`，tmpfs 文件用 `rvvm_memfs_dev()` + inode（高点 bit 区分）。
- owner 是 `rvvm_userland_t*`（一个 guest 进程/地址空间），不是宿主 fd；线程共享 owner，
  `fork` 出的子进程是新 owner，不继承父锁。
- 支持 `F_RDLCK`/`F_WRLCK`/`F_UNLCK`、`SEEK_SET/CUR/END`、`l_len=0` 到 EOF、
  负 `l_len`、区间切分/合并、同 owner 不冲突。
- `F_SETLK` 冲突返回 `EAGAIN`；`F_SETLKW` 每 10ms 检查冲突，pending signal/线程结束
  返回 `EINTR`。
- 关闭**任意**同文件 fd 释放该进程在该文件上的全部锁（POSIX 语义）；进程退出由
  `userland_fd_table_free()` 的 close 钩子 + `fd_lock_release_owner()` 兜底。
- `fd_lock_file_info()` 只接受 regular file；proc/dev/socket/pipe 的锁请求返回 `EINVAL`。
- trace：`RVVM_TRACE=fd` 会输出 `fcntl_lock setlk/getlk/setlkw/release_file/...`，
  每次等待/冲突/成功一行。

## 验证

在 `guest-build` 用 `GUEST_DEBUG=0` 构建两个 guest app：

- `probe_fcntl_simple`：单进程 `open → F_SETLK → F_GETLK → F_UNLCK → close`。
- `probe_fcntl_lock`：fork 子进程看到的父 write lock、子写/读冲突、`F_SETLKW` 阻塞
  到父解锁、close 释放、按 byte range 的冲突/非冲突锁；同时跑 `/tmp`（memfs）和
  `/root`（hostfs）。

独立测试（不启动 supervisor/ssh，避免把测试通道和被测逻辑混在一起）：

```powershell
# 从 release 根目录或临时 rootfs：
$env:RVVM_TRACE='fd'
$env:RVVM_VERBOSE='1'
.\rvvm_user_x86_64.exe -prefix <rootfs> /probe_fcntl_simple
.\rvvm_user_x86_64.exe -prefix <rootfs> /probe_fcntl_lock
```

结果：`probe_fcntl_simple` 输出 `simple: done`；
`probe_fcntl_lock` 连续 3 次（无 trace）输出 `PROBE-FCNTL-LOCK-PASS`，退出码 0；
trace 显示 `setlk -> ok`、`getlk -> type=0 ...`、`setlkw waiting (owner pid 100)`
后父 `setlk type=2 -> ok`、子 `setlkw -> ok`、`release_file n=1`。
`test_std` 直接跑 `159 checks / 0 failed / 7 skipped`（PASS）；之前经 SSH/pty
看到的 dup(2) 基线 FAIL 在直接运行时没有出现。

## 遗留

- 未在真 SQLite 二进制上跑（rootfs 无 sqlite3）；探针覆盖 SQLite unix VFS 使用的
  `F_GETLK/F_SETLK/F_SETLKW`、fork/close/byte-range 语义。
- `F_OFD_*` 未实现；标准 SQLite unix VFS 使用 `F_SETLK`，如启用 OFD 变体需另加。
- 跨 supervisor 实例（不同 core 进程）访问同一 DB 文件时，锁表不共享；这是
  emulator-owned 锁的边界，必要时可叠加宿主锁，但会重新引入 per-host-process owner。
