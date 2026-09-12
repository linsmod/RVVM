我需要先摸清当前 `rvvm_user.c` 里所有的全局静态状态，以及它们被谁引用。让我系统地搜一遍。
输出被截断了。让我直接读关键区域，把全局静态状态完整梳理出来。
继续找其余的全局状态，包括表结构、`fake_root` 等。
再确认一下 fd 表和其他可能的状态，以及它们被哪些文件引用。
`-r` 被误当成替换参数了，重跑一次。
已经把现状摸清了。下面是完整结论与改造方案。

## 一、当前的文件作用域静态量（`rvvm_user.c`，共 21 个）

按职责可以分成 5 组，覆盖了所有实例状态：

| 组 | 变量 | 说明 |
|---|---|---|
| **A 实例句柄 / 回调 / 配置** | `userland`(machine*)、`io_callback`、`exit_callback`、`prefix_path`、`fake_root`、`fake_uid`、`fake_gid` | 引用最多的是 `userland`（34 次）、`prefix_path`（13） |
| **B guest 地址空间分配器** | `guest_lock`、`guest_free[]`、`guest_free_num`、`guest_bump`、`guest_mmap_end`、`guest_stack_base/top`、`guest_brk_start/end/ptr`、`brk_lock` | `guest_lock`/`guest_free` 各 14 次，是 mmap/brk 的核心可变状态 |
| **C guest 进程级状态** | `siga[64]`、`elf`、`interp` | ELF 映像 + 信号处置，**天然属于一个 guest 进程** |
| **D 线程注册表 / 生命周期** | `userland_threads_lock/threads`、`userland_main_thread`、`userland_exit_reported`、`userland_suspend`、`userland_parked` | 就是 suspend/stop 那套 |
| **E 调试 TLS** | `current_user_hart` | 本来就是 per-thread，放错位置了 |

整文件约 93 个函数、这些全局被引用约 150 次。**改造量可控，不需要重写。**

## 二、两个决定性事实（决定了方案）

1. **实例锚点已经存在**：`rvvm_create_userland()`（`src/core/rvvm.c:1105`）每次都 `safe_new_obj(rvvm_machine_t)`，而 `rvvm_hart_t.machine` 直连它（`src/core/rvvm.h:222`）。也就是说"每实例一个 machine + 若干 hart"的骨架已经在了，只是 `rvvm_user.c` 没利用它。
   - 但 `rvvm_machine_t`（`src/core/rvvm.h:288`）**目前没有 `userdata` 槽位**，需要加一个。

2. **guest 线程 == host 线程，1:1 且 hart 不跨线程迁移**（`riscv_hart_run_userland` 在创建它的宿主线程上跑，wrap 循环不换线程）。因此 `THREAD_LOCAL` 可以安全承载"当前实例"，而且项目已有可移植宏 `THREAD_LOCAL`（`compiler.h:517`，MSVC 走 `__declspec(thread)`，GCC/Clang 走 `__thread`）。

## 三、目标结构：一个 `rvvm_userland_t`

把 A–D 全部收进一个结构体，`userland.c` 私有：

```c
typedef struct rvvm_userland {
    rvvm_machine_t*         machine;        // 反向指针（原全局 userland）

    // A：配置 / 回调
    rvvm_user_io_callback   io_callback;
    rvvm_user_exit_callback exit_callback;
    const char*             prefix_path;
    bool                    fake_root;
    int                     fake_uid, fake_gid;

    // B：guest VA 分配器（锁随实例走，多实例不再互相串行）
    spinlock_t    guest_lock;
    guest_range_t guest_free[GUEST_FREE_MAX];
    size_t        guest_free_num;
    rvvm_addr_t   guest_bump, guest_mmap_end;
    rvvm_addr_t   guest_stack_base, guest_stack_top;
    rvvm_addr_t   guest_brk_start, guest_brk_end, guest_brk_ptr;
    spinlock_t    brk_lock;

    // C：进程级状态
    struct uapi_sigaction siga[64];
    elf_desc_t elf, interp;

    // D：线程注册表 / 生命周期
    spinlock_t           userland_threads_lock;
    vector_t(rvvm_user_thread_t*) userland_threads;
    rvvm_user_thread_t*  userland_main_thread;
    uint32_t             userland_exit_reported;
    uint32_t             userland_suspend, userland_parked;
} rvvm_userland_t;
```

**锚点**：在 `rvvm_machine_t` 里加一个**通用**槽位（不要让核心头认识 userland 类型，因为 `rvvm_user.c` 是可裁剪模块）：

```c
struct randomize_layout rvvm_machine_t {
    ...
    void* userdata;   // 由拥有者管理：userland 模式下指向 rvvm_userland_t
};
```

再提供访问器：

```c
static inline rvvm_userland_t* rvvm_user_ctx(rvvm_machine_t* m) {
    return m ? (rvvm_userland_t*)m->userdata : NULL;
}
```

## 四、两条访问路径（关键设计）

C 没有隐式 `this`，而 syscall 层（~90 个 handler）拿不到 hart。所以分两层：

- **有 hart/能拿到 machine 的地方** → `rvvm_user_ctx(hart->machine)`。用于线程 wrap、suspend/resume、`rvvm_user_linux` 主路径。
- **syscall 深层 helper（无 hart）** → 线程局部指针：

```c
static THREAD_LOCAL rvvm_userland_t* tls_uctx = NULL;
static inline rvvm_userland_t* uctx(void) { return tls_uctx; }
```

在每个 guest 入口（`rvvm_user_thread_wrap` 开头、`rvvm_user_linux` 主路径）设置：
```c
tls_uctx = rvvm_user_ctx(thread->cpu->machine);   // 或 machine
```

然后全是机械替换：`guest_brk_ptr` → `uctx()->guest_brk_ptr`、`prefix_path` → `uctx()->prefix_path` ……

**为什么用 TLS 而不是给每个 handler 加参数**：TLS 把改动收敛到"结构体定义 + 线程入口一次赋值 + 字段改名"三处；显式透传要改 90 个函数签名和全部调用点。而且 TLS 语义在本项目成立（1:1 线程绑定，不迁移）。

## 五、API 分层（区分 guest 上下文 vs 宿主控制）

这是本项目改造中最实用的一点：

| 类别 | 调用者 | 处理 |
|---|---|---|
| **guest 上下文 API**：`rvvm_user_guest_ptr()`、`rvvm_user_host_ptr()` | `vp_cmdpost.c`（Android NDK 代理，**跑在 guest 线程里**） | **签名不变**，内部走 `uctx()`。调用方 0 改动 |
| **宿主控制 API**：`rvvm_user_suspend/resume/is_suspended/stop`、`set_io/exit_callback`、`rvvm_user_linux` | `win32_cmdpost_bridge.c`、`jni_bridge.c`（**GUI / 控制线程**，无 TLS） | **加实例参数**，如 `rvvm_user_suspend(rvvm_machine_t*)` |

宿主控制线程没有 TLS，这正是它必须显式带实例的原因——也是当前 `rvvm_user_suspend()` 作为全局函数在多实例下会"挂错实例"的根本原因。

## 六、迁移步骤（可分批、每批可编译）

1. 加 `rvvm_machine_t.userdata` + `rvvm_userland_t` 定义 + `uctx()`。
2. `rvvm_create_userland()` 里 `machine->userdata = safe_new_obj(rvvm_userland_t)` 并初始化（`machine` 反填、回调置 NULL）。
3. 线程入口写 `tls_uctx`。
4. 机械替换 A/B/C/E 组引用（sed 可搞定）：`userland`→`uctx()->machine`（或直接用局部 hart 的 machine）。
5. D 组：suspend/stop 改为操作 `uctx()->userland_threads`，函数加实例参数。
6. 删掉 `rvvm_user_linux` 里那段"重置上一次 guest"的代码（`rvvm_user.c:2522-2531`）——每实例独立 ctx 后就不再需要，这是本改造最直接的收益。
7. 外部调用点：`win32_cmdpost_bridge.c` / `jni_bridge.c` / `rvvm_user_main.c` 传入 machine 句柄；`vp_cmdpost.c` **不用改**。

## 七、必须注意的坑

- **`userland` 存不存在的问题**：`rvvm_user_linux` 目前是"没 machine 就自己建一个"（`rvvm_user.c:2536`）。多实例下建议拆成 `rvvm_user_linux_ex(machine, argc, argv, envp)`，保留原签名做单实例便捷包装。
- **spinlock 位置**：`guest_lock`/`brk_lock`/`userland_threads_lock` 必须进 ctx，否则两个实例会互相串行、或 `SPINLOCK_INIT` 静态初值不再适用。放到 `safe_new_obj` 零初始化即可（现存 `brk_lock = {0}` 说明零值合法）。
- **`current_user_hart`** 直接改 `THREAD_LOCAL`（独立小修，和整体重构解耦）。
- **`siga[]` 是 guest 可见的进程级状态**，跟实例走没问题；但 `user_fault_handler_install()` 装的宿主信号处理器是**进程全局**的（还带 `_Exit`），多实例下要改成只做诊断或去重，别每个实例装一遍。
- **cmdpost 层（`vp_cmdpost.c`）自己也有一堆全局**（`g_window_lock_cb` 等），是 NDK 代理状态。真正的多实例（并发多个 guest）也要一并收敛；顺带一提，win32 启动器已经是**同一进程顺序跑多个 guest**（所以才需要那段 reset），改造后顺序复用也顺带更稳。
- **TLS 未设的风险**：任何在"非 guest 线程"里调用 guest 上下文 helper 的路径都会拿到 NULL。加断言/兜底（`to_ptr` 等回退到最近设置或直接返回 NULL）而不是静默崩溃。

---


重构已完成。结果如下：

## 改动概况

```
 src/core/rvvm.h                                    |   6 +
 src/core/rvvm_user.c                               | 708 +++++++++------------
 src/core/rvvm_user.h                               |  50 +-
 src/virtpass/android-host/.../jni_bridge.c         |  40 +-
 src/virtpass/win32-host/win32_cmdpost_bridge.c     |  79 +-
 5 files changed, 593 insertions(+), 290 deletions(-)
 ?? src/core/tls-userland-multi-instance-plan.md   (计划文档，未跟踪)
```

## 核心结果

1. **新增通用锚点** `rvvm_machine_t::userdata`（`src/core/rvvm.h`）—— userland 模式指向 `rvvm_userland_t`，完成「每实例状态」的挂载点。

2. **全局 static 收敛**（`src/core/rvvm_user.c`）：原本的文件级 static 全局按分组 A–E 迁入 `rvvm_userland_t`：
   - A 配置/回调、B guest VA 分配器、C 进程映像（siga/elf/interp）、D 线程注册表/生命周期
   - E `current_user_hart` 保留为 `THREAD_LOCAL`（guest 线程与宿主线程 1:1）
   - 双访问路径：`rvvm_userland_ctx(machine)`（有 hart/machine）+ TLS `uctx()`（深层 syscall 辅助）

3. **API 分层调整**：host 控制 API 改为接收 `rvvm_machine_t*`，并拆分创建/运行/释放：
   - `rvvm_user_create()` → `rvvm_user_linux_ex(machine, argc, argv, envp)` → `rvvm_user_free(machine)`
   - `rvvm_user_suspend/resume/is_suspended/stop/set_io_callback/set_exit_callback` 全部改为 `(rvvm_machine_t*, ...)`
   - `rvvm_user_linux(...)` 保留为单例便捷入口

4. **调用点接线**：win32 与 android host 均新增 `g_guest_machine` 句柄，运行线程局部捕获并回传，各控制路径（Stop/Suspend/Resume/Shutdown/Exit）改为按 machine 操作。

## 验证

- `build_virtpass-win32.ps1`（release.windows.x86_64）编译通过
- 冒烟测试：`test_render.exe` / `test_game_activity.exe` 正常启动与退出
- 早期失败路径 `does_not_exist.exe` 正常报错、释放 machine、退出码 1，无崩溃
- 同进程两次连续启动 guest 的自动化测试通过

按你的要求未做清理：临时脚本/日志与 `src/core/tls-userland-multi-instance-plan.md` 均保留。改动尚未提交。

---
我先读取需要改动的几个安卓侧文件。
现在读取 JNI 桥接文件的相关部分。
现在看 win32 侧的 suspend/resume 参考实现语义。
需要拿到精确内容用于编辑，重新完整读取 `jni_bridge.c`。
现在读取 Java 侧文件。
现在重新读取 Java 侧文件以精确编辑。
现在开始改动。先加 vsync 启停辅助函数：
现在读取 Java 侧文件。
改动完成，构建验证。
`.o` 时间戳（9:56:11）晚于源文件（9:55:26），确认 C 改动已编入。再构建 APK 以验证 Java 与资源。
验证 vsync 线程停止的竞态处理，读一下当前线程函数实现：
改用主机侧标志位，避免在 UI 轮询线程中解引用已被 guest 线程释放的 machine：
安卓侧 suspend/resume 已接到按钮，构建通过（CMake 双 ABI + Java + 资源）。

## 改动

**`jni_bridge.c`**
- 抽出 vsync 源启停：`jni_vsync_start()` / `jni_vsync_stop()`（幂等）；`nativeInit` / `nativeDestroy` 改为调用它们，不再内联那段线程创建/拆除代码。
- 新增主机侧状态 `g_guest_suspended`：`nativeIsGuestSuspended()` 由此返回，不解引用 machine —— 因为 guest 线程退出时会立刻 free 掉 machine，而 UI 是 100ms 轮询线程，直接查 machine 有 UAF 窗口。它在 `nativeRunElf` / `guest_thread_func` / 停用路径统一清零。
- 新增三个 JNI：

```1340:1390:src/virtpass/android-host/app/src/main/cpp/jni_bridge.c
Java_com_rvvm_android_RvvmNative_nativeSuspendGuest(...)
Java_com_rvvm_android_RvvmNative_nativeResumeGuest(...)
Java_com_rvvm_android_RvvmNative_nativeIsGuestSuspended(...)
```

- suspend：`rvvm_user_suspend()` 后 `jni_vsync_stop()`（对齐 win32 的 `vsync_clock_stop`，被 park 的 guest 不再有 60Hz 空转时钟）。
- resume：先 `jni_vsync_start()` + `jni_register_cmdpost_callbacks()`（`stop` 会把 source 标记为 lost 并丢掉 fd，重注册既清 lost 标志、也让 guest 重新走 fd 唤醒快路径），再 `rvvm_user_resume()`，保证 guest 醒来时时钟已就绪。
- `nativeStopGuest`：若处于挂起，先 resume 再 stop（挂起的 vCPU 无法轮询停机请求）；此路径不重启 vsync，guest 拆除期间回落到自有时钟。`nativeDestroy` 同样先 resume 再清桥。

**`RvvmNative.java`**：新增 `nativeSuspendGuest()` / `nativeResumeGuest()` / `nativeIsGuestSuspended()`。

**`activity_main.xml`** + **`strings.xml`**：Run 与 Stop 之间加 `suspendButton`（`Suspend`/`Resume` 文案）。

**`MainActivity.java`**：`toggleSuspendGuest()` 切换挂起/恢复并更新状态栏；`updateButtonStates()` 里 `suspendButton` 跟随 `nativeIsGuestRunning()` 启停，文案按挂起态切换。

## 设计取舍
- `nativeSuspendGuest/Resume` 返回 void（对齐 win32 的 `guest_suspended()`/`guest_resume()` 语义），UI 状态统一由 `nativeIsGuestSuspended()` 查询；`rvvm_user_suspend()` 的 `parked` 布尔只在 C 侧用于日志（`parked` / `parking (one is in a blocking syscall)`）。
- 挂起时停 vsync 源：与 win32 一致；Android 侧 `ALooper_wake()` 写的是常驻 eventfd，不会丢唤醒，故 `pthread_join` 安全。

## 验证
`./build_virtpass-android.ps1 -Target apk` → BUILD SUCCESSFUL，`buildCMakeDebug[arm64-v8a]` / `[x86_64]` 均执行；`read_lints` 0 报错。真机 Run→Suspend→Resume→Stop 冒烟未做（无设备）。