# fix：guest ioctl 请求号符号扩展 + pty fd 的 stat 身份

> 2026-11-18。只追加修复条目，不描述现状。
> 现象 → 根因 → 修法 → 验证。两处修复是同一条 pty-req 链路上的前后两关。

## 条目 1：ioctl 请求号被符号扩展，TIOCGPTN 永不命中

### 现象

`ssh -tt`（pty-req）到 tinysshd 必失败：客户端报 `PTY allocation request failed on channel 0`，
服务端 tinysshd.log 报 `unable to open terminal (not a tty)`（errno ENOTTY）。此前所有
ssh 会话都是无 pty 跑的，`echo` 类命令不受影响，所以从未暴露。

### 根因

musl 的 `ioctl(int req, ...)` 把请求当 **int** 传给 `syscall()`。`TIOCGPTN = 0x80045430`
高位为 1，作为 int 是负数，进 a1 寄存器时被符号扩展成 `0xFFFFFFFF80045430`。
模拟器的 ioctl 分发直接拿 64 位 a1 与 case 常量比较，永不命中，落到宿主
`ioctl()`（`posix_shim.c` 非 socket 一律 ENOTTY）。真内核按 `unsigned int cmd` 截断，
所以 Linux 上没这个问题。低位为 0 的请求（如 TIOCSPTLCK `0x40045431`）恰好不受影响，
于是 openpty 的前半段全绿、`TIOCGPTN` 单点失败——trace 上表现为同一 fd 先成功后
ENOTTY，极具迷惑性。

### 修法

`rvvm_user.c` case 29：先 `uint32_t req = (uint32_t)a1` 再分发（console / pty / 宿主三条
路径都用它）。这是内核 ABI 的忠实翻译，不是兼容层。

### 验证

探针（`rvvm_info`，已回退）实证分发实参 `cmd=ffffffff80045430`；修复后 `ssh -tt`
`echo/tty/stty size` 全通。见条目 2 的验证一节。

## 条目 2：pty fd 的 fstat 与路径 stat 身份不一致，ttyname 判 ENODEV

### 现象

条目 1 修掉后 pty-req 仍失败，但失败点前移：musl `openpty` 已完成
open(ptmx) → TIOCSPTLCK → TIOCGPTN → open(/dev/pts/N)，挂在 `ttyname(slave)` 上。

### 根因

musl `ttyname_r`：readlink `/proc/self/fd/N` 得到名字后，`stat(名字)` 与 `fstat(fd)`
比对 `st_dev` 与 `st_ino`。路径 stat 由设备表回答（`st_dev=RVVM_SHADOW_DEV`、
`st_ino=userland_dev_ino("/dev/pts/N")`），而 fd 侧的 `userland_pty_fill_stat`
memset 清零后只填 mode/rdev——两组身份必然不等，musl 返 ENODEV。

### 修法

`userland_pty_fill_stat` 补齐身份：master 填 `/dev/ptmx` 的、slave 填 `/dev/pts/<index>`
的 `st_dev`/`st_ino`，与同名路径 stat 严格一致（`userland_dev_ino` 前置声明）。

### 验证

`ssh -tt` 到 core（7930）：`PTY-OK`、`tty` → `/dev/pts/0`、`stty size` → `24 80`；
supervisor 起 daemon 实例经 ws 桥跑 `ws_smoke.py`：`WS-BRIDGE-OK`（pty 回显 + 行规程 +
算术求值）。回归：`fs_selftest.ps1` PASS（90/0）；`openssh_e2e.ps1` FAIL 17、
`sshd_crash_e2e.ps1` FAIL 4——与 stash 基线逐项一致（基线条目见 issues/INDEX
`[tools][e2e][env]`），其「core survived 6 sshd sessions」检查通过。
