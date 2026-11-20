# 测试耗时记录

最后核对：2026-11-19。时间为本机 Windows / release 树的实测，仅作量级参考；
并行环境或冷缓存会不同。

| 测试 | 环境 | 实测 | 备注 |
|---|---|---|---|
| `tools/supervisor/selftest.py` | Windows，独立测试 release + 7912 daemon + 7940/7941 实例 | 48.8 s | 两实例 + SSH 检查 + 卷用例 8 项（`/persist`、`volumes`、`rm` 保留、`--purge` 删除，后者多起一个实例）+ prune 2 项；结束清理。早期版本 33.5 s |
| `tools/fs_selftest.ps1` | Windows | 未计时（PASS） | 90 checks，0 failures |
| `tools/hostfs_selftest.ps1` | Windows | 未计时（PASS） | 150 checks，0 failures |
| `/data/app/test_std/bin/test_std.exe` | `rvvm_user -prefix <rootfs> /test_std`（直接，不 ssh） | < 30 s（PASS） | 159 checks，0 failed，7 skipped；times/tms 5 项 PASS，statx 在 Windows SKIP。之前经 SSH/pty 时出现的 dup(2) 基线 FAIL 在直接运行下不复现 |
| `rvvm_user -prefix <rootfs> /probe_fcntl_lock` | Windows 宿主，直接 userland | < 1 s（PASS） | fork/F_GETLK/F_SETLK/F_SETLKW/byte range/close 释放；`RVVM_TRACE=fd` 可见锁表 trace |
| `rvvm_user -prefix <rootfs> /probe_fcntl_simple` | 同上 | < 1 s（PASS） | 单进程 open/lock/getlk/unlock/close |
| `rvvm_user -prefix <rootfs> /probe_mmap_shared` | 同上 | 0.04 s（PASS） | P0：可写 `MAP_SHARED` 答 `ENODEV`、只读共享快照、私有映射不回写；P2 落地后同一探针会跑共享页用例 |
