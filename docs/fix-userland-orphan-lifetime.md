# fix: 孤儿进程从 /proc 消失、kill 后不收尸、崩溃进程父 wait 卡死——进程生命周期三处缺口

最后核对：2026-11-20。

## 现象

三种症状，同一个区域（`rvvm_process_t` 的注册与家族遍历）：

1. **孤儿进程不可见、不可杀**。guest 里 detached 起一个进程（`setsid sleep 300 &`，
   或 supervisor 用 `setsid` 起的 mockserver），等启动它的那个 ssh shell 退出后：

   ```
   $ setsid sleep 300 &          # 老会话
   $ ls /proc                    # 新会话：没有它
   $ ps                          # 没有它
   $ kill 106                    # sh: can't kill pid 106: No such process
   $ wget -qO- http://127.0.0.1:8899/   # 但它还在服务
   ```

   于是 `deploy --restart` 的 `/proc/<pid>/comm` 扫不到旧进程 → 杀不掉 → 新进程
   `bind` 失败退出，旧进程继续服务，看起来像"部署成功却还是旧二进制"。

2. **被杀的孤儿永久 `Z`**。一旦能杀到它（见修法），`/proc/<pid>` 仍在，`stat` 是
   `Z`，`ps` 也一直列着，永不消失。

3. **异常崩溃把父进程卡死**。guest 里执行一条非法指令：

   ```
   $ /tmp/deep; echo $?          # 修复前：60 秒后 exec 超时，没有输出
   ```

   崩溃进程的父 shell 永远等不到 `wait4`，`us exec` 只能超时；`/proc` 里留一个
   "运行中" 的进程。同一次崩溃还会把回溯刷满日志：同一帧重复几千次（实测某轮
   `runtime/supervisor/7949.log` 长到 190 MB），且 `Instruction bytes` 用 `printf`
   直写 stdout，和其它行走的不是同一个通道，transcript 顺序错乱。

## 根因

- **/proc 的枚举路径**：`userland_proc_snapshot_walk()` 从 `userland_family_root()`
  出发，沿每条记录的 `child_ctx` 递归。fork 出来的进程，**唯一指向它 address space
  的那条记录在父 ctx 的 registry 里**；父 ctx 销毁时记录一起释放，孤儿仍在运行但
  从家族树上掉了下来。Linux 的做法是 reparent 到 init。
- **kill 与 exit 的收尸路径不一致**：`userland_exit_process()`（guest 自己 exit）在
  "没有活着的父进程"时会 `userland_proc_forget()`；`userland_kill_process()` 只
  `userland_proc_exit()` + `userland_finish_threads()`，没有这一步，所以记录永远留着。
- **traps 只结束了线程**：vCPU 循环的 `Boom!` 分支打印完寄存器与回溯就 `break`，
  既没有 `userland_proc_exit()` 也没有 `userland_notify_parent(SIGCHLD)`，父进程的
  `wait4` 无从唤醒。真实内核会把这种 trap 变成 SIGILL/SIGSEGV 投给进程。
- **回溯没有帧上限**：`do { ... } while (true)` 只在 FP 不再递增或不可读时退出；
  损坏的帧链可以一路递增着走完整个栈区，于是同一帧被打印几千次。

## 修法

`src/core/rvvm_user.c`：

1. `userland_destroy()` 释放 address space 之前，把**还活着**的 fork 子进程重新登记到
   family root（`userland_proc_register(root, p)` + `p->parent_ctx = root`），等价于
   reparent 到 init。`ppid` 故意保留已死的父 pid：它退出时
   `userland_parent_alive()` 因此返回 false，走既有的 auto-reap，而不是留下没人
   `wait` 的僵尸。必须在 `userland_detach_family()` 之前做——后者会把 `parent_ctx`
   清空。
2. `userland_kill_process()` 结束进程后补同样的 auto-reap：
   `if (!proc->run_root && !userland_parent_alive(proc)) userland_proc_forget(...)`。
   正常父子关系不受影响（父还活着就不收，`wait4` 语义不变）。
3. `Boom!` 分支在打印完回溯后，把它当作致命信号结束**进程**：
   `cause == 2`（illegal instruction）→ `UAPI_SIGILL`，其余 → `UAPI_SIGSEGV`，
   然后 `userland_kill_process(uctx(), thread->proc, fatal_sig)`。这既标记退出、
   停掉同进程的其它线程，也会 `userland_notify_parent()`。
4. 回溯加两道闸：最多 64 帧；同一 PC 连续出现 3 次即停。`Instruction bytes around PC`
   与对齐标记改用 `rvvm_warn`，和其余 dump 进同一个日志通道。

## 验证

构建：`mingw32-make BUILDDIR=release.test.windows.x86_64 bin`（实例在跑时 exe 被
Windows 锁住，先 `us rm/stop`）。以下都是新 core 的实测输出。

孤儿可见（一个会话起、另一个会话看）：

```
setsid sleep 300 &      → ok
ls /proc                → 100 106 107 ...        # 106 在
ps                      → 106 root 0:00 sleep 300
cat /proc/106/stat      → 106 (sleep) S 105
kill -TERM 106; sleep 2 → GONE
```

被杀的孤儿不再滞留，端口同步释放（mockserver 场景）：

```
kill 163                    → KILLED
host LISTEN 8899            → 0
deploy --restart            → DEPLOY_OK
GET /                       → 200 (15036 bytes)
新进程                       → pid=308
```

崩溃不再卡死父进程，dump 有界且在同一通道：

```
/tmp/deep; echo $?      → exit=132          # 128+SIGILL，立即返回
日志: * * * Backtrace repeats PC 1022fc8, stopping
      Instruction bytes around PC: e780000509a0033584fa232aa4fc39a0...
      * * * guest exception is fatal, ending the process (signal 4)
/proc                    → 崩溃进程已消失
```

回归面：`test_jobctl` **直接运行**（不接管道）仍是基线的 8 项 ctty FAIL，无新增；
`ps`/`top`/`kill` 正常（接管道跑会多 6 项 —— stdout 不是 tty，ctty 前提变了，那是用法
问题）。本轮按用户要求**未跑 e2e**（`tools/*_e2e.ps1`）。

## 已知限制

- `rvvm_user_child_main()` 的兜底 `fork: child threads linger, leaking the machine`
  仍在：子 address space 的线程 1 秒内没收干净就泄漏整台机器。本轮未触发，未改。
- `/proc` 一次性快照仍受 `RVVM_PROC_PID_MAX`（256）限制。
