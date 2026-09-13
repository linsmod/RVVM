handover_clear_and_clean_sensor.md


# Virtpass 传感器路径：clean design

先立约束，再推结构。以下设计的所有决策都能回溯到一条约束，不引入任何"因为现在这么写"的理由。

## 0. 六条约束（设计公理）

| # | 约束 | 推论 |
|---|---|---|
| C1 | NDK ABI 严格对齐（项目规则），可用子集 | `vp_android.h` 里只允许出现 NDK 有的东西，签名逐字对齐 |
| C2 | guest 内存是 rvvm_user 私有缓冲，**无共享内存 ABI** | 所有数据只能经 `rvvm_user_guest_ptr()` copy-in/copy-out |
| C3 | NDK 的 opaque 对象必须保持 opaque，且要有真实宿主生命周期 | 不许用全局单例冒充 `ASensorEventQueue` |
| C4 | 设备事实归 host 所有 | guest stub 不得硬编码 `g_sensors[]`；但 NDK accessor 是**本地**的 → 枚举一次、本地读 |
| C5 | 生产者线程 ≠ 消费者线程，必须显式建模 | 一个锁 + 明确定义的唤醒边沿规则 |
| C6 | host 能力不同（Android 真传感器 / win32 桩 / 无） | 能力位协商，guest 确定性降级 |

C2 是回答上一轮问题的地方：**跨线程交接缓冲是 C5 的必然产物，它属于 host 实现，不属于 ABI。**

## 1. 分层与文件归属

```
include/virtpass/vp_android.h      ①  guest 可见的 NDK 面（opaque + NDK 签名）
include/virtpass/vp_sensor_abi.h   ②  传输层（子命令 / cap / 错误码 / wire struct）  ← 新增
src/virtpass/vp_sensor.c/.h        ③  host 传感器子系统（描述符 + 队列表 + FIFO）      ← 新增
src/virtpass/vp_cmdpost.c          ③  dispatch 只做转发（薄）
src/virtpass/vp_ndk_stub.c         ④  guest stub
src/virtpass/android-host/...      ⑤  Android 适配器（直接用平台 NDK 传感器 API）
src/virtpass/win32-host/...        ⑤  win32 适配器（桩源）
include/virtpass/vp_sensor_ringbuf.h   ✗ 删除
```

**删除清单**（都是当前设计的病征，不是附带清理）：

- `vp_sensor_ringbuf.h` 整文件（C2 违反物）
- `vp_cmdpost.h`：`sensor_init/enable/data_callback`、`cmdpost_set_sensor_callbacks`、`cmdpost_init_sensor_ringbuf`、`cmdpost_push_sensor_event`、`cmdpost_ASensorEvent`
- `vp_cmdpost.c`：`g_sensor_ringbuf`、`g_sensors_enabled`、`g_sensor_*_cb`、`sensor_ringbuf` include
- `vp_ndk_stub.c`：`g_sensors[8]` 硬编码表、`g_sensor_queue` 单例
- 文档：`win32-host/README.md` 的 "guest pops the ringbuf itself"

## 2. ① guest ABI（`vp_android.h`）

**保持 opaque**，三类签名对齐问题逐条修：

```c
/* 现在写成 void* callback —— 违反 C1。NDK 是 ALooper_callbackFunc。 */
ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager,
        ALooper* looper, int ident, ALooper_callbackFunc callback, void* data);
```

注意 `ALooper_callbackFunc` 目前定义在文件底部（Looper 段），需上移或前向 typedef。

**补齐 `ASensor` 类的缺口**：`ASensor_getMaxDelay` / `ASensor_getMaxRange` / `ASensor_getPower` 目前缺失（子集允许，但既然是 sensor 类收口，就补全）。

**Direct Channel 一律不声明**（`createHardwareBufferDirectChannel` / `configureDirectReport` / `createSharedMemoryDirectChannel` 保持缺席）。只保留 `isDirectChannelTypeSupported` / `getHighestDirectReportRateLevel` 声明且恒返回 false / 0 —— 这是 C1 允许的"子集"，同时不给 guest 任何虚假期待。

## 3. ② 传输层（`vp_sensor_abi.h`）—— 只放线协议

命名对齐 `vp_audio_ringbuf.h` 的**内容模型**（该文件自身注释就写着 "wire protocol only / No shared ring buffer"），但不再用 `*ringbuf*` 这种误导性名字。

**子命令独立成块**。现在 sensor 号 1..4 和 window/input/game 交错，不可读；按 audio 的先例（BASE+40..51）给 sensor 一个连续窗口：

```c
#define VP_SENSOR_SYS_BASE   (0x10000 + 60)   /* 1..4 退役，一次性重编号 */

VP_SENSOR_MANAGER_INIT      /* ret = VP_SENSOR_CAP_* 位图 */
VP_SENSOR_LIST              /* a1=guest vp_sensor_info_t[], a2=max, ret=n */
VP_SENSOR_DEFAULT           /* a1=type, ret=handle 或 -1 */
VP_SENSOR_QUEUE_CREATE      /* a1=guest 唤醒写端 fd(可 -1), a2=looper 存在?, ret=queue_id */
VP_SENSOR_QUEUE_DESTROY     /* a1=queue_id */
VP_SENSOR_QUEUE_ENABLE      /* a1=queue_id, a2=handle, a3=0/1 */
VP_SENSOR_QUEUE_DISABLE     /* 同上 */
VP_SENSOR_QUEUE_SET_RATE    /* a1=queue_id, a2=handle, a3=period_us */
VP_SENSOR_QUEUE_REGISTER    /* a1=id, a2=handle, a3=period_us, a4=max_batch_us */
VP_SENSOR_QUEUE_HAS         /* a1=queue_id, ret=0/1 */
VP_SENSOR_QUEUE_READ        /* a1=queue_id, a2=guest vp_sensor_event_t*, a3=count, ret=写入数 */
```

**wire event 只定义一次**，guest 侧 `ASensorEvent` 是它的别名（保证字节同一）：

```c
typedef struct vp_sensor_event {
    int32_t version, sensor, type, reserved0;   /* sensor = handle（NDK 语义） */
    int64_t timestamp;                          /* ns */
    union {                                     /* 64 字节，与 NDK 同 */
        float data[16];
        struct { float x, y, z; float pad[13]; } vector, acceleration, magnetic;
        struct { float azimuth, pitch, roll; float pad[13]; } orientation;
        float light, pressure, temperature, distance, relative_humidity;
    };
    uint32_t flags;                             /* SENSOR_FLAG_* */
    int32_t  reserved1[3];
} vp_sensor_event_t;   /* sizeof == 104, align 8 */
```

补 `acceleration` / `magnetic` / `light` 等命名成员是**免费**的（union 尺寸不变）且显著提升源码兼容性——现在只给 `vector`/`orientation`，guest 写 `event.acceleration.x` 编不过。

加编译期断言（两侧各一份）：

```c
_Static_assert(sizeof(vp_sensor_event_t) == 104, "ASensorEvent ABI");
_Static_assert(offsetof(vp_sensor_event_t, timestamp) == 16, "");
_Static_assert(offsetof(vp_sensor_event_t, flags) == 80, "");
```

`vp_sensor_info_t` 承载描述符（`handle/type/reporting_mode/min_delay/max_delay/resolution/max_range/power_ma/fifo_max_events/fifo_reserved_events/wake_up/string_type[64]/name[64]/vendor[64]`）。fifo 计数在这里公布，是 C7 的诚实性落点。

错误码沿用 audio 的形态：`VP_SENSOR_OK / _ERROR_INVALID_ARG / _UNSUPPORTED / _NO_MEMORY / _CLOSED`。

cap 位：`CAP_LIST | CAP_RATE | CAP_WAKEUP | CAP_FD_WAKEUP`。`CAP_DIRECT_CH` **不设**（未实现就不声明）。

## 4. ③ host 子系统（`vp_sensor.c`）—— 唯一持有状态的地方

**控制面 = 注册的 ops 表**（照抄已经正确的 `vp_audio_ops_t` 形态）；**数据面 = cmdpost 自有的 ingest 入口**。这个反转是本次设计的核心修正：

```c
typedef struct vp_sensor_ops {
    int32_t  (*enumerate)(vp_sensor_info_t* out, int32_t max);          /* 设备事实来源 */
    int32_t  (*set_enabled)(int32_t handle, bool enable);               /* 起停平台源 */
    int32_t  (*set_rate)(int32_t handle, int32_t period_us, int32_t max_batch_us);
    uint32_t (*query)(void);                                            /* CAP_* */
} vp_sensor_ops_t;
void cmdpost_set_sensor_ops(const vp_sensor_ops_t* ops);

/* 生产者入口：host 从自己的传感器线程调用，线程无关。 */
void vp_sensor_ingest(int32_t handle, const vp_sensor_event_t* ev);
```

现在的 `sensor_data_callback`（host→cmdpost 送数据）方向是反的，而且 **Android 从来没注册过它**（`cmdpost_set_sensor_callbacks` 全仓只有 win32 一处调用）——C3/C5 违反。反转后：数据由 host 主动 push 进来，控制由 cmdpost 回调出去，和 AAudio 完全同构。

**队列表**（复刻 AAudio 的 slot 表做法，仓库已有先例）：

```c
typedef struct {
    bool     used;
    int      wake_fd;          /* guest 的唤醒写端；-1 = 无 */
    uint32_t enabled_mask;     /* 按 dense handle 位图 */
    int32_t  rate_us[VP_SENSOR_MAX_HANDLES];
    vp_sensor_event_t fifo[VP_SENSOR_FIFO_MAX_EVENTS];   /* 64，power of 2 */
    uint32_t head, tail;
    uint64_t dropped;          /* 溢出计数，可经 QUEUE_INFO 暴露 */
    bool     wake_armed;       /* 见下 */
} vp_sensor_queue_t;
static vp_sensor_queue_t g_queues[VP_SENSOR_MAX_QUEUES];
static vp_lock_t g_sensor_lock;   /* 一个锁，见 §7 */
```

**唤醒边沿规则（必须写进注释，这是"clear"的关键）**：

> `wake_armed` 在队列由空变为非空时置位，并向 `wake_fd` 写 1 字节；在 `QUEUE_READ` 把队列读空时清除。即"空→非空"沿触发一次，"读空"重新武装。

这条规则同时保证：不丢唤醒、不风暴、不需要每次事件都 write。

**溢出策略**：满时**丢最旧**（保留最新），因为 Android HAL 批 FIFO 就是覆盖最旧；`dropped++` 计数。禁止"丢最新"以外的第三种模糊行为，并写进 `vp_sensor_abi.h` 注释。

**handle 语义**：host 的 `enumerate` 返回**稠密本地索引**（0..N-1）作为 Virtpass handle，私有维护 index → `const ASensor*` 映射。数据里 `sensor` 字段填这个索引，`type` 填类型。这样 `enabled_mask` 能做位图，handle 稳定，且对 guest 完全不透明。**现在 `jni_bridge.c` 把 `sensor = type`、win32 干脆不填 `sensor`**，两 host 不一致——这是 C1 语义对齐的实质 bug。

## 5. ④ guest stub（`vp_ndk_stub.c`）

- `ASensorManager` 保持单例 id=0（NDK 本身就是按包单例，合理）；首次 `getInstance` 发一次 `MANAGER_INIT`。
- `struct ASensor` 私有结构 + `g_sensor_pool[VP_SENSOR_MAX_HANDLES]`，由 `SENSOR_LIST` / `SENSOR_DEFAULT` 填充。**accessor 全部本地读**，零 syscall，与 NDK 行为一致（C4 的落点：事实来自 host，读取是本地缓存）。
- `struct ASensorEventQueue` 池化；`createEventQueue` 里 `pipe()` 出一对 fd，把**写端**通过 `QUEUE_CREATE` 交给 host，把**读端**用既有的 `ALooper_addFd(looper, fd, ident, ALOOPER_EVENT_INPUT, callback, data)` 注册进 Looper。
  - 这是已有且已经验证过的模式（Choreographer 的 `SET_FD` 就是这么干的），所以 guest 能写出完全真实的 NDK 惯用法：
    ```c
    while (running) ALooper_pollAll(-1, NULL, NULL, NULL);   /* 被传感器/vsync 唤醒 */
    ```
  - stub 内部的 fd 处理器**必须先排空唤醒字节**再调用户 callback / 返回 ident，否则 `poll` 会自旋。
- `getEvents` → `QUEUE_READ`；`hasEvents` → `QUEUE_HAS`；`setEventRate` → `QUEUE_SET_RATE`（现在整段是 no-op，C1 下的功能空洞）。
- **降级**：host 未置 `CAP_FD_WAKEUP` 时不建 pipe，guest 只能靠 `hasEvents` 轮询——与 vsync 的 `VP_VSYNC_CAP_FD_WAKEUP` 降级叙事完全同构。`CAP_* == 0` 时 `getSensorList` 返回 0、`getDefaultSensor` 返回 NULL（真机上没有该传感器的行为），而不是伪造一个 stub 传感器。

## 6. ⑤ 两个 host 适配器

**Android**：新 `vp_sensor_android.c` 直接用平台的 `ASensorManager` / `ASensorEventQueue`（host 就是 Android，和 `android_gl_host.c` 直接用系统 `libEGL.so` 同一策略）：

- `enumerate` ← `getSensorList` + `ASensor_getHandle/…`，handle 换成稠密索引
- `set_enabled` / `set_rate` ← `enableSensor` / `setEventRate`
- 生产端：自己的 looper 线程 + `createEventQueue(..., ALOOPER_POLL_CALLBACK, on_events, NULL)`，在回调里 `getEvents` 后逐个 `vp_sensor_ingest()`。**平台队列自己就是那个 FIFO**，不额外做缓冲。
- 随之**删除** `nativeEnableSensor/nativeDisableSensor/nativePollEvents/nativePushSensorData` 与 Java 侧的 `registerListener`——现在这套是"Java listener 生效、native 队列无人排空"的双份记账，正是本轮要消灭的病灶。
- 唯一需要产品决策的点：`MainActivity.sensorDataText` 的 UI 读数。若要保留，让 `onSensorChanged` 只更新 UI、不再调 native；否则一并删。

**win32**：新 `vp_sensor_stub_win32.c` 实现同一 `vp_sensor_ops_t`，桩传感器（accel/gyro/light）由定时器按 `set_rate` 的周期驱动，tick 时直接 `vp_sensor_ingest()`。`query()` 返回 `CAP_LIST | CAP_RATE`。它和 Android 的区别只在 ops 表背后——这正是 README 里"两个 host 实现同一 callback ABI，guest 不用改"的承诺被兑现的地方。

## 7. 线程与生命周期规则（显式写下，不留隐式约定）

- **单锁**：一个 `vp_lock_t` 保护队列表 + 所有 FIFO。不做 lock-free——生产者是 JNI/UI 线程，JNI 开销远大于一次加锁，原子操作零收益却把正确性成本拉满。新增 `src/virtpass/vp_lock.h`（`_WIN32` → `CRITICAL_SECTION`，否则 `pthread_mutex_t`），因为 host 两侧 RTOS 不同且仓库已有 `posix_shim.c` 的先例。
- **锁不跨 guest 边界**：`QUEUE_READ` 持锁取出、`memcpy` 到 `rvvm_user_guest_ptr(a2)` 后释放；任何情况下不在持锁时回调 guest。
- **destroy 时机**：`QUEUE_DESTROY` 可在任意时刻到达，之后该 id 立即失效（`VP_SENSOR_ERROR_INVALID_ARG`）。guest stub 必须先 `ALooper_removeFd` 再 destroy。
- **teardown 后 ingest 必须安全**：`cmdpost_cleanup()` 清 ops、销毁队列；之后 `vp_sensor_ingest()` 直接丢弃。这是 win32 那段"重启 guest 会探测到死代理"注释的同类要求。
- **`getInstance` 幂等**，`MANAGER_INIT` 只发一次。

## 8. 一致性校验（防止旧 bug 复现）

| 手段 | 拦截的问题 |
|---|---|
| `_Static_assert` on wire event（两侧各一份） | 结构漂移 |
| 重写 `test_sensor_guest.c` 为真 NDK 惯用法：枚举 → 50Hz 使能 accel → `postFrame`+`pollAll(-1)` 循环 → 断言 `event.sensor == ASensor_getHandle(accel)` 且 `event.type == ASENSOR_TYPE_ACCELEROMETER`、时间戳单调 | 上一轮发现的"读未初始化数组"和 `sensor`/`type` 语义错乱 |
| 溢出用例：最高速率使能、长时间不读、验证 `fifo_max_events` 上界与 `read` 返回最新样本、`dropped > 0` | 容量与策略失真 |
| 无 host ops 注册时全链路返回 `UNSUPPORTED`/空列表，guest 正常退出 | C6 降级路径 |

## 9. 迁移顺序（每步可独立验证）

1. 新增 `vp_sensor_abi.h` + `vp_lock.h`，只做定义，不动逻辑（编译通过即验证）。
2. 建 `vp_sensor.c`：ops 表 + 队列表 + `ingest` + 唤醒规则；`cmdpost_dispatch` 的 `SENSOR_*` 改为薄转发。
3. 改 guest stub：删硬编码表 → 描述符池 + 队列池 + Looper fd 接入。
4. 接 Android 适配器，删 Java/native 双份记账。
5. 接 win32 适配器。
6. 删 `vp_sensor_ringbuf.h`，清 `vp_cmdpost.{c,h}` 残留，改文档。
7. 重写一致性用例，跑双 host。

---

一句话概括这套设计的取向：**guest 侧只留 NDK 的对象与签名（opaque + copy-out），host 侧把"设备事实、队列身份、跨线程缓冲、唤醒边沿"四件事收进一个带锁的子系统，其余全部删除。** 现在那份 `vp_sensor_ringbuf.h` 之所以碍眼，不是因为它是个环形缓冲，而是因为它同时泄露了 ABI、伪造了共享内存、且没有任何调用者。

要我按这个顺序落地吗？建议从第 1、2 步开始（纯 host 侧，不改 guest 行为，可单独编译验证）。

USER: MainActivity.sensorDataText 的 UI 读数不用保留。

