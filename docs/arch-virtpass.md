# 架构现状：VirtPass 抽象层与宿主运行时约束

最后核对：2026-11-17（来源：原根目录 `DEV.md` 的现状段落，未逐条重新实测）。

只写「现在是什么」。构建与运行的操作步骤见 `../workflows/wf-build-and-test-win32.md`，
踩坑与教训见 `hist-lessons.md`，guest 侧命名空间/fd/进程见 `arch-userland.md`。

## 1. VirtPass 是什么

给应用一套 API，让它能跑在 VirtPass 平台上。VirtPass 是抽象层概念，由模拟器提供 bridged API：

- Android NDK 风格的 ABI：GameActivity 生命周期、Looper、sensors、Input、ANativeWindow（用户态）
- EGL/GLES（已嵌入，未测试）
- 其他 API

**扩充必须遵循**：NDK 部分的 ABI 签名严格跟随 Android NDK，可以按需只实现某类的子集。

## 2. 代码位置

| 内容 | 位置 |
|---|---|
| stub 头文件 | `include/` |
| win32 宿主 | `src/virtpass/win32-host` |
| Android 宿主 | `src/virtpass/android-host` |
| guest 样本 | `src/virtpass/guest-samples` |
| guest 侧 SDK | `src/virtpass/vp-sdk` |
| 运行时 stub 实现 | `src/virtpass/vp_ndk_stub.c`、`vp_session.c`、`vp_cmdpost.c`、`vp_sensor.c` |

## 3. 启动面（宿主 → guest）

guest **只能**作为 bundle 里的 app 包启动：`apps.tar.gz -> apps/<id>.vapp`，入口与参数取自包内 `meta.json`。

```powershell
rvvm_winhost.exe --app <id> [args...]     # Android-host intent 的对应物
rvvm_winhost.exe                          # 无 --app = picker launcher
```

- **已不存在** `--guest` / `--direct` / `RVVM_ASH_SHELL` 这类「按路径跑任意 ELF」的门；
  传一个目录/路径进去只会落到 picker。
- 调试开关：`RVVM_TRACE`、`RVVM_VERBOSE`。
- win32 host 是 console 应用，stdin 会泵进 guest 控制台（等 guest 真正启动后才开始读），
  所以可以用 test pipe 脚本化驱动：

  ```powershell
  printf 'ls /\nexit\n' | rvvm_winhost.exe --app test_cli
  ```

- guest 的退出码 = 进程退出码，guest stdout = 进程 stdout。
- **例外**：需要「命令运行中按键」时序的作业控制（`^C`/`^Z`/`jobs`/`fg`）管道复现不了，
  用 `tools/jobctl_e2e.ps1`，见 `../tools/INDEX.md`。

## 4. 会话与控制台

- `vpsessiond` 是 **guest** 程序，够不着宿主 logger：`main()` 开头 `dup2(fopen("/tmp/vpsessiond.log","a"), 1)`，
  日志落在 run 的 rootfs 里；宿主从 `<release>/runtime/rootfs/tmp/vpsessiond.log` 读。
  `tools/session_e2e.ps1` 启动前删旧日志，并断言「daemon 行不进 console」+「core 记了 endpoint/session」。
- **console 是 attach，不是描述符**（`rvvm_fd_backend_t`）：槽的 `backend` 说明这个号指向什么
  （宿主句柄 / 模拟器自有对象 / 控制台）。0/1/2 初始化时 `backend=CONSOLE`，
  `dup2(console, n)` 复制的是同一个 attach 而不是 `dup()` 宿主 stdout，`close` 它不会动宿主句柄。
  因此 `is_console` 一律问 backend，不问 fd 号；`shared` 标记不再承担 console 职责，
  只留给「dup 失败时骑在父进程宿主 fd 上」这一种情况。
- 普通描述符**不得占用宿主 0/1/2**（`userland_fd_host_off_console`）：core 以 std 句柄被 detach 的方式
  启动时，CRT 会把 0 分配给第一个打开的文件，与 console attach 的落地句柄撞号。

## 5. 日志：一个 sink + 一个 ring

- `src/util/utils.c` 的 `log_emit()` 是全树**唯一**的成行函数。六个 level（`debug/info/warn/error/fatal/trace`）
  和每个 host 的 tag 都汇进它。
- host 侧写日志**只用** `rvvm_log(level, tag, fmt, ...)` 与 `RVVM_LOGx(tag, ...)`，不要在文件里自定义宏：

  ```c
  #define VP_TAG "RVVM-JNI"
  #define LOGI(...) RVVM_LOGI(VP_TAG, __VA_ARGS__)   /* 保留旧名字，调用点不动 */
  ```

- tag 同时是 Android 的 logcat tag（`adb logcat RVVM-JNI:D` 照旧有效）；logcat priority 按 level 映射。
- **guest console 不是日志**，不并入 logger：win32 上它就是宿主 stdout，折进来会重演
  vpsessiond 诊断插进 transcript 的老问题。guest 侧仍走 `jni_guest_output` → `vp_session.c` 的行组装。
- **sink 可换**：`rvvm_log_set_sink((level, cat, tag, ids, line, len))`，行已格式化；默认 sink = stderr
  （Android 另加 logcat），并导出 `rvvm_log_default_sink` 供自定义 sink 委托。sink 不得回调 logger
  —— `log_emit` 的 `log_in_call` 递归守卫会丢掉那行。**目前两个 host 都没装自定义 sink**，它是扩展点。
- **ring**：`LOGRING_BYTES` = 128 KiB，定长静态、不 malloc；每条到达 sink 的行同时进 ring，
  旧行被覆盖、新行永远保留。用途：崩溃后 tail 还在；`RVVM_TRACE=all` 这种每秒几千行的噪音可以先收下、
  事后 dump。`rvvm_fatal()` 在 abort 前把 ring dump 到 stderr；`RVVM_LOG_RING_DUMP=<path>` 正常退出时写文件
  （故意是文件不是 stderr：正常退出时那份 transcript 还在被读）。
- **pid/tid 不需要额外插桩**：身份是 `rvvm_user.c` 注册的一次回调（`userland_trace_ids`），读 TLS 的
  `current_user_thread`，`log_prefix()` 每行调一次。宿主线程是 `[host:N]`，N 每线程一个、进程内不复用；
  没注册 formatter 的构建（`rvvm_cli`、`librvvm`）保持 `INFO: ` 裸形式。

```powershell
RVVM_VERBOSE=1        rvvm_winhost --app test_cli     # INFO 到 stderr
RVVM_LOG_FILE=<path>  rvvm_winhost --app test_cli     # 每行另存文件（驱动用这个）
RVVM_LOG_RING_DUMP=<path> ...                         # 退出时写 ring
adb shell setprop debug.rvvm.verbose 1                # Android 侧
adb shell setprop debug.rvvm.trace sys,path
```

`RVVM_LOG_FILE` 是**懒开**的：默认 loglevel=WARN 且一个 WARN 都没有时文件不会存在，
这等于「这次运行没有诊断」。

## 6. /proc

`/proc` 由 guest 进程表合成，宿主目录里只有 bundle 写的真实 `/proc/mounts`。支持面与约束见
`arch-userland.md`（进程与 fd 一节）。
