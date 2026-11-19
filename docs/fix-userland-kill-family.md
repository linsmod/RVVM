# fix: kill(2) 只能看见自己 address space 的进程——`timeout` 的看门狗杀不掉命令

最后核对：2026-11-19。

## 现象

guest 里 `timeout 2 sleep 10` 等满 10 秒才返回（`rc=0`），`timeout 1 sleep 3` 等 3 秒；
busybox 的 `timeout` 在真 Linux 上 2 秒就杀掉。同源症状：

```
$ echo $$ $PPID      # 144 141
$ kill -0 $PPID      # 1（ESRCH），但 ps 里 141 明明在跑
$ sh -c '... kill -TERM $PPID ...'   # sh: can't kill pid 141: No such process
```

即：**子进程杀不了父/祖先进程**，只有自己的后代能杀。

## 根因（两层）

1. `userland_signal_pid()`（kill 的实现）用 `userland_proc_find(ctx, pid)` 查 pid——
   只查**调用者自己的 `ctx->procs` 表**。而本模拟器里每个 `fork()` 都是一个独立
   address space（独立 ctx），父进程的记录在父/祖父的注册表里，所以
   `kill(父pid)` 一律 ESRCH。分组信号（`kill(-pgid)`、终端的 ^C）走的是
   `userland_signal_group()` + `userland_family_root()`，所以只有单 pid 的 kill 坏。

2. 改成家族查找后还有一个坑：busybox `timeout` 的看门狗是**两次 fork** 出来的——
   中间进程退出，看门狗被 reparent。`userland_detach_family()` 在父 ctx 死亡时把
   子 ctx 的 `parent_ctx` 置 NULL，于是 `userland_family_root()` 只能走回它自己，
   家族退化成一个进程，`kill(0, 原祖先进程)` 仍然 ESRCH。这也解释了为什么
   `timeout` 的看门狗 1 秒后就悄悄退出：`timeout_wait()` 里 `kill(pid, 0)`
   失败被当成"命令已经结束了"，直接 return，不再发 SIGTERM。

## 修法

- `kill(pid>0)` 改用 `userland_proc_find_family()`（与 /proc、分组信号同一套家族遍历），
  并把信号投到记录所属的 ctx（`home`）。`tkill` 保持按本进程线程解析。
- ctx 增加 `family_root_ctx`：fork 时从父继承（父没有就取父自己），**不随
  `parent_ctx` 被 detach 清空**——Linux 上 reparent 到 init 仍在同一个 pid namespace。
  该 namespace 的根真正消失时（`userland_detach_records` 扫到）才清空，避免悬垂指针。

## 验证

```
timeout 2 sleep 10                              → rc=143, waited=2
kill -0 $PPID（普通会话）                        → 0
双重 fork 的孤儿 kill -0 原家族里的进程           → 0（修复前 1）
sleep 5 & p=$!; kill -TERM $p; wait $p          → rc=143
test_jobctl（直接跑，不接管道）                   → 仍是基线的 8 项 ctty FAIL，无新增
```

注意 `test_jobctl` 接 `| tail` 跑会多出 6 项失败（stdout 不是 tty，ctty 前提变了），
那是测试用法问题，不是回归。

另外此前的判断“`alarm(2)`/`setitimer` 不生效”是**错的**：`RVVM_TRACE=signal` 显示
`timeout` 根本没武装定时器（busybox 的 `timeout` 用 fork + `sleep 1` 轮询实现），
定时器本身的 arm/expire 路径是正常的。
