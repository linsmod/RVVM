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
| 回归（`procfs_e2e` / `ash_e2e` / `session_e2e` / `jobctl`） | ✅ 全 PASS（`-Multi` 的 4 个失败是既有的，见下） |
| sshd 启动 / 守护化 | ✅ 通过 |
| SSH banner 互换 | ✅ 通过（此前"banner 从未互换"是 sshd 自己 `fatal()` 了，见 §6A.1） |
| privsep monitor 通道 | ✅ socketpair 上双向一字节不差走通 |
| KEX + 公钥校验 | ✅ 通过 |
| **公钥会话 + 执行远端命令** | ⚠️ **能通,但不稳定**（§6B.5）|
| **第二个会话复用同一守护进程** | ⚠️ 同上 |
| `sshd -d -d` 单连接调试模式 | ❌ 同一处 anchor 记账失衡（§6B.4/§6B.5） |

`openssh_e2e` **5–6 / 7**,会飘。**功能层面 client + sshd 已经打通**:真的公钥会话、
真的执行远端命令、真的第二个会话复用同一个守护进程。**但它现在还不能算"可靠"** ——
privsep 子进程偶尔拿不到 socket,而且这不是两个问题,是同一个（§6B.5）。

已修复 14 个模拟器缺陷（见 §5、§5A、§6A、§6B），其中 9 个是"任意程序都可能踩到"的通用 bug：
writev 零长度 iovec、地址空间 UAF、win32 带超时等待可能永久阻塞、
`socket` anchor 号码被回收、`chroot(2)` 缺失、guest 凭证不分 real/effective id、
数据报 `connect(0.0.0.0)` 语义、AF_UNIX `connect` 缺路径的 errno、创建文件不应用 umask。

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

__而且这不只是 daemonize 路径的专利__（第三轮补充）：走 pty 时，sshd `fatal()` 之后
紧接着 `exit_group`，会话随即被拆掉，__最后几行日志会随 pty 一起消失__ —— 而"最后
一行"恰恰是要看的东西。第三轮里 `Function not implemented`、`was able to restore
old [e]gid`、`Invalid argument` 这三条根因，全都是只在重定向到文件时才看得到的。
所以只要是"日志到某一行就断"的排查，一律先把 stderr 落到 guest 文件，**再另开一条
`-c` 去 `cat`**：

```sh
rm -f /tmp/sshdbg.log
/usr/sbin/sshd -d -d -p 2222 ... >/dev/null 2>/tmp/sshdbg.log &
sleep 2; /usr/bin/ssh -p 2222 ... root@127.0.0.1 'echo ok'; sleep 2
cat /tmp/sshdbg.log
```

具体怎么起 core、怎么发那条 `-c`、怎么读日志，`tools/openssh_e2e.ps1` 里就是活的
版本（它就是这个手法的自动化），改的时候对着它改，不要在这里维护第二份命令。

注意 `2>&1` 在 guest 里合并的是 sshd **自己的** fd 1/2，和 core 的 trace（走宿主
stderr）是两套互不干扰的通道，可以同时开。

### 1.4 顺带记录的 socketopt 缺口

`-d -d` 与 daemonize 两条路径的日志都在 `Connection from ...` 之前出现这两行，
sshd 只是 warning 后继续，但说明宿主语义映射确有缺口，**仍然待跟进**：

```
setsockopt IPV6_V6ONLY: Protocol not available      # setsockopt(IPPROTO_IPV6, IPV6_V6ONLY)
setsockopt socket 6 IP_TOS 184: Invalid argument   # setsockopt(IPPROTO_IP, IP_TOS)
```

这一族的 `getsockopt` 侧曾经不是 warning 而是**直接致命**：`IP_OPTIONS` 的 get 在
§6A.1 里吃掉了整条连接。教训是同一个族要两侧一起看 —— `setsockopt` 失败只说明
映射有洞，`getsockopt` 失败可能直接结束进程。

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
6. __先确认那行日志是 `fatal()` 还是 `debug()`__：`debug1:` 前缀极具误导性。 `check_ip_options()` 里 `Connection from ... with IP opts:` 是 **`fatal()`**（见 §6A.1），而 §6.3.1 却按"普通日志"读它，于是把"进程已经死了"当成"连接是死的"，白追两轮。 有疑问就去抠 format string / 读源码，别在 trace 里猜语义。
7. __musl 的失败可能报陈旧 errno__：`initgroups: root: Network is down` 里的 errno 来自**上一次**失败的 `connect()`（§6A.5）。 语义上不该出现的 errno（查 group 怎么会是网络错）先别顺着 errno 名猜，回头找最近一次失败的 syscall。
8. __`RVVM_VERBOSE=1` + `RVVM_TRACE=sys` 会打出 syscall 实参__：`INFO: sys_getsockopt(6, 0, 4, ...)`。 §6A.1 的 `IP_OPTIONS` 常量取错（Linux 4 / WinSock 1）就是这样被发现的 —— 只打指针的 trace 看不出这种错。
9. __`INFO:` 行不带 `[pid:tid]`__：要按进程归因就用 `RVVM_TRC`（`TRACE:` 行才有前缀）。 §6A.5 第一版把 connect 的目标 trace 打在 INFO 通道上，结果分不清那两次 UDP 探测是谁发的。 和坑 #2 一样是**格式**骗人。
   → **已修**：现在 `log_prefix()`（`src/util/utils.c`）给**所有**级别都加身份，`INFO[104:104]:` / `WARN[host]:` / `ERROR[104:104]:` 都带。 无 userland 挂载时仍是 `INFO: `，注册 formatter 之前那两行（`Loaded ELF ...`）同理。
10. __宿主没有的语义要照 Linux 的规则补，不要照宿主__：`connect(0.0.0.0)` 在 Linux 是成功的（内核把 `INADDR_ANY` 改写成 loopback），在 WinSock 上是 `WSAEADDRNOTAVAIL`（§6A.6）。 这一类差异不会报错，只会让 guest 的探测静默失败并留下一个误导性的 errno。

---

## 4. 基础设施改动

### 4.1 trace 带 `[pid:tid]`

`src/util/utils.{h,c}`：新增 `rvvm_trace_set_id_fn()` 钩子；`rvvm_user_thread_wrap` 里注册一次， 回调输出 `[pid:tid]`（guest 进程/线程），非 guest 线程输出 `[host]`。没有它， 多进程 fork/exec 的日志完全无法归因（本次多个误判都源于此）。

**后来扩到所有级别**（第三轮）：原先只有 `TRACE:` 带身份，`INFO:`/`WARN:`/`ERROR:` 不带 ——
而 `sys_connect()` 这类最需要归因的行恰恰是 `INFO:`。见坑 #9。现在 `log_prefix()`
统一给 `DEBUG`/`INFO`/`WARN`/`ERROR`/`FATAL`/`TRACE` 加身份，形如
`INFO[104:104]: `；guest 之外是 `[host]`，没有 userland 挂载时保持原样。

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

## 6. OpenSSH：当时的阻塞点（已被 §6A 推翻）

> ⚠️ **本节的结论已被 §6A 推翻，保留只为对照。** 本节（连同 §6.3.1 的更正）花了
> 两轮，把"客户端连接上 sent/recv 为 0"一路追到 `userland_fd_table_inherit`，方向是错的。
> 真实原因根本不需要追：sshd 在那之前就已经 **`fatal()` 退出 255** 了，客户端看到 RST
> 是结果而不是起点。§6.3.1 的"父进程手里那条客户端连接是死的"其实是
> **父进程已经死了**。
> 当前状态见 §0，修复过程见 §6A。

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

> ❌ **这份清单本身是错的，别照着查。** 三个候选里没有一个是真凶，而且"那个 fd 上
> 第一次 read/write"根本不存在 —— 父进程在写之前就 `fatal()` 退出了（§6A.1）。
> 保留它是因为它记录了当时的推理路径；正确的方法见 §6A.0。

---

## 7. 涉及文件

- `tools/openssh_e2e.ps1`（新增）：OpenSSH 端到端用例
- `src/core/rvvm_user.c`：trace id 钩子注册、ctx children 注册表与 detach 走查、writev 零长度段、 写 helper 的空写语义、SIGCHLD 通知语义、socket 的 fstat、**`chroot(2)`**（§6A.2）、**guest 凭证的 real/effective id 与 `setres*`/`getres*`**（§6A.3）、**`prlimit64(2)`**（§6A.4）、**`sys_connect` 的目标地址 trace**（§6A.6）、**per-ctx umask 与创建时套用**（§6B.2）
- `src/util/threading.c`：win32 带超时等待的上限 + timer 装配校验
- `src/util/vma_ops.c`：`seh_handler` 不再取 `seh_lock`
- `src/util/utils.{h,c}`：trace 行身份前缀（后扩到所有日志级别，见 §4.1）
- `src/win/win_socket.c`：收发 trace、anchor 引用计数与 generation 校验、**`getsockopt(IP_OPTIONS)` 的 Linux 语义**（§6A.1）、**数据报 `connect(0.0.0.0)` 视为本机**（§6A.6）、**AF_UNIX `connect` 缺路径回 ENOENT**（§6B.1）



update: 9-27 13:13

# 找到根因了

> ⚠️ 这一节的结论**是错的**，见下面"第二轮"和 §6A。它把 trace 采样窗口里看到的
> "子进程 fd 6 是空的"当成了根因；实际上 sshd 早就 `fatal()` 退出了。

## 决定性证据

`fd 6` 在 sshd-session(子进程)里**从头到尾是空的**:

```
TRACE[103:103]: fd_wr[init]    pid=106 fd=6 used=0 host=0
TRACE[103:103]: fd_wr[inherit] pid=106 fd=6 used=0 host=0     ← 继承下来就是空的,之后再没被填过
```

而 sshd-session 自己的日志说它用的是 fd 6:

```
debug1: sshd-session version OpenSSH_10.3
debug1: network sockets: 6, 6                                  ← 客户端连接在 6
debug2: fd 6 setting TCP_NODELAY
Connection from 127.0.0.1 port 60701 with IP opts:  80 00 00 00
<—— 再无输出:banner 互换的两行 debug 都没有>
```

**子进程认定客户端连接在 fd 6,而它的 fd 6 是个空槽。** 真正的连接在别处(peer 60701 的持有情况):

```
fd_drop pid=103 fd=8  host=10  socket peer 60701   ← sshd master
fd_drop pid=106 fd=8  host=11  socket peer 60701   ← 子进程,只有 fd 8
fd_drop pid=103 fd=0  host=12  socket peer 60701
fd_drop pid=103 fd=1  host=16  socket peer 60701
fd_drop pid=103 fd=6  host=6   socket peer 60701
```

父进程在 6/8/0/1 上都有,**唯独继承到子进程的那个是空的**。所以 `sshd_exchange_identification()` 往 fd 6 写 banner 时,写的是一个不存在的描述符 —— banner 永远出不去,客户端只等到 RST。

顺带:sshd 明明打印了 `Connection from 127.0.0.1 port 60701`,说明它 accept 成功、拿到了对端地址。它知道连接在,却仍然写不出去 —— 完全符合"fd 号对不上内容"。

## 顺带挖出一个更严重的独立问题

`userland_fd_host()`(`src/core/rvvm_user.c`):

```c
static int userland_fd_host(rvvm_userland_t* ctx, int fd)
{
    if (userland_fd_tracked(ctx, fd)) {
        return ctx->fds[fd].fd;
    }
    return fd;        // ← 未跟踪的 guest fd,直接当成宿主同号 fd 用
}
```

一个**已经被 close 掉的 guest fd**,在宿主侧会落到**宿主进程同号的 fd** 上。guest 往一个已关闭的 fd 写,实际可能写进宿主的任意描述符 —— 这既是正确性问题,也是隔离问题。

我**没有动它**。这个"未跟踪就按同号透传"看起来是为 `dup` 到表外编号兜底而故意留的(见 `userland_fd_slot_alloc` 的注释),全局改成 EBADF 很可能连带打断 dup2 兜底和 console 处理。在没有回归验证手段的前提下改它不负责任,需要单独评估。

## 我没有修复 SSH

诚实说:**没有**。我这轮做到的是把根因从"客户端连接 I/O 为 0"收敛到一个具体的、可验证的事实 —— **privsep 子进程里 sshd 声称的连接 fd(6)是空的,而连接实际在 fd 8**。

剩下的未知是**为什么**空的。fork 继承路径 `userland_fd_table_inherit` 是嫌疑最大的地方(它按 fd 逐个复制,某些分支会静默跳过),但我没有验证到那一行,不想再给一个未经验证的结论 —— 这轮我已经有两次因为过度解读 trace 而下错结论。

## 下一步(具体)

1. 在 `userland_fd_table_inherit` 里,子进程 fd 6 为什么没被填?重点看 `shared` / pty / procfs 三个提前 `continue` 的分支,以及 `dup()` 失败时的 `inherit_shared` 兜底。
2. 父进程在 fd 6 上装连接的时刻,相对 fork 的先后 —— 如果父进程的 dup2 发生在 fork **之后**,那就是竞态/顺序问题,子进程自然看不到。
3. 验证手段已经就绪:`fd_wr[... ] pid=N` 现在能直接看出每一步是哪个进程,`fd_drop` 带 peer。

## 状态

| commit | 内容 |
|---|---|
| `851622d` | pid/tid 全 run 共享(修碰撞)+ e2e 装 openssh |
| `c14043f` | 收发 trace 带 peer |
| `248aa00` | 文档更正 privsep 结论 |
| `08c53ce` | fd trace 带 owner pid + `fd_drop` |

`procfs_e2e` / `ash_e2e` PASS,openssh_e2e 3/7(与基线一致)。工作区干净,没有提交任何未经验证的改动。

关于 gdb:我考虑了,但 gdb 断的是模拟器的宿主符号,看不到 guest 的 fd 表语义 —— 这里真正缺的是**子进程 fd 6 的来源**那一行的 trace,比在宿主上断点更直接。所以这轮没用上 gdb。如果你要我继续,我会先补一条针对 `userland_fd_table_inherit` 每个分支的 trace,再决定要不要上 gdb。

---

# 第二轮:定位到 anchor 复用(已修)

上面"找到根因了"那一节的结论**是错的**,本节更正。它把"子进程 fd 6 是空的"当成根因,
但那其实是 trace 采样窗口的错觉:加上 `sys` 分类后重跑,`inherit` 全部是 `used=1` 的
`inherit_dup`,子进程的 fd 表从来没空过。

## 真正的根因:anchor 号码被同 run 的其他进程借走

`win_socket_alloc_anchor()` 用 `_open("NUL")` 从 **CRT fd 表**取一个号来锚定 socket。
但 guest fd 表把 host fd(就是这个号)长期存下来。两个事实叠加:

1. CRT fd 表是**宿主进程全局**的,一个 run 里所有 guest 进程共用;
2. guest 进程的普通 `open()` 也从同一张表取号。

所以只要 run 里另一个进程(哪怕是毫不相干的 ssh 客户端)开一次文件,拿到 sshd 正占着的
那个号;anchor 一旦归还,这个号就归文件所有 —— 而 sshd 的槽位里还写着这个号。

## 决定性证据

master 的 fd 9 从装上到出问题,中间**没有任何一行 `fd_wr` 写过它**:

```
564212684  fd_wr[install] pid=113 fd=9  used=1 host=9  gen=24    ← socketpair 端 A
564212684  fd_wr[install] pid=113 fd=10 used=1 host=10 gen=25    ← 端 B
564212687  fd_wr[setfl]   pid=113 fd=9  host=9  gen=24           ← 仍是 24,槽位完全正确
564212707  fd_stale pid=113 fd=9 host=9 gen=24 now=32            ← 20ms 后 anchor 9 已是别人的
```

同一时刻在干这件事的是 **pid 114(并发的 ssh 客户端)**:

```
564212686  fd_wr[install] pid=114 fd=4 host=11
564212687  fd_wr[close]   pid=114 fd=4 host=11
564212688  fd_wr[install] pid=114 fd=4 host=11
```

它在不停 open/close `/root/.ssh/id_e2e*`,host 号被反复分配释放。

后果就是最初那个 3605 字节脏数据:master 往自己 socketpair 上写 privsep 数据,
字节实际落进了客户端连接,客户端收到 `0x00` 开头的东西,报 `invalid format`。

## 修法

anchor 加**引用计数 + generation 校验**(`wsock_fds_refs` / `wsock_fds_gen`):

- socket 仍然立刻关,但 **CRT 号只在最后一个 guest 槽位放手时才归还**;
- `userland_fd_host()` 每次解析都核对 generation,不符即返回 EBADF(`fd_stale` trace);
- `userland_fds_write()` 在装/覆盖槽位时配平引用。

**没有选"独立编号域"**:派发路径(`posix_shim` 的 read/close、`win_socket_is_fd` 判定)
依赖 anchor 是**真 CRT fd**,换编号域会波及所有派发。引用计数保住了这个前提。

## 效果与遗留

`fd_stale` 12 次 → **0 次**;`invalid format` 消失,privsep monitor 通道 2252 字节干净送达。
sshd 的失败点因此**后移**到 `kex_exchange_identification: Connection reset by peer`。

回归:`procfs_e2e` 22/22、`ash_e2e` 7/7、`session_e2e` PASS、
`jobctl killpg` / `wait-bg` PASS。`openssh_e2e` 仍 3/7(与基线一致)。

**仍未修好 SSH。** 剩下的 kex 失败是 banner 之后的独立问题,需要载荷级 trace
(把 sshd 实际写出的字节 dump 出来)才能继续,不是 socket 收发统计能回答的。

## 踩到的坑:引用计数不能在 userland_fds_write 里配平

第一版把 ref/unref 写在 `userland_fds_write()` 内部,结果 **procfs_e2e 挂死在
`/proc/self/stat`**(stash 后基线 3/3 通过,确认是真回归)。

原因:`userland_fds_write()` 不是"一次调用 = 一次归属变更"。`userland_fd_table_inherit()`
对同一个槽位会**连续调两次** —— 先 `inherit` 复制父的槽位,再 `inherit_pty` /
`inherit_dup` 换成自己的。按调用配平会把第一次的引用立刻 unref 掉,anchor 计数
提前归零,号被提前归还 CRT,fd 悬空 → guest 卡死。

**结论:引用只在槽位归属真正改变的地方配平** —— `userland_fd_install()` 取,
`userland_fd_close()` 还,inherit 路径显式取。`userland_fds_write()` 只管记账,
不再碰引用。这也符合它自己的注释("唯一改写 ctx->fds[fd] 的函数")。

## 另一个既有问题(与本次改动无关):core 日志泄漏进客户端 stdout

`procfs_e2e` 偶发 FAIL `/proc/self/cwd readlinks to /`。已定位:**不是** fd 问题,
`readlink /proc/self/cwd` 本身 6/6 都正确返回 `/`,但每 2 次有 1 次 core 的
`vpsessiond:` 日志混进了客户端捕获的 stdout:

```
match=True  raw=[/]
match=False raw=[[...] vpsessiond:   [slot 0] open sock=4|[...] client connected, 1 of 16 in use|
                 [...] session 4 started (pid 107, command), 1 of 16 in use|
                 /|[...] close why=shell exited sock=4 pid=0 child_done=1]
```

驱动的 `Client()` 把两路输出合并,检查用 `^\s*/\s*$` 整行匹配,混入日志行就失配。
**未修** —— 与 anchor 修复无关,属于独立问题,不应混进同一个提交。


## 状态

| commit | 内容 |
|---|---|
| `851622d` | pid/tid 全 run 共享(修碰撞)+ e2e 装 openssh |
| `c14043f` | wsock: name the peer in the send/recv traces |
| `248aa00` | 文档更正 privsep 结论 |
| `08c53ce` | fd trace 带 owner pid + `fd_drop` |
| `ac9041b` | wsock: anchor 引用计数 + generation 校验,消除跨进程串写 |
| `5bd87f1` | core: adjust fd slots allocation |
| `0dfc951` | wsock: `getsockopt(IP_OPTIONS)` 按 Linux 语义返回"无选项"(§6A.1) |
| `75b9c76` | userland: chroot / real-vs-effective id / prlimit64(§6A.2-4) |
| `626173d` | wsock: 数据报 `connect(0.0.0.0)` 视为本机 + connect 目标 trace(§6A.6) |
| `f553308` | util: 所有日志级别都带 `[pid:tid]`(§4.1、坑 #9) |
| `4ed92f0` | wsock: AF_UNIX `connect` 缺路径回 ENOENT,`getgrouplist` 才肯读 `/etc/group`(§6B.1) |
| `144c2ea` | userland: 创建文件应用 umask,私钥不再 world-readable(§6B.2) |
| (本轮) | 文档:新增 §6B,openssh_e2e 6/7,剩下 `-d -d` 一条(§6B.4) |


---

# 第三轮:四个 `fatal()` 各吃掉一条连接(已修)

## 6A.0 先说方法:这一轮为什么快

前两轮都在**读 trace 猜语义**,反复误判(§3 的坑 #1、§6.3.1 都栽在"拿 trace 里的
现象反推原因")。这一轮换了做法,一步就定位:

> **去读 sshd 的源码,并把 guest 二进制里的 format string 抠出来。**

`sshd_debugging_exp.md` 记的是"怎么查",但这一轮真正的教训是"**先去查清楚那个
日志行是 `fatal()` 还是 `debug()`**"。`debug1:` 前缀让人以为那只是日志;实际上
`check_ip_options()` 里那句是 `fatal()`,进程当场 `cleanup_exit(255)`。这一个事实
解释了此前所有观测:没有 banner、没有 I/O、客户端只等到 RST、`wait` 得到 255。

具体做法(可复用):

```powershell
# 1) 从 guest 二进制里抠 format string,确认日志行的确切形状
$b = 'release.windows.x86_64\runtime\rootfs\usr\sbin\sshd'
$txt = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($b))
[regex]::Matches($txt, '[\x20-\x7e]{0,60}rexec start in[\x20-\x7e]{0,80}') | % { $_.Value }
#   -> rexec start in %d out %d newsock %d config_s %d/%d

# 2) 拿同版本源码对答案(注意 tag要对上 V_10_3_P1)
Invoke-WebRequest https://raw.githubusercontent.com/openssh/openssh-portable/V_10_3_P1/sshd-session.c
#   -> check_ip_options(): 那行是 fatal(),不是 logit()
```

第二条命令还顺带纠正了一个一直没人核对的数字:`config_s 9/10` 的 `9/10` 是
**config socketpair 的两端**,不是别的意思。

## 6A.1 `getsockopt(IPPROTO_IP, IP_OPTIONS)` 返回 4 字节垃圾 → `fatal()`

- 现象(guest 日志最后一行,之后再无输出):

  ```
  Connection from 127.0.0.1 port 63222 with IP opts:  80 00 00 00
  ```

  进程 `exit 255`,客户端 `kex_exchange_identification: Connection reset by peer`。

- 根因:`sshd-session.c` 的 `check_ip_options()` 读
  `getsockopt(sock_in, IPPROTO_IP, IP_OPTIONS, opts, &option_size)`,**只要
  `option_size != 0` 就 `fatal()`**(Linux 上把 IP options 视为源路由攻击)。
  WinSock 的这个 get **成功返回 4,但不填调用者的缓冲区**,于是 guest 读回自己栈上
  的残留 —— `80 00 00 00` 就是未初始化栈,不是对端发来的东西。Linux 对普通 TCP
  对端返回 0,所以真机上永远不会走到 fatal。

- 定位过程中最关键的一步,是让 trace 打出**实参**而不是指针:

  ```
  INFO: sys_getsockopt(6, 0, 4, 3ffff9a0, 3ffff870)
  ```

  `optname = 4`,而 **WinSock 的 `IP_OPTIONS` 是 1**。也就是说这条调用根本没被
  拦在映射层,是原样打到宿主上的。第一次修的时候按 WinSock 的值写了常量 1,
  结果 `-d -d` 依旧 fatal,daemonize 路径反而从 2/8 变成 8/8 —— 正是这个
  "一半好了" 的信号指出常量取错了。Linux `netinet/in.h` 是
  `IP_TOS 1 / IP_TTL 2 / IP_HDRINCL 3 / IP_OPTIONS 4`。

- 修复:`src/win/win_socket.c` 的 `win_socket_getsockopt()` 对
  `(IPPROTO_IP, IP_OPTIONS)` 合成 Linux 的答案(`*len = 0`,返回 0)。
  guest socket 是直通的 loopback 转发,不可能带上 IP options,所以"没有"是唯一
  正确的答案。

- 效果:日志变成正常形态 `Connection from ... on 127.0.0.1 port 2222`,进程继续往下走。

## 6A.2 `chroot(2)`(syscall 51)未实现 → ENOSYS → fatal

- 现象:`chroot("/var/empty"): Function not implemented`,紧接
  `monitor_read_log: child log fd closed` / `mm_reap: child exited with status 255`。
- 定位:ENOSYS = 38,trace 里对应 `Syscall 51 failed: -38`;riscv64 musl
  `bits/syscall.h` 里 `__NR_chroot 51`(注意 **51 在 asm-generic 里也是
  `__NR_fchmod`**,极易看错)。
- 修复:`rvvm_sys_chroot()`(`src/core/rvvm_user.c`)。宿主没有 chroot,而且 guest
  进程本来就有整个宿主文件系统,没有东西可以再收走,所以**只保证返回值的含义**:
  路径按 chdir 同样的方式解析和校验(不存在 → ENOENT,不是目录 → ENOTDIR,是目录 →
  成功),root 本身不动。
- ⚠️ 这意味着**依赖 chroot 做文件隔离的 guest 在这里拿不到隔离**。OpenSSH 只是
  chroot 之后就不再碰文件系统,所以对它无影响;这一点已在代码注释里写明。

## 6A.3 guest 凭证不分 real / effective id → `permanently_set_uid()` 自查失败

- 现象:

  ```
  debug1: permanently_set_uid: 22/22
  permanently_set_uid: was able to restore old [e]gid
  ```

  OpenSSH 在丢掉特权后会**故意验一次"特权真的回不来了吗"**,回得来就 `fatal()`。
  这是安全设计,不是它挑刺。

- 根因:`USERLAND_DEFAULT_FAKE_ROOT` 默认 true,于是
  `rvvm_sys_setuid()` 无条件 `ctx->fake_uid = uid; return 0;` —— `setuid(0)`
  在丢掉特权之后照样成功。另外 `geteuid`/`getegid` 直接返回 `getuid`/`getgid`,
  `setresuid`/`setresgid` 忽略另外两个参数(注释里自称 "semi stub")。

- 修复:`rvvm_userland_t` 增加 `fake_euid`/`fake_egid`,和 real id 分开。规则照 Linux:
  euid 为 0 才能设全部三个 id;非特权的只能设自己已经持有的 id,其余 EPERM
  —— `setuid(0)` 因此自然失败,不需要为 sshd 特判。顺带把
  `geteuid`/`getegid`/`setresuid`/`setresgid`/`getresuid`/`getresgid` 补成真的。
  子空间在 `userland_child_create()` 里继承这四个字段。

## 6A.4 `prlimit64(2)`(261)写死 EINVAL → sandbox fatal

- 现象:`ssh_sandbox_child: setrlimit(RLIMIT_FSIZE, { 0, 0 }): Invalid argument`。
- 根因:`case 261` 原本是 `a0 = -UAPI_EINVAL;`(上一行 `prlimit()` 还被注释掉了)。
  musl 的 `getrlimit`/`setrlimit` **都走 prlimit64**,所以这条一失败,sandbox
  子进程当场 fatal。
- 修复:`rvvm_sys_prlimit()`,按地址空间记 soft/hard,执行 Linux 的规则
  (硬上限不可抬高 → EPERM;硬上限压到软上限以下时软上限跟着降),`-1` 表示不改。
  同样**只记录不强制** —— 和 chroot 一个理由:模拟器已经把整个宿主文件系统交出去了,
  卡文件大小并不额外收走任何访问权。
- 只支持 `pid == 0`(自身)。别的 pid 直接 `ESRCH`:限额挂在地址空间上,这里没有
  按 pid 查找,与其拿本进程的数去回答另一个进程的查询,不如明确拒绝。

## 6A.5 当前阻塞点:`initgroups()` 失败,errno 是陈旧值

修完上面四条,guest 日志能一路走到公钥校验:

```
debug1: kex_server_update_ext_info: Sending SSH2_MSG_EXT_INFO
debug1: userauth_pubkey: publickey test pkalg ssh-ed25519 ... SHA256:ffRn6CUCLsSBqyX80o0Y4Urlp+cORGTFmrOb0/j8DQI
debug1: temporarily_use_uid: 0/0 (e=0/0)
initgroups: root: Network is down
```

`Network is down` = ENETDOWN = 100。而**这轮里唯一返回 100 的 syscall 是两次
无关的 UDP `connect()`**:

```
TRACE[104:104]: connect fd 6, 16 byte(s): 02 00 ff ff 00 00 00 00 00 ... -> inet 0.0.0.0:65535
TRACE[104:104]: Syscall 203 failed: -99 / -100
TRACE[104:104]: connect fd 6, 28 byte(s): 0a 00 ff ff 00 00 00 00 00 ... -> family 10 (::)
TRACE[104:104]: Syscall 203 failed: -99 / -100
TRACE[104:104]: connect fd 7, 110 byte(s): 01 00 "/var/run/nscd/socket"
```

三次都是**连上就关、一个字节都不发**的探测:UDP v4 → UDP v6 → nscd。errno 在两次
`connect()` 之间被改掉,而 OpenSSH 报的是 `initgroups` 的 errno。

**坑 #9（本轮自己踩的）**:`INFO:` 行不带 `[pid:tid]`。第一版 trace 打在 INFO 通道上,
于是"这是哪个进程的 connect"根本看不出来,差点把不同进程的调用混成一条线索。**要归因
就用 `RVVM_TRC`**。

排除掉的方向(都已实测):

- group/passwd 数据没问题:`/etc/group`、`/etc/passwd` 完整,`sshd:x:22:22` 在。
- 不是 DNS:`getent hosts example.com` 在 guest 里正常解析,而且解析器用的正是
  `socket(AF_INET6, SOCK_DGRAM|…)` + `connect` 这个形状 —— 只是地址是真的
  (`2606:4700::`)。所以那两个 `0.0.0.0:65535` 不是解析器。
- busybox 的 `groups root` / `id -G root` 正确,只发一次 nscd 的 AF_UNIX connect。

### 6A.5.1 反汇编把 initgroups 本身排除了

`objdump` 在 msys64 里不认识 riscv64,但 guest 的 `lib/ld-musl-riscv64.so.1` 就在
`runtime/rootfs` 里,`nm -D` 能读符号表(`initgroups` @ `0x34278`),python 的 capstone
配 `CS_MODE_RISCV64|CS_MODE_RISCVC` 能反汇编(ELF 头/`.dynsym`/重定位手写解析即可)。
于是直接把两个函数拆开看:

```
initgroups:  malloc → getgrouplist → setgroups → free        # 没有任何 socket
getgrouplist: 入口有一个 AF_NETLINK 探测,之后 fopen/fread/strcmp 解析 /etc/group
```

**结论:`initgroups` 和 `getgrouplist` 都不发 UDP 包。** 更关键的是,失败那轮里
**`/etc/group` 根本没被 open**(`sys_openat` 里没有它,紧接着就去读
`/etc/ssh/ssh_host_rsa_key` 了)—— 说明 `getgrouplist` 在走到自己的 `fopen` 之前
就返回了负值,而它没有为此设置 errno。

所以剩下的未知收窄成一件事:**那两次 UDP 探测是谁发的**。它既不属于 `initgroups`,
也不属于 `getgrouplist`,也不是解析器。

下一步的取法(本机都不通,需要换环境):

- musl 源码:`git.musl-libc.git` TLS 握手失败,`sources.debian.org` 有 PoW 墙,
  GitHub 上的 musl 镜像路径全部 404。
- 或者:起一个 Ghidra 实例,把 `ld-musl-riscv64.so.1` 按 `RISC-V:LE:64` 导进去,
  从那两个探测点往回追调用者。

**新加的坑 #7**:

> **musl 的失败可能报一个陈旧 errno。** `initgroups: root: Network is down` 里的
> errno 是**前面某次失败的 `connect()` 留下的**,不是 initgroups 自己的原因。看到一个
> 语义上不该出现的 errno(查 group 怎么会是网络错),先回头找最近一次失败的 syscall,
> 别顺着 errno 名去猜。

## 6A.6 顺带修掉的第二个模拟器缺陷:`connect(0.0.0.0)`

探测用的 `sockaddr` 全是零,只有 `sin_family` 和 `sin_port = htons(-1) = 0xffff`
非零。这不是 guest 的毛病,是宿主语义:

- Linux `__ip4_datagram_connect()` 会把 `INADDR_ANY` 改写成 `INADDR_LOOPBACK`,
  `__ip6_datagram_connect()` 对 `in6addr_any` 同理 —— 所以**数据报 socket 连
  0.0.0.0 是成功的**,含义是"本机"。
- WinSock 没有这条规则,回 `WSAEADDRNOTAVAIL`。

`win_socket_connect()` 现在照 Linux 的做法替换成 loopback。**这不是 `initgroups`
的原因**(修完两次探测都成功了,`initgroups` 照旧失败),但它本身是个真的语义缺口,
而且那个 errno 正是会被误报出去的东西。

顺带把 `sys_connect` 的目标 trace 从 INFO 通道挪到 `RVVM_TRC`(见坑 #9),现在会打出
地址/端口/路径和原始字节:

```
TRACE[104:104]: connect fd 6, 16 byte(s): 02 00 ff ff 00 00 00 00 00 00 00 00 00 00 00 00 00 -> inet 0.0.0.0:65535
TRACE[106:106]: connect fd 3, 16 byte(s): 02 00 08 ae 7f 00 00 01 00 00 00 00 00 00 00 00 -> inet 127.0.0.1:2222
TRACE[104:104]: connect fd 7, 110 byte(s): 01 00 2f 76 61 72 2f 72 75 6e 2f 6e 73 63 64 2f -> unix "/var/run/nscd/socket"
```

一个 guest 根本没打算发起的 `connect()`,和一次被宿主拒绝的 `connect()`,以前在
trace 里长得一模一样。

## 6A.7 回归

`procfs_e2e` 22/22、`session_e2e` PASS(含 `^C`/`^Z`/`jobs`/kill)、`ash_e2e` 7/7
—— A.3 改了 `stat()` 报出来的 owner id(用的是 `fake_uid`,即 real id,没动),
`/proc/self/status` 的 Uid 与 `ps` 输出均未受影响;A.6 动的是 `connect()` 的
目标地址替换,guest 侧所有 connect 路径都过一遍,`session_e2e` 的 AF_UNIX 监听
和 `apk` 的 HTTPS 都照常。

---

# 第四轮:打通了 —— 6/7

## 6B.0 两个新缺陷,都是"静默地错"

`initgroups` 那条查到最后不是 guest 的问题,是我们把 errno 答错了。**这两个都属于
最难查的一类:不报错,只是结果不对,而且对大部分程序无害。**

### 6B.1 AF_UNIX `connect()` 缺路径必须是 ENOENT

拿到 musl 源码(`gh api repos/ifduyue/musl` —— 注意目录是 `src/passwd/`,不是
`src/grp/`)之后,`__nscd_query()` 一眼就说明了:

```c
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		/* If there isn't a running nscd we simulate a "not found" result */
		if (errno == EACCES || errno == ECONNREFUSED || errno == ENOENT) {
			errno = errno_save;
			return f;
		}
		goto error;          /* 任何别的 errno -> 返回 NULL */
	}
```

而 `getgrouplist()` 拿到 NULL 就直接 `goto cleanup` 返回 -1,**连 `/etc/group`
都不会去开**。WinSock 对一个不存在的 unix 路径回 `WSAENETDOWN`,不在那三个之列。

**这影响的是每一个 guest 进程,只是大部分程序不 care:**

```
guest 里 $ id -G root
0                    ← root 明明在十几个组里;真机上是一长串
```

busybox 自己解析 `/etc/group`(所以它的 `id -G` 一直"看起来正常"),`getgrouplist`
只有 OpenSSH 这种把它当前置条件的程序才会炸。修法是照 Linux 的语义自己做存在性
判断,不赌宿主会回哪个码。

### 6B.2 创建文件不应用 umask

修完 6B.1 之后,握手已经全通,客户-side 报了另一件事:

```
Permissions 0644 for '/root/.ssh/id_e2e' are too open.
Load key "/root/.ssh/id_e2e": bad permissions
root@127.0.0.1: Permission denied (publickey,keyboard-interactive).
```

`umask` 在 guest 里是 0000,于是去 trace 里看 `ssh-keygen` 怎么建的文件:

```
sys_umask(3f)                              ← umask(0177)
sys_openat(-100, /root/.ssh/k1, 8241, 1a4) ← mode 0644
sys_umask(0)
```

**`ssh-keygen` 故意不传 0600,而是靠 umask。** 内核会 `mode & ~umask`,所以真机上
私有 key 是 0600;我们的 `openat` 把调用方的 mode 原样记下来,而 `umask(2)` 被转发
给了宿主 CRT —— 那边的值这里根本没人读。两个缺陷叠加,私钥就 world-readable 了。

顺带也是个小隔离问题:转发的 `umask()` 改的是 **CRT 自己的** umask,也就是宿主侧
建文件时遵守的那个,guest 能去动它。现在改成 per-address-space(和 Linux 的
per-process 一致,fork 继承),并在 `openat`/`mkdirat`/`mknodat` 上套用。

## 6B.3 现在的状态

```
ok   the core (ash --serve) publishes its endpoint
ok   install: openssh present in the guest (apk)
ok   setup: host keys, privsep dir/user, client key authorized
FAIL sshd (single-connection debug) serves a pubkey session
ok   sshd starts and daemonizes (fork/setsid)
ok   sshd serves a pubkey session and runs a remote command      ← openssh-e2e-ok
ok   a second session reaches the same running daemon             ← openssh-e2e-ok2
```

**客户端 + sshd 打通了**:真的公钥会话、真的执行远端命令、真的第二个会话复用同一个
守护进程。回归:`procfs_e2e` / `ash_e2e` / `session_e2e` / `jobctl` 全 PASS。
`session_e2e -Multi` 的 4 个失败**是既有的**——已在改动前后各跑一次对比确认一致。

## 6B.4 剩下的一条:`sshd -d -d` 调试模式

契约已经从源码确认(`sshd-session.c` 的 `privsep_preauth()`,以及 `sshd-auth.c`
的 `main()`):

```c
	/* 子进程必须摆成: 0/1 = 网络 socket, 3 = monitor socket, 4 = log socket */
	if (... != STDIN_FILENO && dup2(ssh_packet_get_connection_in(ssh), STDIN_FILENO) == -1)
		fatal("dup2 stdin failed: %s", strerror(errno));
	...
	closefrom(PRIVSEP_MIN_FREE_FD);      /* = 5 */
	execv(options.sshd_auth_path, saved_argv);
```
```c
/* sshd-auth.c: 连接就是 stdin */
	sock_in = sock_out = dup(STDIN_FILENO);
	debug("network sockets: %d, %d", sock_in, sock_out);
```

现象是这两行:

```
debug1: network sockets: -1, -1 [preauth]
main: fcntl(-1, F_SETFD, FD_CLOEXEC): Bad file descriptor [preauth]
```

### 6B.4.1 `dup(0)` 拿到的是 EBADF,不是 pty

`case 23: dup` 上补了一条 trace(打出槽位说的和解析出来的),一次就定了性:

```
TRACE[108:108]: dup: fd=0 tracked=1 backend=0 slot_fd=16 resolved=-1 gen=30
```

**`resolved=-1`** —— 槽位记的是 host 16、gen 30,但 `userland_fd_host()` 因为
**anchor 的 generation 对不上**而返回 -1(就是 `ac9041b` 加的那道 `fd_stale` 保险)。
所以 `dup(0)` 回 EBADF,`sshd-auth` 的 `dup(STDIN_FILENO)` 自然是 -1。

(顺带更正:在加这条 trace 之前,我从 trace 的先后顺序推断"`dup(0)` 复制出了一个
pty"——**那是错的**。`sys_dup(0)` 后面那条 `fd_wr[install]` 是 `stdfd_devnull()` 开
`/dev/null`,不是 dup 的结果。教训还是 §3 坑 #1。)

### 6B.4.2 根因:anchor 引用计数失衡,号码被提前还给 CRT

为什么 gen 对不上?把这一段 anchor 的生命周期全捞出来:

```
[581044969] [104] close anchor 16 (socket 0x1cc)
[581044969] [104] free anchor 16 (last reference)   ← unref #1 → 计数归 0,_close(16)
[581044995] [104] free anchor 16 (last reference)   ← unref #2,中间没有任何 alloc
[581044995] [108] alloc anchor 16                   ← _open("NUL") 把 16 原样发回来
[581044997] [108] alloc anchor 16                   ← ★ 又一次,而 fd 0 还指着它
```

`win_socket_alloc_anchor()` 是 `_open("NUL", ...)` —— **它问 CRT 要号,而 CRT 只认
自己的空闲表**。引用计数(`wsock_fds_refs`)只能保证 anchor 的 CRT 句柄在最后一个
使用者消失前不被 `_close`,一旦被 `_close` 过,_open 就会把同一个号再发出来。

这里 pid 104( sshd monitor)**对同一个 anchor 释放了两次**,中间没有 alloc。
多出来的那次 unref 说明**某条 close 路径把引用放多了** —— 这是要查的地方。

### 6B.4.3 试过并否掉的一个改法

第一反应是给 `wsock_anchor_release()` 加护栏:计数已经 ≤0 时不再 `_close`
(`--0` 会变成 -1,被 `<= 0` 判成"最后一个引用",于是对**可能已经被别的 `open()`
重新持有的 CRT 号**再关一次)。**这个改法让 e2e 从 1 FAIL 变成 2 FAIL,已回退。**

回退的理由很有价值:**"计数为 0 时也关"是 load-bearing 的**——
`win_socket_free_anchor()` 的注释写明它服务于"不是 guest 持有的槽位(分配失败、
dup 没能绑定)"这种路径,那时计数就是 0,需要一次真正的 `_close` 收干净。
所以护栏不能加在 release 侧,**要修的是那个多出来的 unref**。

**下一步**:找出 pid 104 在 26ms 内对同一个 anchor 释放两次的那条路径
(`userland_fd_close()` / `win_socket_close()` / inherit 三者之一)。
判据已经有了:`free anchor N (last reference)` 在没有 `alloc anchor N` 的情况下
出现第二次。`dup` 的那条 trace 建议保留 —— 它把"槽位说的"和"实际解析出来的"
分开打出来,正是它把这一条从"猜"变成"看"。

### 6B.4.4 读 trace 时的陷阱:`alloc anchor N` 不等于"新建了一个 anchor"

`win_socket_alloc_anchor()` 是 `_open("NUL", ...)`,**它拿到的号很可能是回收来的** ——
`_open` 只认 CRT 的空闲表。所以那一行表示"取到了一个号",不表示"产生了一个 anchor";
真正让 anchor 存在的是 `wsock_fd_alloc()` 里的 `fd N <- socket ... (gen G)`。

我第一遍就是把这两行当成一回事,才得出"anchor 被分配了两次中间没有释放"这个
(表面上很吓人的)结论。**实际上中间确实少了一次释放,但那 45 条"没有 alloc 就 free"
里绝大多数只是我的统计窗口太短 —— 长命的 socket 分配发生在几百行之前。**
判据要按 `fd N <- socket (gen G)` 来算,不是按 `alloc anchor N`。

trace 已改名为 `take crt number N (reused if it comes round)`,以免下一个人再踩。

## 6B.5 同一个 bug 也会打到守护进程路径(所以它会飘)

连跑几次 `openssh_e2e`,`sshd serves a pubkey session` 这一条时而通过时而失败。
抓到的失败 transcript 与 §6B.4 **完全同一个签名**:

```
debug1: network sockets: 6, 6
debug1: network sockets: -1, -1 [preauth]
main: fcntl(-1, F_SETFD, FD_CLOEXEC): Bad file descriptor [preauth]
```

也就是说:**不是"调试模式特有"的问题,而是同一个 anchor 记账失衡,只不过在守护进程
路径上取决于分配时序,有时撞上有时撞不上。** 这也和"同一个号被发给两个活槽位"
的性质一致 —— 是时序相关的,不是必现的。

所以现在该做的事没变,而且更重要了:**这个 fd 记账问题不修,`openssh_e2e` 就不能算绿**,
因为它已经会让一个曾经通过的检查随机失败。

**建议的下一步**(比继续读代码可靠):在 `wsock_anchor_ref()` / `wsock_anchor_release()`
上加一个"每个号的有引用槽位数"计数,和实际记账对账。失衡的那一刻会自己报出来是哪
个号、计数是多少,而不是等它变成一个远端的 EBADF。这比现在这样从症状倒推便宜得多。



