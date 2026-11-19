# 测试耗时记录

最后核对：2026-11-19。时间为本机 Windows / release 树的实测，仅作量级参考；
并行环境或冷缓存会不同。

| 测试 | 环境 | 实测 | 备注 |
|---|---|---|---|
| `tools/supervisor/selftest.py` | Windows，独立测试 release + 7912 daemon + 7940/7941 实例 | 33.5 s | 两实例 + SSH 检查；结束清理 |
| `tools/fs_selftest.ps1` | Windows | 未计时（PASS） | 90 checks，0 failures |
| `tools/hostfs_selftest.ps1` | Windows | 未计时（PASS） | 150 checks，0 failures |
| `/data/app/test_std/bin/test_std.exe` | 7940 实例 | 未计时 | 154 checks，1 failed（既有 dup(2) 基线），7 skipped |
