注：本文为初期决策记录，不代表最新状态。

我先分析现有的 stub 架构，看看已有的实现方式。
我把现有 stub 架构完整读了一遍（`vp_cmdpost.c` / `vp_cmdpost.h`、`vp_ndk_stub.c`、`vp_sensor_ringbuf.h`、`win32-host/*`、`android-host/jni_bridge.c`、`project.mk`）。结论：**方案可行，而且和现有架构契合度很高**，基本是复刻现有 vsync 的「回调 vtable + fd 唤醒 + SPSC ringbuf」三件套。下面是依据和具体设计。

## 一、四个决定可行性的事实

| # | 事实 | 依据 | 对音频的意义 |
|---|---|---|---|
| 1 | **guest 虚拟地址 == host 虚拟地址**，host 拿到 a1..a5 直接当指针用 | `vp_cmdpost.c` 里 `(void*)(size_t)a1` 直接解引用；`on_window_lock(window, outBuffer, ...)` 把 guest pixel 指针当 host 指针 | 配置结构体、PCM ring buffer **可零拷贝共享**，不需要任何地址翻译/拷贝层 |
| 2 | **guest 线程是真 host 线程** | `rvvm_sys_clone()` → `rvvm_thread_create_ex(rvvm_user_thread_wrap,...)`，每个 hart 一个 host 线程，无全局执行锁（只有 mmap/brk 的 spinlock） | **callback 模式可以完全在 guest 侧跑**，避免 host 音频线程回调进 guest（那是本架构的硬约束，vsync 就是靠 fd 唤醒绕开的） |
| 3 | **guest fd 就是 host fd**（syscall 直通，Looper 用真 `poll()`） | `SYS_ANDROID_CHOREOGRAPHER_SET_FD`：guest 把 pipe 写端交给 host，`vp_cmdpost_vsync_tick()` 直接 `write(fd,...)` | 直接照搬做**音频 drain 唤醒**：稳态下 `AAudioStream_write` 阻塞在 guest 侧 `poll()`，**不产生任何 hypercall** |
| 4 | `minSdk = 28` ≥ 26，宿主进程里 AAudio 无条件可用 | `app/build.gradle` | Android 侧真的是「host 直通」，零 shim |

顺带确认：仓库里目前**完全没有** AAudio / OpenSL ES / audio 相关痕迹（全仓库搜 `aaudio|opensl|SLES|audio` 无命中），是全新实现。

## 二、推荐方案

```
guest app (RISC-V ELF)
  └─ libAAudio API ──> vp_aaudio_stub.c   [新] stub：builder 状态机 + SPSC ring + callback 线程
                          │ ecall SYS_ANDROID_CALL(a0=SYS_ANDROID_AAUDIO_*)
                          ▼
                     vp_cmdpost.c           [改] 新增 case + cmdpost_set_audio_callbacks() vtable
                          ├─ Android host: vp_aaudio_android.c  [新] 直接调真 AAudio（直通）
                          └─ Win32  host: win32_aaudio_wasapi.c [新] WASAPI shared-mode shim
```

### 关键设计点

**1. 数据通路：ring buffer 归 guest 所有，host 只持指针**
guest stub 在 `openStream` 时按 `bufferCapacityInFrames`（默认 2~3×burst）在自己的堆上分配 SPSC ring（直接复刻 `vp_sensor_ringbuf.h`，新建 `include/virtpass/vp_audio_ringbuf.h`，改成按帧而非按事件），把指针 + drain fd 一起在 OPEN 时交给 host。
- 播放（host 是消费者）：guest 写满 → `poll(drain_fd)` 等 host 消费 → 继续写。
- 采集（host 是生产者）：对称。
- **稳态零 hypercall**：只有 OPEN/START/PAUSE/STOP/FLUSH/关闭才进 host。

**2. 两种 write 语义**
- 阻塞/写模式（`AAudioStream_write`）：上面那条路径，host 每消费一个 burst 往 drain fd 写一次（完全对标 `vp_cmdpost_vsync_tick` 的 payload 语义，用负值表示「设备丢了」）。
- **callback 模式（必须支持，Oboe/SDL3 默认走这条路）**：`setDataCallback` 的 fp **只留在 guest**，stub 起一个 guest pthread，循环：调 app callback 填 guest buffer → 写 ring → 阻塞等 drain。因为事实 #2，这是安全的；且天然满足「callback 不在 UI 线程」的语义。
- 备选简单版（若想先跑通）：host 持有 ring，`AAUDIO_WRITE` hypercall 里 memcpy 一次（1 次 syscall + 1 次拷贝/块）。可作为 fallback，但不推荐做主线。

**3. ABI / 命令号（`include/virtpass/vp_android.h` + `vp_cmdpost.c` 两处同步）**
现在已用 `BASE+1..14`、`+20..28`；注意 `SYS_ANDROID_CALL = BASE+34`，子命令建议避开 34 用一整块：

```c
#define SYS_ANDROID_AAUDIO_OPEN        (SYS_ANDROID_BASE + 40)
#define SYS_ANDROID_AAUDIO_CLOSE       (SYS_ANDROID_BASE + 41)
#define SYS_ANDROID_AAUDIO_START       (SYS_ANDROID_BASE + 42)
#define SYS_ANDROID_AAUDIO_PAUSE       (SYS_ANDROID_BASE + 43)
#define SYS_ANDROID_AAUDIO_STOP        (SYS_ANDROID_BASE + 44)
#define SYS_ANDROID_AAUDIO_FLUSH       (SYS_ANDROID_BASE + 45)
#define SYS_ANDROID_AAUDIO_READ        (SYS_ANDROID_BASE + 46)   /* 采集，Phase 2 */
#define SYS_ANDROID_AAUDIO_GET_INFO    (SYS_ANDROID_BASE + 47)   /* 回读实际 rate/format/ch/容量 */
#define SYS_ANDROID_AAUDIO_SET_BUFSIZE (SYS_ANDROID_BASE + 48)
#define SYS_ANDROID_AAUDIO_GET_TS      (SYS_ANDROID_BASE + 49)
```
`OPEN` 只传一个指针，指向 guest 栈上的 `vp_aaudio_config_t`（rate/ch/format/direction/sharing/perf/usage/bufferFrames/ring_ptr/ring_frames/drain_fd），host 直接读——这就是事实 #1 的红利，不用像 `gl_call` 那样逐参数 marshal。返回 handle（host 侧索引，非指针，避免泄漏 host 地址语义）。

**4. Android host 直通（`vp_aaudio_android.c`）**
把 vtable 实现成对真 AAudio 的直调：OPEN→`AAudioStreamBuilder_*`+`openStream`；host 侧用 **数据回调**（在 AAudio 线程里把 ring 拷进 AAudio 给的 buffer，再 `write`），或起一个 pump 线程用阻塞 `write`。**绝不让 AAudio 线程进 guest**。不需要改 Java 层，纯 NDK；只有录音要在 `AndroidManifest.xml` 加 `RECORD_AUDIO` + 运行时授权（Phase 2）。

**5. Windows WASAPI shim（`win32_aaudio_wasapi.c`）**
```
CoInitializeEx(MTA) → CoCreateInstance(MMDeviceEnumerator)
 → GetDefaultAudioEndpoint(eRender) → IAudioClient
 → Initialize(SHARED, EVENTCALLBACK|AUTOCONVERTPCM|SRC_DEFAULT_QUALITY,
               <直接用 guest 请求的 WAVEFORMATEXTENSIBLE>, bufDur, 0, hEvent)
 → GetService(IAudioRenderClient)
音频线程: WaitForSingleObject(hEvent) → GetCurrentPadding → GetBuffer → ring 取帧 → ReleaseBuffer
                                                    → write(drain_fd) 唤醒 guest
```
- **`AUTOCONVERTPCM | SRC_DEFAULT_QUALITY` 是关键**：可以直接用 guest 要求的采样率/声道/位深去 `Initialize()`（I16/I24/F32 都吃），WASAPI 引擎自己做重采样和格式转换 → **shim 里不需要写任何转换代码**，ring 里放的就是 guest 格式。
- 低延迟：Win10+ 优先 `IAudioClient3::InitializeSharedAudioStream` 拿 `GetSharedModeEnginePeriod` 的最小周期，失败则回落到 `Initialize()`——和 `win32_gl_backend.c` 里「先试高级路径再回落」的风格一致。
- 无设备/独占被占：OPEN 返回 `AAUDIO_ERROR_UNAVAILABLE`，同时照 `vp_cmdpost_vsync_source_lost()` 的模式往 drain fd 写 `-1`，让 guest 走静音降级而不是卡死（headless CI 必须有这条路）。
- `AvSetMmThreadCharacteristicsW(L"Pro Audio")` 提优先级（可选）。
- **链接方式沿用现有习惯**：`win32_gl_backend.c` 是用 `LoadLibraryA` + `GetProcAddress` 运行时加载 `libEGL/libGLESv2` 的。WASAPI 可以同样处理（`LoadLibraryA("ole32.dll")` 取 `CoInitializeEx/CoCreateInstance`，头文件前 `#define INITGUID` 自带 IID，连 `-luuid` 都省了）；若嫌麻烦就在 `Makefile` 加一个 `LDFLAGS_USE_* := $(call check_cc_flags,-lole32 -luuid -lavrt)` 的 useflag。**改动都落在 `project.mk` 的 `bin_src_rvvm_winhost` 列表里**（win32-host 目录下的 `CMakeLists.txt` 不是主构建路径）。

**6. guest 侧打包（`project.mk`）**
`vp_ndk_stub.c` 已经是 1200+ 行单体，AAudio 建议独立成 `src/virtpass/vp_aaudio_stub.c`，产出 `libaudio_stubs.a`，并加进 `ANDROID_GUEST_LIBS`（链接行补 `-laudio_stubs`）；同时加进 `lib_src_virtpass_guest`，这样 native riscv64 下的 `virtpass_stub` 目标也一起带上。
**必须注意**：stub 要导出与 NDK `<aaudio/AAudio.h>` **完全一致的符号名和 ABI**（`AAudioStream`/`AAudioStreamBuilder` 不透明指针、`aaudio_result_t` 枚举值、`AAudioStream_dataCallback` 签名），否则预编译 guest app 链不上。

## 三、风险与对策

| 风险 | 说明 | 对策 |
|---|---|---|
| 额外延迟 | guest ring + host 缓冲 = 双缓冲，比原生 AAudio 多 1~2 个 burst | 每次 open 默认容量压到 2×burst；`setBufferSizeInFrames` 允许下调（host 侧只改目标值，能生效的只有 WASAPI 侧，如实回读） |
| A/V 同步 | `AAudioStream_getTimestamp` 是 Oboe 的音频时钟，乱返回会导致画面抖动 | Phase 1 用 `CLOCK_MONOTONIC` + `framesWritten` 合成，标 `AAUDIO_OK`；Phase 2 用 WASAPI `IAudioClock2::GetDevicePosition` / `CLOCK_MONOTONIC` 或 AAudio 的 `getTimestamp` 回填 presentationTime |
| 录音/输入 | 需要权限、`AAudioStream_read` 路径 | Phase 2；Windows 走 `eCapture` + `IAudioCaptureClient`，Android 直接 AAudio input |
| 覆盖不到 OpenSL ES | SDL2 及大量老游戏只会调 OpenSL ES，AAudio 单独做救不了它们 | **建议把 vtable 设计成通用的 `stream + format + ring + drain_fd`**，OpenSL ES（CreateAudioPlayer/Enqueue/`SLAndroidSimpleBufferQueueItf`）在 guest 侧只是薄适配（它也是 push 模型），几乎零成本复用 |
| ring 生命周期 | guest 若在 stream 未 close 时 free/munmap ring，host 线程会踩坏内存 | 定死契约：`AAudioStream_close` 前不得释放；host 在 CLOSE 里必须 join 掉音频线程再返回 |
| host 线程退出顺序 | guest 线程可能先退出（`execve`/断点），音频线程要能收尾 | CLOSE 里同步 join；再加一个 `audio_source_lost` 兜底（对标 `vp_cmdpost_vsync_source_lost`） |

## 四、改动清单与分期

| 文件 | 动作 |
|---|---|
| `include/virtpass/vp_android.h` | 新增 `SYS_ANDROID_AAUDIO_*` + `vp_aaudio_config_t` |
| `include/virtpass/vp_audio_ringbuf.h` | 新建（改 `vp_sensor_ringbuf.h` 而来，按帧） |
| `src/virtpass/vp_aaudio_stub.c` | 新建：AAudio API 全量 stub + builder + callback 线程 |
| `src/virtpass/vp_cmdpost.c` / `.h` | 新增 10 个 case + `cmdpost_set_audio_callbacks()` |
| `src/virtpass/android-host/.../vp_aaudio_android.c` | 新建：真 AAudio 直通 |
| `src/virtpass/win32-host/win32_aaudio_wasapi.c` | 新建：WASAPI shim |
| `win32-host/win32_main.c` + `win32_cmdpost_bridge.c` | 各加几行 init/注册 |
| `project.mk` | 加 `vp_aaudio_stub.c`→`libaudio_stubs.a`、winhost 源码列表、可能的 `-lole32` |
| `guest-samples/test_audio.c` | 新建冒烟样例（正弦 440Hz，write 模式和 callback 模式各一段） |

**Phase 1（能听到声）**：命令号 + stub + 播放 write 路径 + Android 直通 + guest 样例 —— 工作量最小、能立刻验证。
**Phase 1.5**：Win32 WASAPI shim + 无设备降级。
**Phase 2**：callback 线程、`getTimestamp`、`setBufferSizeInFrames`、采集/录音。
**Phase 3**：OpenSL ES 适配层（复用同一 vtable）。

## 需要你确认的三点

1. `vp_aaudio_stub.c` 独立成文件 + 独立 archive，还是并到 `vp_ndk_stub.c` 里？（我建议独立）
2. 主目标 guest app 是走 AAudio（Oboe/SDL3）还是 OpenSL ES（SDL2/老游戏）？这决定要不要 Phase 3 提前——如果目标是后者，AAudio 单做基本听不到声。
3. Android 侧直通用「host 数据回调」还是「独立 pump 线程」？前者更贴近 AAudio 原生语义，后者更好调，我倾向先 pump 线程（Phase 1）+ 回调（Phase 2）。

确认后我可以直接从 Phase 1 开工。

注：本文为初期决策记录，不代表最新状态。