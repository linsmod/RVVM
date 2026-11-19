# 修复史：guest 可见的宿主信息与默认特权

修复日期：2026-11-19
背景与逐项执行记录：`task-host-info-exposure.md`
验证入口：`tools/supervisor/selftest.py`、`tools/fs_selftest.ps1`、
`tools/hostfs_selftest.ps1`；guest 侧 `test_std`。

## 现象

- guest 内 `cat /rootfs.modes` 能看到 `H:\...` 宿主绝对路径；`/.ash.lock` 带宿主 pid。
- guest 默认可以 `mount -t hostfs 'C:\' /mnt`，直接读写宿主文件。
- guest 内 `wget http://127.0.0.1:7911/api/health` 能读无 token 的 supervisor API
  （宿主路径、pid、其他实例），同一鉴权分支还能 stop/delete/exec。
- `/run/vpsessiond` 是所有实例共享的可写宿主目录，能看到/改写别人的 ready、auth.pub、日志。
- `df`/`stat -f` 显示宿主磁盘 1000G/97%；`stat` 的 `st_ino` 是 NTFS file id。
- `/proc/uptime` 与宿主 `LastBootUpTime` 一致；guest 二进制的 DWARF 带
  `H:/github_repos/RVVM`、`C:\Users\...\zig...`；`uname` 带 git commit/dirty。

## 根因

- guest hostfs 把 `mount(2)` 的 source 当宿主路径，且默认允许。
- supervisor 把 loopback 当鉴权边界；guest 的 socket 就是宿主 socket。
- 每个实例共用 `runtime/run` 作为 `/run/vpsessiond`。
- per-instance mode store/lock 放在 guest 的 `/` 里。
- `statfs` 对 host-backed root 落回宿主；`stat`/`getdents` 直接透传宿主 `st_ino`。
- uptime/monotonic 直接读宿主单调钟；`uname` 直接拼 `RVVM_VERSION`。
- guest ELF 默认 `-O0 -g` 并打进 bundle。

## 修法

| commit | 修法 |
|---|---|
| `b23e899` | guest hostfs 默认 `EPERM`；`rvvm_user_set_guest_hostfs()` + `rvvm_ash --allow-guest-hostfs`；宿主预挂载不受影响 |
| `dc0e96c` | supervisor 永远要求 token（自动生成 `runtime/supervisor/token`）；API 只回 basename、无 endpoint；`us`/ws/UI 同步 |
| `db5d27c` | 每实例 `runtime/run/<port>`，core 新增 `--run-dir`；auth.pub/ready/日志不再共享；`rm` 清理 |
| `f511db0` | mode store/lock 改 `<rootfs>.modes`/`<rootfs>.lock`；只持久化 prefix 内路径；`rm` 删 sidecar |
| `d44d5e3` | rootfs/hostfs statfs 合成固定预算（1 GiB/64 MiB），`fstatfs` 不再问宿主 |
| `1d5e6bc` | `st_dev` 固定、`st_ino=hash(host ino)`；`getdents64`/`statx` 同 hash |
| `72eea66` | `run_start_ns` rebase uptime/monotonic/starttime/btime/times；futex/clock_nanosleep 单调 deadline 加 base |
| `c5baea6` | `uname` 固定 `RVVM userland`；mounts 头 guest-facing；`GUEST_DEBUG=0` 时 `-O2 -s`，bundle 默认剥离 |
| `a115ef5` | `statx` 对 host-backed 路径与 `stat` 用同一 dev 0:1/ino hash；core 合成行返回 `ENOSYS`；`times()` 按 guest 4×64-bit `struct tms` 逐字段转换 |

## 验证

- `tools/supervisor/selftest.py`（隔离测试 release + 临时 daemon/DB/端口）：
  401/200、API 无绝对路径/endpoint、两实例 run 目录互不可见、guest hostfs 默认拒绝、
  statfs 预算、stat/getdents 同 ino 且不等于宿主、uptime/btime、uname/mounts 头、
  rm 清理 sidecar。约 33.5s。
- `fs_selftest` 90 checks PASS；`hostfs_selftest` 150 checks PASS。
- `test_std`：154 checks / 1 failed（既有 `dup(2)`/proc link 基线）/ 7 skipped；
  statx 在 Windows 为 SKIP。

## 已知遗留

- `statx` 身份分支未在 Linux 宿主实测。
- `times()` 的 `struct tms` 布局仍是既有 TODO。
- Android 宿主未逐项实测。
- 现有 `release.test` bundle 需重打才会更新 guest 二进制；仓库外
  `userland-vm` 技能文档需同步 token/per-instance run dir。
