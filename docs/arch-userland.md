# 架构现状：userland 模拟 + VirtPass 宿主

最后核对：2026-11-17（依据源码与 `src/core/rvvm_user.h` 注释，未逐条实测）。

本文只写「现在是什么」。设计取舍见 `../handover.md`（Step 1-6 落地记录，早于挂表行模型，读时注意）、
调试经验见 `../dev_debug_notes.md`、构建与运行细节见 `../DEV.md`。

## 1. 代码地图

| 层 | 位置 | 说明 |
|---|---|---|
| userland 模拟核心 | `src/core/rvvm_user.c` | 最大单文件（≈900 KB）：RISC-V syscall 翻译、guest 进程/fd 表、procfs 合成、console attach、挂表 |
| 宿主 API | `src/core/rvvm_user.h` | 长注释即现状说明，改行为前先读对应段落 |
| 文件系统抽象 | `src/core/rvvm_fs.{c,h}` | 统一 `stat/readdir/open` 等操作面 |
| 内存文件系统 | `src/core/rvvm_memfs.{c,h}` | 无 guest 参与的纯数据结构，可宿主侧单测 |
| 宿主目录 backing | `src/core/rvvm_hostfs.{c,h}` | 每次操作都要落到真实目录，只能端到端测 |
| UI/设备/CPU | `src/gui`、`src/devices`、`src/cpu`、`src/rvjit` | 上游 RVVM 部分 |
| win32 宿主 | `src/virtpass/win32-host` | `rvvm_winhost`、`rvvm_ash`（控制台宿主）、`vp_client`（本地 shell 客户端） |
| Android 宿主 | `src/virtpass/android-host` | APK + `librvvm_jni.so` |
| guest 侧 ABI stub | `include/`、`src/virtpass/vp_ndk_stub.c`、`vp_session.c`、`vp_cmdpost.c`、`vp_sensor.c` | VirtPass 抽象层，NDK 部分必须跟随 Android NDK 签名 |
| guest 样本 | `src/virtpass/guest-samples` | `test_*` 可执行样本 |
| 日志 | `src/util/utils.c` | `log_emit()` 是全树唯一成行函数 |

## 2. 命名空间

- run 的 `/` 由 `rvvm_user_set_prefix()` 决定：**NULL = 内存文件系统（run 结束即消失）**，
  非 NULL = 宿主目录（bundle 由 `vp_rootfs_extract()` 安装进去）。
- **其余一切都是挂表的一行**：`rvvm_user_mount_hostfs()` 或 guest 自己 `mount(2)`
  （`-t hostfs`、`tmpfs`）。`/dev`、`/sys`、`/proc`、`/tmp`、`/var/tmp` 各有自己的一行，不吃 prefix。
- `root=` 是 **guest 路径**，与 prefix 组合（等价 chroot），不是宿主路径；只在有 prefix 时生效。
- dev 号由合成规则给出，不用宿主真实 `st_dev`（三宿主行为必须一致）。
- `/proc` 由 guest 进程表合成；`/proc` 的名单只列**已实现**的项。

## 3. 进程、fd、会话

- fd 后端是 `rvvm_fd_backend_t`：**console 是一种 attach，不是一个描述符号**。
  `is_console` 问 backend，不问 `fd==0/1/2`。
- 普通描述符不得占用宿主 0/1/2（`userland_fd_host_off_console`）。
- 合成 fd 的整套入口都要接：`read/readv/pread64/write/pwrite64/lseek/fstat/close/dup/poll`
  加 `sendfile` 路径，否则会出现「一半工具正常」。
- 进程枚举从 family root 沿 `child_ctx` DFS 并按 pid 去重（`ps` 自己是子进程）。
- `vpsessiond` 是 guest 程序：`main()` 里 `dup2` 把日志落到 run 的 rootfs `/tmp/vpsessiond.log`，
  宿主从 `<release>/runtime/rootfs/tmp/vpsessiond.log` 读。

## 4. 宿主与启动方式

- guest 只作为 **bundle 里的 app 包**启动：`apps.tar.gz -> apps/<id>.vapp`，入口与参数取自包内 `meta.json`。
- `rvvm_winhost.exe --app <id> [args...]`；无 `--app` = picker launcher。
  **已不存在** `--guest` / `--direct` / `RVVM_ASH_SHELL` 这类「按路径跑任意 ELF」的门。
- `rvvm_winhost` 是 console 应用，stdin 会泵进 guest 控制台，可用管道脚本化驱动。
- guest 退出码 = 进程退出码。

## 5. 日志不变量

- host 侧写日志**只用** `rvvm_log()` / `RVVM_LOGx(tag, ...)`，不要自造宏，不要 `printf` 到 stdout。
- sink 可换（`rvvm_log_set_sink`），默认 stderr（Android 另加 logcat）；sink 不得回调 logger。
- 128 KiB 定长 ring 始终保留最近的行；`rvvm_fatal()` 转储，`RVVM_LOG_RING_DUMP=<path>` 正常退出时写文件。
- **guest console 不是日志**，不并入 logger。
- 开关：`RVVM_VERBOSE`、`RVVM_TRACE=fd|sys|path|all`、`RVVM_LOG_FILE`、`RVVM_NOJIT`。

## 6. 构建与验证

- 唯一构建入口：`./build_virtpass.ps1 -Target {all|win32|apk|android|assets|dist|jni|sdk|clean}`；
  真正的编译规则在 `Makefile` + `project.mk`（mingw32-make，产物落 `release.windows.x86_64/`）。
- 验证入口是 `tools/*_e2e.ps1` 与 `tools/*_selftest.ps1`，见 `../tools/INDEX.md`。
- 详见工作流 `../workflows/wf-build-and-test-win32.md` 与 `../workflows/INDEX.md`。
