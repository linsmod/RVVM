# 修复：sigaltstack 参数错位，faulthandler 启动时把 24 字节零写进 libc 的 .rodata

> 修复史。现象、根因、修法、验证。现状见 `arch-userland.md`。

## 1. 现象

`python3 -X faulthandler -c "import _ctypes"` 必定非法指令退出（132），
不带 `-X faulthandler` 时同一个 import 正常。崩溃现场：

```
Exception 2 (tval 0) at PC 8493508, SP 1fffebb0
  X12 = 8493508  X14 = ffffff04  X15 = 8493508
  PC 8493508 -> @ Interpreter, reloc 93508
```

`tval 0` 说明取到并执行的字就是 `00000000`。`0x8493508` 是解释器
（`ld-musl-riscv64.so.1`）基址 `0x8400000` 加 `0x93508`，落在 `.rodata` 里。

崩溃转储中 `PC-16` 起的 32 字节与磁盘文件对比：

| 地址 | 文件内容 | 崩溃进程内存 |
|---|---|---|
| 0x93508..0x93513 | `043df8ff 0a3cf8ff fe3cf8ff` | **全零** |
| 0x93514..0x93517 | `143cf8ff` | `143cf8ff`（未变） |
| 0x93518..0x9351f | `183cf8ff 223cf8ff` | **全零** |

即：20 个零字节，中间 4 字节原样保留。这正好是
`struct uapi_sigaltstack { uint64_t sp; int32_t flags; /* 4 字节 padding */ uint64_t size; }`
写入 `sp=0, flags=0, size=0` 的字节图案（padding 不参与赋值）。

`0x93508` 是 musl `sysconf()` 的跳转表起点，表项是相对表基址的 32 位偏移；
`sysconf(_SC_PAGESIZE=30)` 经 map 表得到 index 4，读 `table[4]` 作为偏移。
表项被清零后 `base + 0 = 0x8493508`，于是 `jr a5` 跳进表头（数据）取指，
tval 为 0。

`libffi` 在 `dlopen` 时调用 `sysconf(_SC_PAGESIZE)`，所以触发点在
`import _ctypes`（它链接 libffi）。

## 2. 根因

`rt_sigaction` 之后的 `sigaltstack`（syscall 132）把参数读成了 **a1/a2**，
而这颗 ABI 上 `int sigaltstack(const stack_t *ss, stack_t *old_ss)` 的参数是
**a0/a1**：

```c
/* 错 */
ss  = a1 ? to_ptr_sz(a1, ...) : NULL;
old = a2 ? to_ptr_sz_wr(a2, ...) : NULL;
```

CPython 的 faulthandler 在启动时调用 `sigaltstack(&stack, NULL)`：

- `a0 = &stack`，`a1 = 0`，`a2` 是调用者留下的垃圾（不是这个二参数调用的入参）。
- 错位后：新栈被当成 NULL（**备用栈从未真正安装**），而 `old` 指针取到了
  `a2`，于是把一份 `sp/flags/size` 全零的 24 字节结构写到了 `a2` 指向的
  guest 地址。
- 这次 `a2` 恰好是一个指向解释器 `.rodata` 的指针（`0x8493508`），
  一次 `sigaltstack` 调用就零掉了 `sysconf` 跳转表的 5 个表项。

这也解释了为什么只有 `-X faulthandler` 会崩：只有它会在这个时机调用
`sigaltstack`；不带 faulthandler 的进程里同一段 `.rodata` 与磁盘一致
（同一镜像内用 `ctypes` 直接读 `base+0x93508` 验证过）。

## 3. 修法

`src/core/rvvm_user.c`：

1. syscall 132 改为按 ABI 读 `a0`/`a1`，`old_ss` 只在 `a1` 非空时写。
2. `guest_vm_init()` 重置备用栈状态为 `sp=0, size=0, flags=SS_DISABLE,
   on=false`。这样：
   - 新进程首次 `sigaltstack(NULL, &old)` 得到 `SS_DISABLE`（之前报 0）；
   - `execve()` 随地址空间一起丢掉旧备用栈，符合 Linux 语义。

修完 faulthandler 才第一次真正把栈装进 `ctx->altstack_sp/size`，
`SA_ONSTACK` 的帧才会落在备用栈上（以前的“已验证”只证明了 syscall 返回 0）。

## 4. 验证

本机 Windows x86_64 宿主，`release.windows.x86_64` 树，guest 为 Alpine riscv64：

1. guest C 探针（zig 交叉编译，`-target riscv64-linux-musl`）：
   - `sigaltstack(&ss, &old)` → 0，`old.ss_flags == SS_DISABLE`；
   - 查询回读 `ss_sp` / `ss_size` 一致；
   - 64 字节的小栈 → `-1 / ENOMEM`；
   - `SS_DISABLE` 后再查询 `SS_DISABLE` 置位；
   - 安装 `SA_SIGINFO|SA_ONSTACK` 的 SIGILL handler，执行 `.word 0`：
     handler 内 `SP` 在 `[altstack, altstack+size)` 内（`on_alt=1`），
     `si_signo=4`，`siginfo`/`ucontext` 非空，`_exit(0)`。
2. `python3 -X faulthandler /probe_fixed.py`：`import ctypes` 成功，
   `base+0x93508` 的 32 字节与磁盘一致（`043df8ff0a3cf8ff...`）。
3. faulthandler 真崩溃路径仍正常：`ctypes` 调用地址 2 触发
   `Fatal Python error: Illegal instruction` 并打印 Python traceback，
   退出码 132。
4. `test_std`：159 checks / 1 failed / 7 skipped，唯一失败是既有的
   `a dup(2) of stdout keeps its /proc link name` 基线。
5. `tools/supervisor/selftest.py --release release.windows.x86_64`：
   `=== PASS: supervisor selftest (0 failure(s)) ===`。

## 5. 结论与遗留

- 原 `issues/INDEX.md` 中 `python3 -X faulthandler import _ctypes` SIGILL 条目
  由此结案；之前登记的“`_ctypes.so` 重定位/GOT”方向是错的，
  与动态链接器无关。
- 顺带修正：此前 `sigaltstack` 的“验证”只覆盖了返回值，没覆盖栈是否真的被使用；
  本次 C 探针把 `SA_ONSTACK` 帧位置也纳入验证。
- 观察到但不在本条范围内的行为：guest 对地址 0 的**写**（`sw zero, 0(zero)`）
  不产生 trap（被当作无 MMIO 的访问丢弃），因此不能用 NULL 写来测 SIGSEGV；
  需要真异常时用非法指令。已登记到 `issues/INDEX.md`（`[userland] [memory]`）。
