````markdown
# sshd 在 RVVM (Win32) 上的调试记录

> 目标：让 guest 里的 OpenSSH (`/usr/sbin/sshd` + `/usr/bin/ssh`, OpenSSH_10.3p1) 端到端跑通。
> 配套 e2e：`tools/openssh_e2e.ps1`。
> 本文记录复现方法、观测手段、踩过的坑、已修复的模拟器缺陷，以及仍然存在的阻塞点。

---

## 0. 结论速览

| 阶段 | 状态 |
|---|---|
| 模拟器稳定性（HOST FAULT / 挂死） | ✅ 已修复，HOST FAULT 归零，e2e 不再挂死 |
| 回归（`procfs_e2e` / `ash_e2e`） | ✅ 22/22、7/7 通过 |
| sshd 启动 / 守护化 | ✅ 通过 |
| sshd 启动前的客户端连接 I/O | ❌ 对客户端连接的 `sent`/`recv` 统计为 **0**，banner 从未互换（§6.3.1） |
| privsep monitor 通道 | ✅ socketpair 上双向一字节不差走通 |
| SSH 握手（pubkey 会话） | ❌ 仍失败：`kex_exchange_identification: Connection reset by peer` |

已修复 7 个模拟器缺陷（见 §5、§5A），其中 3 个是"任意程序都可能踩到"的通用 bug：
writev 零长度 iovec、地址空间 UAF、win32 带超时 futex 等待可能永久阻塞。

**当前阻塞点**：sshd 在 fork 之前、在父进程里，对刚 accept 的客户端连接不做任何
I/O —— 既不写自己的 banner，也不读客户端的（§6.3.1）。monitor 那条 socketpair
反倒完全正常，所以问题不在 privsep，而在 banner 互换之前那一步。

---

## 1. 怎么复现和驱动

### 1.1 core + client

```powershell
# 起一个 core（run 边界），后台运行并重定向日志
$env:RVVM_TRACE='pty,job,fd,wsock,sys'   # 按需；'' 表示关闭
$env:RVVM_VERBOSE='1'                    # 每个 syscall 的 INFO 行
rvvm_ash_x86_64.exe --serve --port 7897 >$env:TEMP\core_out.txt 2>$env:TEMP\core_err.txt &

# 用同一个二进制当客户端，跑一条命令
rvvm_ash_x86_64.exe --port 7897 -c 'echo hello'
````

- `-c` 的命令体由 vpsessiond __原&#x6837;__&#x4EA4;给 `sh -c`（`execl(shell, "-c", s->cmd)`），所以 shell 引号照常生效。
- __命令里只能用单引号__：PowerShell 传 native 参数时会把内嵌双引号吃掉，guest 侧就变成了语法错误。 PowerShell 脚本里想写一个单引号要用 `''`（例：`-N ''` 想传空串，要写 `-N ''''`）。
- 长时间跑的 guest 程序要显式收尾：`< /dev/null` 结束 stdin，否则 `-c` 会话会挂住 （ssh-keygen 问 "Overwrite (y/n)?" 就把整个会话冻住过——这是 e2e 里 `rm -f` 掉旧密钥 + `< /dev/null` 的原因）。

### 1.2 观测

- __trace 走 core 的 stderr__（每个进程自己的 stderr，不是 stdout）。`TRACE:` 行带 `[pid:tid]` （本次新增，见 §4.1），`INFO:` 行不带 pid。
- `RVVM_TRACE` 分类：`pty` / `job` / `fd` / `wsock` / `sys` / `signal` / `tty` / `dev` / `mmap`； `all` 全开，`-name` 关掉某个。
- __收发 trace 里的 socket 指针不是身份__：释放后地址立刻被复用，跨行甚至跨轮次
  都会撞上另一个 socket。**只有 peer（`sent 3605 byte(s) on fd 6 (peer 127.0.0.1:49526)`）
  是查询出来的真值** —— 认连接一律认 peer，见 §6.3.1 的教训。
- __sshd 的日志甩进 guest 文件，而不是靠 pty 转发__（见 §1.3）。这条是 daemonize 路径下唯一
  能拿到日志的办法，且不依赖会话存活。
- __抓栈__（最有价值的一步）：

```powershell
# 方式 A：让 core 跑在 gdb 里，用 gdb 脚本跑到崩溃就停
gdb --batch -x cmds.txt --args rvvm_ash_x86_64.exe --serve --port 7919
# cmds.txt:
#   set pagination off
#   set environment _NO_DEBUG_HEAP 1
#   handle SIGTRAP nostop noprint pass
#   run
#   bt
#   info registers rip rsp
#   thread apply all bt
#   quit
# 另开一个 shell 用 client 驱动 guest

# 方式 B：core 已经卡住时直接附加
gdb --batch -p <pid> -ex "set pagination off" -ex "thread apply all bt"
```

---

### 1.3 把 sshd 日志甩进 guest 文件（daemonize 路径唯一可用的观测手段）

§3 坑 #4 说"debug 一律用 `sshd -d -d` 并让会话活着"，但 **`-d -d` 会让 sshd
不 fork**（日志里明写 `Server will not fork when running in debugging mode.`），
于是 privsep 子进程那一段**根本不会发生** —— 也就是复现不了 §6.3 里
`[104:104] close anchor 5/6/10 → thread_exit` 那个现象。文档 §6 要查的正是那条路径。

解法：__不要靠 pty 转发 stderr，直接重定向到 guest 里的文件__。`runtime/rootfs` 是
guest `/` 的可写层（不是 scratch copy，见 `win32-host/README.md` 的 Persistence 一节），
路径一一对应，宿主侧可以直接读：

| guest 路径 | 宿主路径 |
|---|---|
| `/tmp/sshd.log` | `release.windows.x86_64\runtime\rootfs\tmp\sshd.log` |
| `/etc/passwd`  | `release.windows.x86_64\runtime\rootfs\etc\passwd`  |
| `/root/.ssh/`  | `release.windows.x86_64\runtime\rootfs\root\.ssh\`      |

好处：daemonize 之后 stderr 已挂到关闭的会话上，但文件照样在宿主盘上；
会话结束、core 被 kill 都不影响日志留存。

具体怎么起 core、怎么发那条 `-c`、怎么读日志，`tools/openssh_e2e.ps1` 里就是活的
版本（它就是这个手法的自动化），改的时候对着它改，不要在这里维护第二份命令。

注意 `2>&1` 在 guest 里合并的是 sshd **自己的** fd 1/2，和 core 的 trace（走宿主
stderr）是两套互不干扰的通道，可以同时开。

### 1.4 顺带记录的 setsockopt 缺口（非当前触发点）

`-d -d` 与 daemonize 两条路径的日志都在 `Connection from ...` 之前出现这两行，
sshd 只是 warning 后继续，但说明宿主语义映射确有缺口，值得单独跟进：

```
setsockopt IPV6_V6ONLY: Protocol not available      # setsockopt(IPPROTO_IPV6, IPV6_V6ONLY)
setsockopt socket 6 IP_TOS 184: Invalid argument   # setsockopt(IPPROTO_IP, IP_TOS)
```

### 1.5 干净 runtime 上 openssh 是不存在的

`bundle/rootfs.tar.gz` 是 Alpine **minirootfs**，只带 busybox。全新 `runtime/`
里 `sshd` / `ssh` / `ssh-keygen` 全都没有，表现为 `ssh-keygen: not found`，
后面每一步都因为错的原因失败。e2e 因此自带一步 `apk add --no-cache openssh`。
想手工复现就先装，否则会误判成模拟器的问题。

另外镜像自带的 `/etc/passwd` 里是 `sshd:x:22:22:sshd:/dev/null:/sbin/nologin`，
不是 OpenSSH privsep 要的身份；e2e 的 setup 用 `grep -q '^sshd:'` 守卫，
只在镜像没带该条目时才补 `74:74:privsep`。

---

## 2. 观测手段里两个必须知道的坑

### 2.1 gdb 会打开 Windows debug heap —— 它会掩盖真正的崩溃

带 gdb 跑时，未定义行为会变成：

```javascript
warning: HEAP[rvvm_ash_x86_64.exe]: Free Heap block 00000000006D4F50 modified at 00000000006E5F00 after it was freed
```

或者直接一个 `SIGTRAP`（调试堆的检查陷阱），__而不&#x662F;__&#x771F;正的 SIGSEGV。 所以脚本里一定要 `set environment _NO_DEBUG_HEAP 1`，让破坏保持潜伏、直到真正的崩溃点。

（这一步是定位 §5.4 的关键：只有关掉它，才会停在真正的调用栈上。）

### 2.2 陈旧 core 会污染一切结论

- client 在连不上时&#x4F1A;__&#x81EA;动拉起一个 core__（wsl 式 autostart）；
- 手工排查时经常残留 core/client，于是"新的实验"实际连到了旧 core 上， 日志描述的是另一个 guest 运行，结论完全错乱（本次就因此误判过好几轮）。

__规矩：每轮实验前 `Get-Process rvvm* | Stop-Process -Force`，并且换个端口。__

---

## 3. 定位过程中踩的坑（方法论）

1. __误认 accept__：`vpsessiond` 的监听器是 guest 里的 AF_UNIX socket，它的 `accept(addr=NULL, len=0)` 和 sshd 的 `accept(addr, addrlen)` 在日志里长得一样。真正的 sshd accept 是 `sys_accept(guest N, host M, <非零指针>, <非零指针>)` 那一条。 认错这个，把"会话建立的流量"当成"SSH 连接的流量"，会把结论带偏一整轮。
2. __grep 被 ANSI 转义打断__：trace 行实际是 `\e[36;1mTRACE\e[0;1m[103:103]: ...`， `TRACE[(\d+):` 匹配不到。要用 `[(\d+):\d+]:` 匹配。
3. __`nr=` 行是十六进制返回值__：`rvvm_info(" nr=%ld -> %lx", a7, a0)`，所以 `nr=220 -> 68` 是 `clone` 返回 `0x68 = 104`（pid），不是 68。
4. __guest 日志会经 pty 转发__：sshd 的 stderr 是会话 pty，客户端可见； 但 daemonize 之后 stderr 仍挂在那个已关闭的会话上，日志就丢了 —— 所以排查 一律用 `sshd -d -d`（前台调试）并让会话活着，__或更好：直接把 stderr 重定向到 guest 文件（§1.3），那是 daemonize 路径唯一可行的观测手段__。
5. __子命令要能自己结束__：`tail -50` 会等到 EOF 才输出，掩盖真实进度； 把输出重定向到 guest 内文件、再用另一条 `-c` 去 `cat`，两边日志都拿得到。

---

## 4. 基础设施改动

### 4.1 trace 带 `[pid:tid]`

`src/util/utils.{h,c}`：新增 `rvvm_trace_set_id_fn()` 钩子；`rvvm_user_thread_wrap` 里注册一次， 回调输出 `[pid:tid]`（guest 进程/线程），非 guest 线程输出 `[host]`。没有它， 多进程 fork/exec 的日志完全无法归因（本次多个误判都源于此）。

### 4.2 `win_socket` 收发可见性

`src/win/win_socket.c`：`win_socket_read()` / `win_socket_write()` 成功后打 `recv N byte(s) on fd` / `sent N byte(s) on fd`（`RVVM_TRC_WSOCK` 分类）。 此前 socket 路径完全没有数据流可见性，"服务端有没有真的发过 banner"根本看不到。

---

## 5. 已修复的模拟器缺陷

### 5.1 `writev`：零长度 iovec 被判 EFAULT

- 现象：任何以 `{NULL, 0}` 收尾的 writev 整体失败。musl stdio 的提示输出正是这个形状。
- 根因：writev 的 own-fd（pty / /dev 设备）路&#x5F84;__&#x6CA1;有__ `if (!iov_len) continue;` （readv 路径有），而 `userland_pty_write()` / `userland_dev_write()` 先判 `!buf` 再判 `!count`。
- 修复：`rvvm_user.c` writev 写循环跳过零长度段；两个写 helper 改为先判 `!count` 返回 0。

### 5.2 地址空间 UAF：`parent_ctx` / `child_ctx` 指向已释放内存

- 现象：`HOST FAULT` 随机出现，崩溃点随机（guest 堆地址、-1、exit code 3 …）。
- gdb 反汇编给出的现场：
  ```javascript
  0x...0033  mov  0x48(%rdi),%rax     ; rax = proc->parent_ctx
  0x...0037  test %rax,%rax
  0x...003a  je   ...
  =>0x...003c cmpq $0x1,0x2a10(%rax)   ; 读 ctx->siga[sig].handler 与 SIG_IGN(1) 比较 → 崩
  ```
  即 `userland_exit_process → userland_notify_parent` 把 SIGCHLD 投递&#x7ED9;__&#x5DF2;释放的 ctx__。
- 场景：会话 shell 退出 → 它的地址空间被 `userland_destroy` 释放 → 而它 fork 出并守护化的 sshd 还在跑，其进程记录仍指向那块内存 → sshd 退出时投递 SIGCHLD 踩空。
- 修复：给 `rvvm_userland_t` 增加 `children` 注册表（`vector_t(struct rvvm_userland*)` + `children_lock`）， 新增 `userland_detach_records()` / `userland_detach_below()` / `userland_detach_family()`： 销毁时向上清理各祖先注册表里的记录、向下断开所有子空间的 `parent_ctx`； 并且每个 ctx 销毁&#x65F6;__&#x5148;把自己从父的 children 里摘掉__（否则先销毁的子 ctx 会作为野指针留在父的列表里， 父销毁时走进查就崩——这个 bug 就是在修第一版时踩到的）。

### 5.3 win32 带超时的等待可能永久阻塞

- 现象：`sh -c 'cmd & wait $!'` 随机永久挂起（trace 显示 `rt_sigsuspend` 之后再无任何 syscall，CPU 0%）。
- 根因：`futex_emu_waiter_wait()` 的 Win32 分支用 `WaitForMultipleObjects({event, timer}, INFINITE)`，__超时完全依赖那个线程局部的手动复位 waitable timer 被正确重置__，而 `SetWaitableTimer` 的返回值被丢弃；一旦装配失败， 100ms 的等待就变成永久等待。
- 修复：等待本身带毫秒级超时上限（计时器只作亚毫秒精度增强）；`thread_local_waitable_timer()` 装配失败则退役该 timer，之后全部走毫秒路径。

### 5.4 SEH 过滤器自死锁 → 整个模拟器卡死

- 现象：一次瞬时 AV 之后，__任何 execve__ 都挂死（core 卡在 `vma_clean` 等锁上，CPU 0%）。
- gdb 附加现场：
  ```javascript
  Thread 6: #5 vma_clean #6 rvjit_flush_cache #7 guest_exec #8 rvvm_sys_execve ...
            #3 rvvm_futex_wait #4 rvvm_lock_wait_raw        ← 永远等不到
  Thread 5: shim_veh → user_fault_handler → ... 递归爆栈，卡在 UnhandledExceptionFilter
  ```
- 根因：`vma_clean()`（清理 JIT 的 RWX 堆时）持全局 `seh_lock` 并装一个临时异常过滤器来掩盖 "别的线程访问正在被清零的 VMA"；而这个过滤器 `seh_handler()` __又去拿同一把非递归锁__。 被它遮盖的 fault 恰好发生在持锁线程自己身上 → 永久持锁。
- 修复：`seh_handler()` 不再取 `seh_lock`（无锁读 `seh_prev_handler` 是良性的）。

### 5.5 fault handler 自身会二次 fault

- 现象：HOST FAULT 之后进程不是干净退出，而是卡死在异常过滤机制里。
- 根因：handler 在栈上开 8KB 缓冲；触发 fault 的线程宿主栈往往已耗尽（guest 深递归会一路穿过 解释器帧），于是 handler 一进去就再 fault，递归到爆栈。
- 修复：加线程内递归保护（二次 fault 直接 `_Exit`），并把缓冲区改为静态。

### 5.6 SIGCHLD 在默认处置下不投递 → `wait` 永远醒不了

- 现象：`sh -c 'sleep 1 & wait $!; echo ok'` 随机挂死；trace 显示 shell 停在 `job: rt_sigsuspend(pid=…) blocking`，而子进程早已退出。
- 根因：ash 用 `rt_sigsuspend` 等 SIGCHLD；`userland_deliver_signal()` 在处置为 `SIG_DFL`/`SIG_IGN` &#x65F6;__&#x76F4;接返回 false、不入队__。而 Linux 上 wait 与是否捕获 SIGCHLD 无关地被唤醒。
- 修复：把 `SIGCHLD` / `SIGCONT` 当作"通知"处理——即使没有 handler 也入 pending 槽 （投递边界遇到 `SIG_DFL`/`SIG_IGN` 照常消费掉、不执行 handler，代价只是一次唤醒）。

### 5.7 附带：win32 上 `fstat(socket)` 报成字符设备

socket 锚点是 `_open("NUL")`，宿主 stat 会给出字符设备。已加分支：命中 wsock 锚点时报 `S_IFSOCK | 0777`（guest 头文件缺 `S_IFSOCK` 时按文件内既有 `S_IFLNK` 的做法补位值）。 __注：这一条修完后 sshd 握手仍失败，说明它不是本次的触发点。__

---

## 5A. 新发现的缺陷（2026-09-27，尚未修复）

### 5A.1 pid/tid 空间是**每地址空间一份**，导致跨分支 pid 碰撞

- 现象：一次 daemonize 路径的运行里，trace 出现**同一个 pid 同时属于两个活进程**：

  ```
  TRACE[104:104]: fork: parent=104 child=105      # sshd 主进程 fork 出 105
  TRACE[105:105]: fork: parent=105 child=106      # 105 再 fork 出 106
  TRACE[101:101]: fork: parent=101 child=105      # ★ shell 又 fork 出一个 105 —— 撞了
  TRACE[106:106]: fork: parent=106 child=107
  TRACE[101:101]: fork: parent=101 child=106      # ★ 又撞 106
  ```

  同一轮里 `pid 105` 先是 sshd 的子进程（`ctx=0x2a4621dac40` 一族），
  随后又成了客户端 `/usr/bin/ssh`（`ctx=0x2a4e53a8090` 一族）。
  退出日志同样错位：`TRACE[105:105]: sys_exit_group(255) ctx=0x2a4621dac40` 与
  `TRACE[105:105]: sys_exit_group(0) ctx=0x2a4e53a8090` —— 同一个 `[105:105]`
  标签下是两个不同地址空间。

- 根因：`userland_task_id_alloc()` 用的是 **ctx 自己的** `next_task_id`
  （`src/core/rvvm_user.c`）：

  ```c
  static uint32_t userland_task_id_alloc(rvvm_userland_t* ctx)
  {
      spin_lock(&ctx->proc_lock);
      uint32_t id = ctx->next_task_id++;   // ← 每地址空间一份计数器
      spin_unlock(&ctx->proc_lock);
      return id;
  }
  ```

  而 `userland_child_create()` 给子地址空间播种时是：

  ```c
  ctx->next_task_id = pid + 1;   // 子进程戴 pid，它自己的 id 从 pid+1 开始
  ```

  这个 `pid + 1` 只保证**父子链上**不撞，保证不了**兄弟子树之间**不撞：
  shell(101) 的计数器还在 105 时，它 fork 出的 sshd(104) 已经用自己那份
  计数器发到了 105/106。于是两条互不相干的分支各自往下发号，号段重叠。

  Linux 上 pid 在**整个 pid namespace** 内唯一；这里的模型是"每个地址空间一份"，
  语义就不对。

- 影响：任何按 pid 索引的结构都会认错人 —— `wait4(pid)` 可能等到别人的孩子、
  `kill(pid)` / 信号投递找错记录、进程组/会话搜索（`tty_fg_pgid` 那套）跨子树失配、
  `/proc` 枚举出现重名。

- **已修复**（commit `851622d`）：改成全 run 共享的、带引用计数的单调计数器。
  fork 序列恢复严格单调，无碰撞。**但它不是握手失败的原因** —— 回到未改动的源码
  重建后，e2e 仍是同样 3 个 FAIL。

---

## 6. OpenSSH 现状：精确定位到的阻塞点

### 6.1 客户端侧

```javascript
debug1: Connection established.
debug1: Local version string SSH-2.0-OpenSSH_10.3
kex_exchange_identification: read: Connection reset by peer
```

### 6.2 服务端侧（`sshd -e -d -d`，即使 `-ddd` 也一样）

```javascript
debug1: network sockets: 6, 6
debug2: fd 6 setting TCP_NODELAY
setsockopt socket 6 IP_TOS 184: Invalid argument
Connection from 127.0.0.1 port 51888 with IP opts:  80 00 00 00
<—— 日志到此为止，无任何 fatal/error>
```

`wait` 得到 __`sshd-status=255`__：是进&#x7A0B;__&#x81EA;己__ `cleanup_exit(255)`，不是被信号杀死 （若被信号杀会是 `128+N`）。

### 6.3 模拟器 trace（按 pid 归因）

⚠️ **下面这段旧记录是错的**，原因见 §6.3.1。保留原文只为对照。

```javascript
# sshd 侧
[103:103] accept -> guest fd 8
[103:103] pair: listener port 55898, server peer 127.0.0.1:55899     ← 监视 socketpair 建成
[103:103] fd_wr[install] fd=9/10                                      ← 监视对两端
[103:103] close anchor 9 (socket 0x1e8)  peer=127.0.0.1:55898         ← 监视进程关掉一端（正确）
# privsep 子进程
[103:103] fd_wr[inherit_dup] ctx=<child> fd=8  host=5   (socket 0x1c4) ← 客户端连接，独立描述符
[103:103] fd_wr[inherit_dup] ctx=<child> fd=9  host=6   (socket 0x204)
[103:103] fd_wr[inherit_dup] ctx=<child> fd=10 host=10  (socket 0x20c)
[104:104] sent 3605 byte(s) on fd 6 ; sent 2253 ...                    ← 与监视器的协议流量
[104:104] close anchor 5/6/10  → thread_exit                          ← 三个 socket 全关后退出
```

__即：__

- socketpair 能建、fork 后三个 socket 都&#x4EE5;__&#x72EC;立的 Winsock 描述&#x7B26;__&#x6B63;确继承 （`WSADuplicateSocket` 语义，MSDN 保证"最后一个描述符关闭前底层 socket 不关"）—— §5.2 那个"父进程 close(9) 就 FIN 掉子进程 fd 4"的原始猜&#x60F3;__&#x4E0D;成立__，fd 继承语义本身是对的；
- 子进程完成 monitor 握手&#x540E;__&#x4ECE;未向客户端连接写过一个字&#x8282;__&#x5C31;退出了；
- 监视进程随之退出 → 连接最后一个描述符关闭 → Windows 回 RST → 客户端看到 `Connection reset`。

### 6.3.1 更正：privsep 的 monitor 通道**完全正常**，错的是客户端连接

`win_socket.c` 的收发 trace 现在会带上 peer（commit `c14043f`）。带上 peer 重跑，
结论和上面**相反**：

```javascript
# 客户端临时端口 49525；socketpair：server 端 49526，client 端 49527
[105:105]: sent 22 byte(s) on fd 7 (peer 127.0.0.1:2240)      ← 客户端发出自己的 banner
[103:103]: fork: parent=103 child=106                          ← privsep 子进程
[106:106]: sent 3605 byte(s) on fd 6 (peer 127.0.0.1:49526)    ← 子进程写 monitorsock[0]（local 49527）
[106:106]: sent 2252 byte(s) on fd 6 (peer 127.0.0.1:49526)
[103:103]: recv 4 byte(s) on fd 8 (peer 127.0.0.1:49527)       ← 监视进程从 monitorsock[1] 读
[103:103]: recv 3601 byte(s) on fd 8 (peer 127.0.0.1:49527)
[103:103]: recv 4 byte(s) on fd 8 (peer 127.0.0.1:49527)
[103:103]: recv 2248 byte(s) on fd 8 (peer 127.0.0.1:49527)
[105:105]: recv err 10054 on fd 7 errno=108                     ← 客户端只拿到 RST
```

- **旧结论错在哪**：`fd_wr[inherit_dup]` / `close anchor` 那些行里的 **socket 指针不可信** ——
  socket 释放后地址立刻被复用（`close anchor 8 (socket 0x1e4)` 紧跟着
  `fd 8 <- socket 0x1e4` 就是同一个地址换了对象）。**只有 `close: peer=` 是查询出来的、
  跨 fork 依然成立的真值**。我按指针跨轮次对照 peer，推出了"写到 monitor 去了"这个
  假象。教训同 §3 坑 #2：**不要拿可复用的地址当身份**。
- `accept -> anchor 8` 也被误读了：那是 `win_socket_pair()` **内部**的 accept
  （它自己 listen/connect/accept 一条回环 TCP），不是 sshd 收客户端连接那次。
- **真正的现象**：整轮 trace 里，对客户端连接（peer 49525）的
  `sent`/`recv` 统计是 **0**。既没人写出 banner，也没人读客户端发来的 banner。
  而 monitor 那条 socketpair 上的 privsep 握手**一字节不差地双向走通**
  （4+3601 与 4+2248 长度前缀消息，配对精确）。
- 所以阻塞点要重述为：__**sshd 从不对客户端连接做任何 I/O**__。
  按 OpenSSH 的流程，`sshd_exchange_identification()`（banner 互换）发生在
  **fork 之前、在父进程里**，父进程此时还没进 privsep。它写完 banner、
  读完客户端的 banner，才 fork。现象正好是"父进程手里那条客户端连接是死的"。

### 6.4 下一步（已按 §6.3.1 更新）

要查的是：**父进程（monitor，pid 103）在 fork 之前，对客户端连接那个 guest fd
做的读写去了哪里**。具体：

1. sshd accept 客户端连接后，guest fd 是几？（注意别把 `win_socket_pair()` 内部的
   accept 认成它 —— 看 peer 是不是 socketpair 那两个端口）
2. 那个 fd 上第一次 `read`/`write` 是哪一条 syscall、返回什么？
   父进程此刻还没 fork，**没有 `inherit_dup` 的干扰**，是整个问题里最干净的一段。
3. 候选：`dup2`/`dup3` 把客户端 socket 挪到 0/1/2 的那几条
   （trace 里能看到 `sys_dup3(N, 0, 0)`），以及 `set_nonblock`。

观测手段见 §1.3：__不要用 `-d -d`__（它让 sshd 不 fork，privsep 路径根本不走），
要 daemonize + `> /tmp/sshdbg.log 2>&1`，再从 `runtime/rootfs/tmp/sshdbg.log` 读。

---

## 7. 涉及文件

- `tools/openssh_e2e.ps1`（新增）：OpenSSH 端到端用例
- `src/core/rvvm_user.c`：trace id 钩子注册、ctx children 注册表与 detach 走查、writev 零长度段、 写 helper 的空写语义、SIGCHLD 通知语义、socket 的 fstat
- `src/util/threading.c`：win32 带超时等待的上限 + timer 装配校验
- `src/util/vma_ops.c`：`seh_handler` 不再取 `seh_lock`
- `src/util/utils.{h,c}`：trace 行身份前缀
- `src/win/win_socket.c`：收发 trace
