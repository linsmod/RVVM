# userland fd 设计现状

最后核对：2026-11-18（P3：防御机制已删，对应 `src/core/rvvm_user.c` 工作树状态）

本文只描述**现在是什么**：guest 的 fd 号如何映射到宿主句柄，fork / close / execve 各自做了什么。修复过程与未解决的问题见 `../issues/fork-shared-crt-fd-space.md`。

## 1. 三层编号

| 编号 | 取值范围 | 存在哪里 | 谁分配 |
|---|---|---|---|
| guest fd 号 | `0 .. USERLAND_FD_TABLE_MAX-1`（4096） | 进程 ctx 的槽表下标 | `userland_fd_slot_alloc(min_fd)`：从 `min_fd` 起找最低空槽 |
| 宿主号 | Windows CRT fd | 槽的 `fd` 字段 | 宿主 `open` / `dup` / `socket` |
| 锚号（socket anchor） | `0x40000000 + index`（`WSOCK_ANCHOR_BASE` 起） | `wsock_fds[]` + `wsock_fds_gen[]` + `wsock_anchor_taken[]` | `win_socket_alloc_anchor()` |

关键点：**锚号是独立号段**，不与 CRT 号重叠。锚背后不再有任何 CRT 句柄（B1 删除了每锚一个的 `_open("NUL")`——该句柄只被写和关、从不被等待，却占着真实 CRT 号）：

```c
wsock_fds[i]     = s;
wsock_fds_gen[i] = InterlockedIncrement64(&wsock_fds_gen_next);   // 永不为 0
return WSOCK_ANCHOR_BASE + i;
```

因此「这个号是不是锚」由区间判定（`wsock_index(fd) >= 0`），不再需要「某个 CRT 号恰好被登记」这种猜测。槽仍记录 `anchor_gen`，但它是**纯诊断字段**（fd_wr trace 打印），没有任何判定读它。

`socket` 在 `userland_own_fd()` 里算「自有」——它不是宿主描述符，不能进宿主 `poll()`/`select()`；而槽的 `backend` 仍记 `HOST`，因为 close 要走 `win_socket_close()`。这两个判定分成 `userland_own_fd()` 与 `userland_sim_fd()` 两个函数，混用会把 socket 槽标成 SIM、让 close 不再关 socket。

## 2. 槽结构

`rvvm_fd_entry_t`，每个进程一张 `fds[USERLAND_FD_TABLE_MAX]`：

| 字段 | 含义 |
|---|---|
| `fd` | `HOST`：宿主号；`SIM`：本层自己的号；`CONSOLE`：不用 |
| `backend` | `FD_BACKEND_HOST=0` / `FD_BACKEND_SIM=1` / `FD_BACKEND_CONSOLE=2` |
| `cloexec` | `FD_CLOEXEC`，`execve` 会关掉该槽 |
| `used` | 槽是否被占 |
| `flags` | guest 的 open-file 标志字（仅 `SIM` 有值，HOST 去问宿主） |
| `anchor_gen` | 归档时的锚代数；0 = 不骑锚（console、普通 host fd、模拟对象）。**纯诊断**，无判定读它 |
| `holds_anchor_ref` | **本槽**是否持有锚引用（与 `win_socket_anchor_ref/unref` 配对，不看数字长相） |
| `path` | 打开时的 guest 路径，仅供 `/proc/<pid>/fd/<n>`；每槽一份独立字符串 |

`SIM` 覆盖：pty 端、`/dev` 项、合成的 `/proc` 文件、socket 锚、memfs 项。

## 3. 解析：guest 号 → 宿主号

`userland_fd_host(ctx, fd)` 纯查表：

1. 未跟踪的号：原样返回（视为「同号的宿主 fd」；close 对这种号另有约束，见第 5 节）。
2. 跟踪的槽：返回槽里的 `fd`。号私有保证槽存活期间没人动它，无 stale 判定。

所有 `read` / `write` / `fcntl` / `poll` 都先走这个解析。

## 4. fork：`userland_fd_table_inherit(child, parent)`

guest 的 `fork` 在宿主进程里用线程模拟，父子两个 ctx 各有槽表，但底层是**同一张 CRT 表**。逐个父槽复制到子槽，按类型分派；**子侧一律拿到自己的号**，不再整段复制父槽的号：

| 类型 | 动作 |
|---|---|
| memfs | `rvvm_memfs_ref` + `pin`，子槽有自己的游标 |
| pty 端 | `userland_pty_ref`（共享同一 pty 对象） |
| `/proc` 对象 | `userland_proc_fd_ref` |
| `/dev` 项 | `userland_dev_by_fd` 只复制号（合成号，无需持有） |
| 普通 host fd | `dup(parent->fds[fd].fd)` → 写子槽；`dup` 失败则子槽无效（`used=false`），与 Linux fork 的 EMFILE 语义一致 |
| socket 锚 | `win_socket_dup()` 出新锚号（SOCKET 对象带引用计数） |

路径名一并复制（父子的 `/proc/<pid>/fd/<n>` 各答各的）。

## 5. close：`userland_fd_close`

按槽的内容分派，**不按数字长相**：

| 情况 | 动作 |
|---|---|
| console 槽 | 只清槽（console 是 attach，不是句柄） |
| memfs 号 | `memfs_fd_release`（还 pin 与存储引用） |
| pty 号 | `userland_pty_release` |
| `/proc` 号 | `userland_proc_fd_release` |
| `win_socket_is_fd(fd)`（锚号区间判定） | `win_socket_close(fd)`：关 SOCKET，锚号按引用计数归还 |
| 其余（普通 HOST） | `host_fd_give()`：槽不再命名这个号就直接关闭它 |
| **未跟踪号**（`userland_fd_close` 返回 false） | 只有 asset 挂载发的裸宿主号按号 `close()`（`asset_fd_take` 弹出）；其余回 `EBADF`，**不碰任何宿主号** |

最后统一 `userland_fds_write(ctx, fd, used=false, ...)` 清槽。

## 6. 宿主号的所有权

宿主号**按槽私有**：一个号只属于一个 address space 里的一个槽。fork 用 `dup()` 给子进程另做一个号，`dup2` 复制时也另做，`open()` 给出一个没有别处命名的号。因此关闭一个槽就关闭它的号，不需要问「还有谁在用」。

- `host_fd_owned(host, backend)`：`backend == HOST && 0 <= host < HOST_FD_MAX(4096)`——这个号是否由槽负责关闭。锚号在独立号段（≥ `WSOCK_ANCHOR_BASE`），区间已排除；合成号不进 CRT，由 backend 排除
- `host_fd_give(host)`：槽不再命名这个号就直接 `close()`（在 `crt_lock` 内）
- `host_fd_alive(host)`：号是否还开着（Windows 走 `_get_osfhandle`），写槽后校验，warn-only 断言，抓「槽指向已关闭的号」
- `crt_lock` 覆盖 `_dup` 与 `_close` 这一对，使「号空闲」和「号归还」是一个原子步骤

号只在 `userland_fds_write()` 里迁移：旧号 `give`、新号 `take`；**同一个号重写不迁移**（`set_flags`/`set_cloexec` 重写同一个槽，不能因此关掉它描述的号）。

> 一处仍未定位的缺口：某个路径在装槽之后关闭了宿主号（`shared=0` 时也会发生），见 `task-fd-ownership.md`。

## 7. 读 / 写路径

- `sys_read` / `sys_write`（case 63 / 64）自己解析 `userland_fd_host()`，然后：
  1. memfs 槽 → `rvvm_memfs_read/write_at`
  2. `userland_pty_by_fd` → `userland_pty_read/write`
  3. `userland_dev_by_fd` → `userland_dev_read/write`
  4. `ctx->console_sink` → 回调截获（宿主注册的 sink 收到的是**宿主号**）
  5. 否则 `read/write(host_fd, ...)`
- `userland_write_fd()` **只**被 splice 式拷贝调用（`rvvm_user.c` 的 read→write 循环），不在普通 `write` 系统调用路径上。
- guest 0/1/2 是 `CONSOLE` 槽时，写先喂 `user_tty_write`（VTerm 解析器），再进 sink / 宿主 fd。

## 8. execve

`userland_fd_exec_close(ctx)` 只清 `cloexec` 槽。**不重建 fd 表**，`HOST` 槽的宿主号在 exec 前后保持不变（`fd_dump[execve_pre]` 与 `[execve_post]` 逐项相同）。

## 9. tinyssh 侧的接口面

会话 fork 走 `third_party/tinyssh/channel_fork.c`：

- 三条管道由 `open_pipe()` 建好，两端都设 `FD_CLOEXEC`、切非阻塞；`channel_fork` 取 `pa[]`（父端）与 `ch[]`（子端）；
- 子进程：`close(pa[i])` → `close(i)` → `blocking_enable(ch[i])` → `dup(ch[i])` 必须等于 `i`，否则 `_exit(111)`；
- 之后 `p_closefrom(3)`。本构建走 `hasclosefrom.h` 的 `#undef HASCLOSEFROM` 分支：先 `3..255` 逐个 `close()`，`dirfd` 有效时再 `fcntl(F_CLOSEM)`。

`dup(ch[i])` 落到哪个宿主号由 CRT 决定，可能与父进程任何槽同号——这正是第 6 节那张计数表要处理的情况。

## 10. 时序案例：vpssh 会话

以 `ssh -p 7900 127.0.0.1 "echo HELLO"` 走的那条链路为样本：`vpshhd` 拉起 `tinysshd`，认证通过后 `channel_fork()` 产生会话子进程，子进程 `execve` 成 `sh`。下面每个阶段都标出**槽记的是什么**与**宿主号的实际归属**，这两个在时序上是分开的。

### 10.1 参与方与拓扑

```
ssh 客户端 ──TCP 7900──> vpshhd ──> tinysshd(会话主, 如 pid 102)
                                          │ fork + execve
                                          └──> sh(如 pid 104)
                            三条 pipe：fd0 / fd1 / fd2
                            父端在 102 的槽里，子端在 104 的槽里
```

编号随 CRT 分配变化，本节所有数字都是**某一次运行**，只用来说明位置关系，不要当作固定值。

### 10.2 阶段：从 accept 到 sh 运行

| # | 动作 | 102 的槽 | 104 的槽 | 宿主 CRT |
|---|---|---|---|---|
| A1 | `open_pipe()` ×3 | 六个槽记三对管道端（HOST），槽位随当前占用而定（实测见过 5、6、7） | — | 六个宿主号被占 |
| A2 | `fork()` → `userland_fd_table_inherit` | 不变 | 父槽表逐项复制；普通 HOST 槽各 `dup()` 一次，锚槽 `anchor_ref` | 子侧另得一批号 |
| B1 | 子 `close(pa[i])` | — | 关掉继承来的父端槽 | 那几个号回收到空闲表 |
| B2 | 子 `close(i)` → `blocking_enable(ch[i])` → `dup(ch[i])` | — | `fd 0/1/2` 改记 `dup` 的结果 | `dup` 落到哪个号由 CRT 决定，**可能与 102 的任何槽同号** |
| B3 | 子 `p_closefrom(3)` | — | `3..255` 全清 | 中间号全部回收 |
| C1 | 子 `execve(sh)` | — | 只清 `cloexec` 槽；**宿主号不变** | 不变（`fd_dump[execve_pre]` 与 `[execve_post]` 逐项相同） |
| C2 | sh `write(1, ...)` | — | `userland_fd_host(1)` 返回槽里的号，再 `write(host)` | 数据进管道 |
| C3 | 102 `poll` | 管道读端 `POLLIN` | — | — |
| C4 | 102 `read` → `packet_channel_send` | — | — | 数据出 socket |

关键点在 B2：**`ch[i]` 是 102 槽里那个号，`dup(ch[i])` 在 104 里得到一个新号**。这一步之后，父子两侧各有自己的号指向同一根管道——所有权是清楚的，问题出在 A1/A2 留下的旧号上：102 的槽仍记着 A1 时的号，而那个号可能已经被 104 的 `dup` 重新用掉。

### 10.3 竞态：父的 close 落在子的 dup 之后

`$env:RVVM_TRACE=fd` 抓到的一段（父 102 / 子 104）：

```
[102:102] fd_close_host pid=102 fd=5 host=8
[102:102] fd_close_host pid=102 fd=5 host=8      ← 同一个槽重复关闭
[102:102] fd_close_host pid=102 fd=5 host=8
[104:104] dup(7) slot_fd=18
[104:104] install fd=1 host=8                    ← 子拿到 8，槽记 8
[104:104] close(8) host=19                       ← 这里关的是 guest 8（宿主 19）
[104:104] host_release host=8 (last ref, closing) ← 计数归零，8 真正被关
[104:104] fd_dump[execve_post] fd=1 host=8       ← 槽里仍然写着 8
```

三件事叠在一起：

1. `pid=102 fd=5 host=8` 被父侧关了**三次**（槽复用后重新 open 再 close，每次按槽里记住的数字再关一遍）；
2. 104 的 `dup` 把同一个 8 拿走，写进 `fd=1`；
3. 父侧最后一次 release 把计数打到 0，`close(8)` 执行——此时子进程的 `fd=1` 仍指向 8。

结果是 `execve` 之后 sh 的 `write(1)` 得到 **EBADF**。另一次运行的同一现象是 `sys_write(guest fd=1 host=11) = -9 (errno 9)`、`sys_write(guest fd=2 host=13) = -9 (errno 9)`，那次管道端落在 11/13 而不是 8——现象相同，只是宿主号不同。而槽、`fd_dump`、`/proc/<pid>/fd` 三处都还显示那个号，看起来完全正常。

### 10.4 读侧看到的只是结果

会话主循环的 `poll`（`main_tinysshd.c`，临时探针）在同一时刻：

```
[vptrace] ret=1 n=4 rev0=1 rev1=4 revfc1=0 fdfc1=6 revto=-1 sendlen=44 putready=1
[vptrace] direct read=-1 errno=11
```

- `revfc1=0`：子 stdout 的读端**没有任何事件**；
- 直读 `EAGAIN`：管道里确实没有数据。

所以「客户端收不到输出」不是 `poll` 漏报，也不是协议层没发送，而是**写端在 sh 写入之前就已经不存在了**。上游所有现象（会话 0 退出、`Connection reset`、`session 102 exited 0/111`）都是这一条的下游表现。

### 10.5 时序上的两个坑

1. **日志时间戳不能用来定序。** 父子在两个线程上，各自取 `TickCount64`；`102` 的 close 行可能打在 `104` 的 dup 行之前，实际作用顺序却相反。判断顺序只能看「槽内容 + 引用计数」，不能看行号。
2. **同一段日志里宿主号可能不是同一套。** 不同运行里管道端可能是 8/10，也可能 11/13；跨运行拼接片段会得出自相矛盾的结论。上面 10.3 与 10.4 是两次运行，只用来分别说明「关掉了什么」和「读到了什么」。

## 11. 已知设计约束

1. ~~父子共享一张 CRT 表。~~ 已解决到「号私有」这一层：`inherit` 不再把父槽的号整段复制给子槽，普通 HOST 号一律 `dup()`，memfs / pty / proc / dev 各自显式写槽；`host_fd_give()` 不看计数，槽不再命名这个号就关闭它。`host_fd_refs[]` 计数表与检测器已随 P3 删除。锚也不再占用 CRT 号（`wsock_nul[]` 已删除）。**「号被别人关掉」的最后一条路径也已闭合**：`sys_close` 对未跟踪号不再裸透传（只有 asset 挂载 fd 按号关闭，其余回 `EBADF`），guest 的 `p_closefrom` sweep 不再命中别的槽刚 `dup` 到的宿主号。证据与定位过程见 `task-fd-ownership.md` B2 一节。
2. ~~锚号与 CRT 号共用数字。~~ 已解决：锚号改为 `0x40000000 + index` 独立号段，`wsock_nul[]` 保存 NUL 句柄且不对外。按数字判断类型的写法不再必要，剩余的 `anchor_gen` 只用于识别同号换对象。
3. ~~引用计数的判定时刻有时差。~~ 已随「号私有」消解：关闭不再由计数决定，`host_fd_give()` 直接关掉槽自己的号，因此没有「当时算出的值」这种跨时刻判断。`crt_lock` 仍覆盖 `_dup` 与 `_close` 这一对，使「号空闲」与「号归还」是一个原子步骤。

后果与证据见 `../issues/fork-shared-crt-fd-space.md`；解法见第 12 节引用的两份文档。

## 12. 当前缺失（非现状）

第 1、2、3 条约束都已落地，B1 遗留的「装槽后号被未知路径关闭」缺口已在 B2 闭合（根因是 `sys_close` 对未跟踪 guest 号的裸透传，详见 `task-fd-ownership.md`），P3 已把 `host_fd_refs` / stale 判定 / `anchor_gen` 猜测分支删除。**唯一遗留**：fork 时 `ctx->asset_fds` 表继承的边界复核（asset fd 不进槽表，父子复制同号会共享宿主描述符；tinyssh 链路不触发，bundle app 链路需复核）。目标形态见 `design-fd-ownership.md`，历史问题证据见 `../issues/fork-shared-crt-fd-space.md`。

## 相关文件

- `src/core/rvvm_user.c` —— 槽表、`userland_fd_host`、`userland_fd_slot_alloc`、`userland_fds_write`、`userland_fd_close`、`userland_fd_table_inherit`、`host_fd_acquire/release`、`sys_read`/`sys_write`（case 63/64）
- `src/core/rvvm_user.h` —— `rvvm_userland_t` 与对外接口
- `src/win/win_socket.c` —— `win_socket_alloc_anchor`、`wsock_fd_get`、`win_socket_anchor_gen`、`win_socket_anchor_ref/unref`
- `third_party/tinyssh/channel_fork.c`、`open_pipe.c` —— 会话侧三条管道
- `third_party/tinyssh-config/hasclosefrom.h` —— `p_closefrom` 分支
- `docs/arch-userland.md` —— guest 侧整体（命名空间、挂表行、`/proc` 支持面）
