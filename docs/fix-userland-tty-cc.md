# fix: 空闲提示符下 Ctrl+C 无反应，连按两次会话卡死

最后核对：2026-11-19。涉及 `src/core/rvvm_user.c`（console tty 与 pty 的行规程、信号注入）、
`src/virtpass/guest-samples/test_jobctl.c`。

## 现象

supervisor web 终端（`ssh -tt` 进 guest）里，**没有输入命令时**按 Ctrl+C：

- 修复前：什么都没发生——没有 `^C`、没有换行、没有新提示符；
- 叠加缺陷：连按两次后整个会话不再响应（后续按键无回显、`echo` 也不执行），
  同时其它 ssh 连接连 hello 都收不到，core 看起来被卡死。

有命令在跑时（`sleep 40`）按 Ctrl+C 是正常的：pty 的 ISIG 分支吃掉字节、回显 `^C\r\n`、
把 SIGINT 发给前台进程组。

## 对照（不是推测）

同一份 busybox ash（1.30.1）在 **WSL2 的真实 Linux tty** 上，用 python `pty.fork()` 驱动：

| 条件 | 输出 |
|---|---|
| 默认（`c_cc[VINTR]=3`） | `$ \x1b[6n` → `^C\r\n$ \x1b[6n` |
| 启动前把 `c_cc[VINTR]` 设为 0 | 按 Ctrl+C 后**什么都没有**（与 RVVM 修复前一模一样） |

RVVM guest 里 `stty -a` 同时报 `intr = <undef>` 等所有控制字符未定义——即 TCGETS 报的
`c_cc` 全是 0。busybox 没问题，是 tty 模拟没把 `c_cc` 对上。

## 根因 1：TCGETS 没有报 c_cc，行规程也不认 guest 的控制字符

- `user_tty_ioctl()`（console）与 pty 的 `UAPI_TCGETS` 只填了 `c_cc[6]`（VMIN = 1），
  其余 31 个字节是 0；`TCSETS` 也只存 `c_lflag/c_iflag/c_oflag`，`stty intr ^X` 无处落地。
- 缺的正是 **raw 模式下唯一能用的那半套语义**：交互式行编辑（busybox ash 的 lineedit）会把
  终端切成 ICANON/ECHO/**ISIG 全关**，然后自己按字节认键——`libbb/lineedit.c` 的 `default`
  分支明确写着 `initial_settings.c_cc[VINTR] != 0 && ic_raw == initial_settings.c_cc[VINTR]`。
  VINTR 报 0 时 Ctrl+C 既不产生信号（ISIG 关着），也不匹配任何键，于是被当作不可打印字节丢掉。
- 同一处还硬编码了 `0x03/0x1a/0x7f/0x04` 当 VINTR/VSUSP/VERASE/VEOF，guest 用 `stty` 改键
  也不生效；`^C`/`^Z` 的回显记号也是写死的，`stty intr ^X` 之后仍回显 `^C`。

## 根因 2：连按两次卡死——`sig_inflight` 被 longjmp 永久占住

- `userland_siginject()` 在把 handler 压栈时置 `sig_inflight = 1`，只在 `rt_sigreturn`
  的还原路径（`userland_sigrestore()`）清零。
- busybox ash 的 SIGINT 处理器走 `raise_interrupt()` → `longjmp`，**永远不经过
  trampoline** 的 `rt_sigreturn`。于是第一次 ^C 之后 `sig_inflight` 永久为 1。
- 第二次 ^C：ash 再次 `raise(SIGINT)`，`sig_pending` 置位但注入被 `sig_inflight` 挡下，
  **永远送不出去**；而 `rvvm_sys_poll_time32()` 只要看到 `sig_pending` 就返回 EINTR，
  shell 的 `poll(stdin)` 于是每次都立刻 EINTR，空转不再读键盘——会话“卡死”，CPU 被吃掉。
- Linux 上这件事不成立：handler 被 longjmp 跳过 sigreturn 后，该信号在 mask 里仍被挡，
  真正解除它的是 guest 自己的 `rt_sigprocmask(SIG_UNBLOCK, …)`（ash 的 `INT_ON`，
  包着每一次 `read_line_input()`）。

## 修法

1. `c_cc` 变成真状态（`tty_cc_init()`，asm-generic 默认值：VINTR ^C、VQUIT ^\、
   VERASE DEL、VKILL ^U、VEOF ^D、VMIN 1、VSTART ^Q、VSTOP ^S、VSUSP ^Z、VREPRINT ^R、
   VDISCARD ^O、VWERASE ^W、VLNEXT ^V）：
   - console 用 `ctx->tty_cc[32]`，pty 用 `pty->cc[32]`；
   - TCGETS 原样回填，TCSETS 原样保存；
   - 两条行规程都改用 guest 的 `VINTR / VSUSP / VERASE / VEOF`，值为 0 表示
     `_POSIX_VDISABLE`（该键就是普通数据），与 Linux 一致。
2. `tty_caret_note()`：`^C`/`^Z` 的回显按实际键位生成（`^X` 就回显 `^X`，DEL 记作 `^?`）。
3. 信号侧记下 `sig_inflight_sig`；`rt_sigprocmask` 里若 guest 正在 **解除** 这个信号
   （`SIG_UNBLOCK` 命中它，或 `SIG_SETMASK` 不再包含它），就清 `sig_inflight`——这正是
   Linux 上被 longjmp 跳过后 mask 由 guest 自己摆正的那一步，之后待投递的信号照常注入。

## 验证

- `rvvm_winhost --app test_jobctl`（新增 `test_pty_cc` 段，共 10 项新检查）：
  `TCGETS reports the control characters…`、`TCGETS reads the changed VINTR back`、
  `with VINTR = ^X the ^C byte is data…`、`the line discipline echoes the new notation`、
  `and ^X is what kills the job (128+SIGINT)` 全 `ok`；其余非 ctty 项全 `ok`。
- 真实会话（`--serve` + `ssh -tt`）：空闲提示符连按 **5 次** ^C，每次都得到
  `^C` + 换行 + 新提示符，随后 `echo alive-$((70+7))` 正常输出 `alive-77`（修复前第 3 次起无任何输出）。
- WSL2 对照脚本见上文表格，行为分界只取决于 `c_cc[VINTR]`。

## 已知差异（已登记 issues）

- 每次 ^C 比 Linux **多一个空行**：`rvvm_sys_poll_time32()` 见到 `sig_pending` 就返回 EINTR，
  而注入在系统调用返回之后才发生，ash 因此先走了一次 `errno == EINTR` 分支并写 `"\n"`。
  属观感问题，不影响功能。
- `test_jobctl` 的 ctty 段 8 项在**改动前的基线**上同样 FAIL（`release.windows.x86_64` 原样二进制
  逐项相同），与本修复无关。
