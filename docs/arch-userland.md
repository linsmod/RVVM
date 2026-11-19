# 架构现状：userland 模拟（guest 侧）

最后核对：2026-11-19（依据源码与 `src/core/rvvm_user.h` 注释；hostfs 默认拒绝已端到端实测）。

只写「现在是什么」。宿主侧（VirtPass ABI、bundle 启动面、日志 sink/ring）见 `arch-virtpass.md`；
设计取舍见 `../handover.md`（Step 1-6 落地记录，早于挂表行模型，读时注意）；
踩坑与教训见 `hist-lessons.md`。

## 1. 代码地图

| 层 | 位置 | 说明 |
|---|---|---|
| userland 模拟核心 | `src/core/rvvm_user.c` | 最大单文件（≈900 KB）：RISC-V syscall 翻译、guest 进程/fd 表、procfs 合成、console attach、挂表 |
| 宿主 API | `src/core/rvvm_user.h` | 长注释即现状说明，改行为前先读对应段落 |
| 文件系统抽象 | `src/core/rvvm_fs.{c,h}` | 统一 `stat/readdir/open` 等操作面 |
| 内存文件系统 | `src/core/rvvm_memfs.{c,h}` | 无 guest 参与的纯数据结构，可宿主侧单测 |
| 宿主目录 backing | `src/core/rvvm_hostfs.{c,h}` | 每次操作都要落到真实目录，只能端到端测 |
| 进程/内存 | `src/core/rvvm.c` | userland hart 的创建与释放（`rvvm_create_user_thread`）走 `machine->harts` 列表 |
| UI/设备/CPU | `src/gui`、`src/devices`、`src/cpu`、`src/rvjit` | 上游 RVVM 部分 |
| 宿主 | `src/virtpass/win32-host`、`src/virtpass/android-host` | 见 `arch-virtpass.md` |

## 2. 命名空间

- run 的 `/` 由 `rvvm_user_set_prefix()` 决定：**NULL = 内存文件系统（run 结束即消失）**，
  非 NULL = 宿主目录（bundle 由 `vp_rootfs_extract()` 安装进去）。
- **其余一切都是挂表的一行**：`rvvm_user_mount_hostfs()` 或 guest 自己 `mount(2)`
  （`-t hostfs`、`tmpfs`）。`/dev`、`/sys`、`/proc`、`/tmp`、`/var/tmp` 各有自己的一行，不吃 prefix。
- **guest 自己 `mount -t hostfs <宿主路径>` 默认被拒绝**（`EPERM`）：宿主路径是宿主信息，
  不是 guest root 的自带权限。宿主用 `rvvm_user_mount_hostfs()` 预挂载不受影响；
  `rvvm_ash --allow-guest-hostfs` / `rvvm_user_set_guest_hostfs()` 是显式授权，测试与
  WSL 形态才打开。默认关是 2026-11-19 宿主信息收敛的一部分。
- **`statfs`/`df` 不回宿主磁盘数字**：rootfs 行报 1 GiB、hostfs 行报 64 MiB 的固定 guest 预算，
  不落回宿主 `statfs()`；`fstatfs` 对宿主持久 fd 同样用预算。数字不是配额，只是 guest 视图。
- **guest 时间从 run 起点算**：`/proc/uptime`、`sysinfo.uptime`、`/proc/<pid>/stat` starttime、
  guest `CLOCK_MONOTONIC*`/`BOOTTIME`、`times()` 都减去 `run_start_ns`；`/proc/stat btime`
  是 guest 启动 epoch（now - uptime）。宿主 uptime 不再出现在 guest。
  `times()` 的 `struct tms` 按 guest 的 4×64-bit 字段逐个转换，不复制宿主 `struct tms`
  （Windows `clock_t` 是 32 位）。
- **guest 可见的构建标识收敛**：`uname.version` 固定为 `RVVM userland`（不再带
  `RVVM_VERSION` 的 git/dirty）；`/proc/mounts` 头是 guest-facing 文字；release/bundle
  的 guest ELF 默认 `GUEST_DEBUG=0`（`-O2 -s`），构建路径不进入 bundle；调试行表用
  `GUEST_DEBUG=1`。
- **宿主持久文件的身份合成**：`st_dev` 固定为 `RVVM_GUEST_DEV`（dev 0:1），
  `st_ino = guest_ino_hash(host st_ino)`；`getdents64` 的 `d_ino` 用同一 hash，
  `statx` 对 host-backed 路径用同一 hash 并写 `stx_dev_major/minor = 0/1`。
  `statx` 对 proc/dev/tmpfs 等 core 合成行返回 `ENOSYS`（交给 libc 回退 `fstatat`），
  避免拿宿主路径去 statx 或和 `stat` 给出不同身份。Windows 上 statx 本身 ENOSYS/SKIP。
  宿主 NTFS file id / 卷号不再直接交给 guest；`stat`/`fstat`/`getdents` 之间的 ino 保持一致。
- **per-instance mode store / rootfs lock 是 rootfs 外的 sidecar**：
  `<rootfs>.modes` / `<rootfs>.lock`；`<rootfs>` 是 guest 的 `/`，所以这两个文件不再
  出现在 guest 里。mode store 只持久化 prefix 内的路径，hostfs chmod 的宿主路径不写入。
  默认（无 `--rootfs`）仍是 `runtime/rootfs.modes` / `runtime/ash-core.lock`。
- `root=` 是 **guest 路径**，与 prefix 组合（等价 chroot），不是宿主路径；只在有 prefix 时生效。
- dev 号由合成规则给出，不用宿主真实 `st_dev`（三宿主行为必须一致）。

## 3. `/proc`（procfs）

`/proc` 由 guest 进程表合成；宿主目录里只有 bundle 写的真实 `/proc/mounts`。

- 已支持：`/proc` 列 pid、`/proc/self`、`/proc/<pid>/{stat,status,statm,cmdline,comm,fd}`、
  `cwd/exe/root` readlink，以及 `/proc/{uptime,stat,meminfo,version,cpuinfo,loadavg,filesystems,cmdline}`。
  `sysinfo(2)` 在非 Linux 宿主上合成。`ps`/`top` 因此可用。
- **名单只列已实现的项**：列了却打不开，`ls -l /proc/<pid>` 会报错。
- **不存在的 pid 要在所有入口返回 ENOENT**（open/stat/readlink/faccessat 都查
  `userland_proc_find_family()`），否则 `stat /proc/999/stat` 会「假成功」。
- 字段要给合理值：`vsz/rss` 全填 0 会被 busybox 的 `top` 判成 swapped（`STAT=W`）；
  `comm` 与 `argv[0]` 不一致时 busybox 会自己加 `{}`，不是 bug。

## 4. 进程、fd、会话

- fd 后端是 `rvvm_fd_backend_t`：**console 是一种 attach，不是一个描述符号**。
  `is_console` 问 backend，不问 `fd==0/1/2`；`dup2(console, n)` 复制的是同一个 attach，
  `close` 它不动宿主句柄。普通描述符不得占用宿主 0/1/2（`userland_fd_host_off_console`）。
- 合成 fd 的**整套**入口都要按 fd 分派：`read/readv/pread64/write/pwrite64/lseek/fstat/close/dup/poll`
  加 `sendfile` 路径（`userland_read_fd/write_fd`），否则会出现「一半工具正常」。
- 合成对象的表放**全局**（emulator 级 + 全局锁），不放 per-address-space：fork 会让子进程继承同一个合成号，
  per-AS 表里没有这个号 ⇒ 子进程关不掉、引用计数不归零、对象泄漏。
- **POSIX 记录锁（SQLite）**：`fcntl(F_GETLK/F_SETLK/F_SETLKW)` 由 emulator 自己的锁表实现，
  键是 `(guest process/ctx, host 文件 dev+ino 或 memfs ino, byte range, type)`；不是宿主 `fcntl`，
  因为所有 guest 进程共用一个宿主进程，宿主锁会失去 per-process 语义。`fork` 不继承锁（owner 是 ctx），
  关闭**任意**同文件 fd 释放该进程在该文件上的全部锁；进程退出由 fd 表清理和 owner sweep 释放。
  `F_SETLK` 冲突返回 `EAGAIN`，`F_SETLKW` 以 10ms 轮询并响应 pending signal（`EINTR`）。
  `RVVM_TRACE=fd` 会打出 `fcntl_lock setlk/getlk/setlkw/...` 行；探针：
  `/data/app/probe_fcntl_lock/...`（fork/阻塞/close）与 `probe_fcntl_simple`（单进程）。
  可在宿主直接 `rvvm_user -prefix <rootfs> /probe_fcntl_lock` 独立跑，不必起 supervisor/ssh。
- 生命周期照 pty/dev 的范式：`is_fd` → `fd_ref`（dup/inherit）→ `fd_release`（close）；
  `fd_release` 在锁内取 `data`、`memset` 槽、解锁后再 `free`。
- 进程枚举从 family root 沿 `child_ctx` DFS 并按 pid 去重（`ps` 自己是子进程）。
- `vpsessiond` 是 guest 程序：`main()` 里 `dup2` 把日志落到 run 的 rootfs `/tmp/vpsessiond.log`，
  宿主从 `<release>/runtime/rootfs/tmp/vpsessiond.log` 读。

## 5. 构建与验证

- 唯一构建入口：`./build_virtpass.ps1 -Target {all|win32|apk|android|assets|dist|jni|sdk|clean}`；
  真正的编译规则在 `Makefile` + `project.mk`（mingw32-make，产物落 `release.windows.x86_64/`）。
- 验证入口是 `tools/*_e2e.ps1` 与 `tools/*_selftest.ps1`，见 `../tools/INDEX.md`。
- 详见工作流 `../workflows/wf-build-and-test-win32.md` 与 `../workflows/INDEX.md`。
