# userland fd 设计现状

最后核对：2026-11-18（对应 `src/core/rvvm_user.c` 工作树状态）

本文只描述**现在是什么**：guest 的 fd 号如何映射到宿主句柄，fork / close / execve 各自做了什么。修复过程与未解决的问题见 `../issues/fork-shared-crt-fd-space.md`。

## 1. 三层编号

| 编号 | 取值范围 | 存在哪里 | 谁分配 |
|---|---|---|---|
| guest fd 号 | `0 .. USERLAND_FD_TABLE_MAX-1`（4096） | 进程 ctx 的槽表下标 | `userland_fd_slot_alloc(min_fd)`：从 `min_fd` 起找最低空槽 |
| 宿主号 | Windows CRT fd | 槽的 `fd` 字段 | 宿主 `open` / `dup` / `socket` |
| 锚号（socket anchor） | 同宿主号区间 | `wsock_fds[]` + `wsock_fds_gen[]` | `win_socket_alloc_anchor()` |

关键点：**锚号不是独立号段**。`win_socket_alloc_anchor()` 用 `_open("NUL", _O_RDWR|_O_BINARY)` 先占一个真实 CRT 号，再由 `wsock_fd_alloc()` 把该号登记为 socket 锚并写一个非零代数：

```c
wsock_fds[fd]     = s;
wsock_fds_gen[fd] = InterlockedIncrement64(&wsock_fds_gen_next);   // 永不为 0
```

所以「锚号 11」和「管道 CRT 号 11」是**同一个数字、两套语义**，只能靠代数区分：`win_socket_anchor_gen(fd) != 0` 才是活锚。

因此判断「这个槽是不是指向锚」**不能**问 `win_socket_is_fd(host)`（它只看 `wsock_fds[fd]` 有没有 socket 对象），必须看槽自己记录的 `anchor_gen`。

## 2. 槽结构

`rvvm_fd_entry_t`，每个进程一张 `fds[USERLAND_FD_TABLE_MAX]`：

| 字段 | 含义 |
|---|---|
| `fd` | `HOST`：宿主号；`SIM`：本层自己的号；`CONSOLE`：不用 |
| `backend` | `FD_BACKEND_HOST=0` / `FD_BACKEND_SIM=1` / `FD_BACKEND_CONSOLE=2` |
| `cloexec` | `FD_CLOEXEC`，`execve` 会关掉该槽 |
| `used` | 槽是否被占 |
| `shared` | 仅 HOST：骑在别的地址空间的宿主 fd 上，**从不在此 close** |
| `flags` | guest 的 open-file 标志字（仅 `SIM` 有值，HOST 去问宿主） |
| `anchor_gen` | 归档时的锚代数；0 = 不骑锚（console、普通 host fd、模拟对象） |
| `holds_anchor_ref` | **本槽**是否持有锚引用（与 `win_socket_anchor_ref/unref` 配对，不看数字长相） |
| `path` | 打开时的 guest 路径，仅供 `/proc/<pid>/fd/<n>`；每槽一份独立字符串 |

`SIM` 覆盖：pty 端、`/dev` 项、合成的 `/proc` 文件、socket 锚、memfs 项。

## 3. 解析：guest 号 → 宿主号

`userland_fd_host(ctx, fd)`：

1. 未跟踪的号：原样返回（视为「同号的宿主 fd」）。
2. 跟踪的槽：若 `anchor_gen != 0` 且 `anchor_gen != win_socket_anchor_gen(槽.fd)`，返回 **-1**（stale，调用方得到 EBADF），因为该号已被 CRT 分给别的 socket。
3. 否则返回槽里的 `fd`。

所有 `read` / `write` / `fcntl` / `poll` 都先走这个解析。

## 4. fork：`userland_fd_table_inherit(child, parent)`

guest 的 `fork` 在宿主进程里用线程模拟，父子两个 ctx 共享**同一张 CRT 表**。逐个父槽复制到子槽，按类型分派：

| 类型 | 动作 |
|---|---|
| memfs | `rvvm_memfs_ref` + `pin`，子槽有自己的游标 |
| pty 端 | `userland_pty_ref`（共享同一 pty 对象） |
| `/proc` 对象 | `userland_proc_fd_ref` |
| 普通 host fd | `dup(parent->fds[fd].fd)` → 写子槽；`dup` 失败则 `shared=true`，子进程不关它 |
| socket 锚 | `win_socket_anchor_ref(host_fd)` + `holds_anchor_ref=true` |

路径名一并复制（父子的 `/proc/<pid>/fd/<n>` 各答各的）。

## 5. close：`userland_fd_close`

按槽的内容分派，**不按数字长相**：

| 情况 | 动作 |
|---|---|
| console 槽 | 只清槽（console 是 attach，不是句柄） |
| memfs 号 | `memfs_fd_release`（还 pin 与存储引用） |
| pty 号 | `userland_pty_release` |
| `/proc` 号 | `userland_proc_fd_release` |
| `anchor_gen != 0` | `win_socket_close(fd)`：锚引用归零才关 NUL 号 |
| `shared` | 只归还本槽的锚引用，**不关宿主号** |
| 其余（普通 HOST） | 归还 `holds_anchor_ref`，CRT 号交给 `host_fd_release` 决定 |

最后统一 `userland_fds_write(ctx, fd, used=false, ...)` 清槽。

## 6. 宿主号引用计数

`host_fd_refs[HOST_FD_REF_MAX=4096]` + 一把 `spinlock`：

- `host_fd_acquire(host)`：计数 +1
- `host_fd_release(host)`：计数 -1，**归零才 `close(host)`**；计数已是 0 时什么都不关（重复关闭不再误伤）
- `host_fd_counted(host, backend)`：`backend == HOST && 0 <= host < 4096 && win_socket_anchor_gen(host) == 0`（锚号有自己的一套引用，不进这张表）

引用只在 `userland_fds_write()` 里迁移：旧号 release、新号 acquire；**同一个号重写不迁移**（fork 会把同一个槽写两次，不能因此多计）。

> 这张表是定位「fork 后宿主号被父线程收走」时引入的，尚未完全闭合窗口，见 `../issues/fork-shared-crt-fd-space.md`。

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

## 10. 已知设计约束

1. **父子共享一张 CRT 表。** 模拟 fork 不复制宿主表，父子槽各记一份宿主号；父线程的 `close` 与子线程的 `dup` 落在同一数字上，顺序由线程调度决定。
2. **锚号与 CRT 号共用数字。** 任何「按数字判断是不是锚」的写法都会串味，必须用槽的 `anchor_gen`。
3. **引用计数的判定时刻有时差。** `counted` 在入槽时算，关闭时用当时算出的值；中间若锚的生灭变化，计数会漏。`host_fd_acquire()` 与 `dup()` 之间也有几条指令的窗口。

后果与候选方案见 `../issues/fork-shared-crt-fd-space.md`。

## 相关文件

- `src/core/rvvm_user.c` —— 槽表、`userland_fd_host`、`userland_fd_slot_alloc`、`userland_fds_write`、`userland_fd_close`、`userland_fd_table_inherit`、`host_fd_acquire/release`、`sys_read`/`sys_write`（case 63/64）
- `src/core/rvvm_user.h` —— `rvvm_userland_t` 与对外接口
- `src/win/win_socket.c` —— `win_socket_alloc_anchor`、`wsock_fd_get`、`win_socket_anchor_gen`、`win_socket_anchor_ref/unref`
- `third_party/tinyssh/channel_fork.c`、`open_pipe.c` —— 会话侧三条管道
- `third_party/tinyssh-config/hasclosefrom.h` —— `p_closefrom` 分支
- `docs/arch-userland.md` —— guest 侧整体（命名空间、挂表行、`/proc` 支持面）
