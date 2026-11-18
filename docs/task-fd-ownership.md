# task: 宿主号私有化（fd 所有权）

> 执行记录与恢复点。计划见 `todo-fd-ownership.md`，目标形态见 `design-fd-ownership.md`。
> 分支：`fd-ownership`（2026-11-18 从 `android-abi-host` 切出）；同日 P3 收官后**快进合回 `android-abi-host`**（`65b918e..03c2134`），两支当时同 tip，后续开发在 `android-abi-host` 上继续。

## 起点状态

- 已落地：`host_fd_refs` 引用计数 + `anchor_gen` 判定（提交 `fc4ba9d`）
- 未实现：P1 / P2 / P3 全部
- 阻断现象：`ssh -p 7900 "echo HELLO"` 无输出，`sys_write(guest fd=1) = -9 (EBADF)`
- 环境限制：`tools/session_e2e.ps1` 在本机 22 项全 FAIL，`git stash` 后基线一致，**不能作为回归依据**；
  唯一有区分力的信号是 ssh 会话输出

## 恢复方式

1. `git switch fd-ownership`，`git log --oneline -5` 看进度
2. 读本文件「进度」一节的最后一条
3. 构建：`mingw32-make bin system-assets pack-system`
4. 复现：清 `runtime/rootfs` 与 `runtime/run/*.log` → `--serve` → `ssh -p 7900 ... "echo HELLO"`

## P1：fork 私有化

### 改动点

行号是 2026-11-18 起点快照，改动后会漂移，按函数名定位。

| 位置 | 动作 |
|---|---|
| `rvvm_fd_entry_t`（~1432） | 删 `bool shared;` 与注释 |
| `userland_fds_write`（~7523） | 签名删 `bool shared` 参数 |
| 同上（7545 / 7562 / 7574） | 删赋值与 trace 字段 |
| `sys_fcntl` setfl（~7745） | 调用删参 |
| `userland_fd_close`（~8007） | `fd_drop` trace 删 shared 显示 |
| 同上（~8036） | 条件去掉 `!ctx->fds[fd].shared &&` |
| 同上（8062-8068） | 删整个 `else if (shared)` 分支 |
| 同上（~8071） | `fds_write` 调用删参 |
| dup 路径（~8603） | 调用删参 |
| dup2 路径（~8736） | 调用删参 |
| `userland_fd_table_inherit`（8788-8792） | 删「父槽自己是 shared 就跳过」 |
| 同上（8806-8822） | `dup` 失败改为不写槽（子侧该 fd 无效），删除 shared 兜底 |

### 语义变化

`dup` 失败时子进程看不到该 fd，而不是看到一个「别关它」的共享号。与 Linux fork 的 EMFILE 行为一致：继承要么成功，要么没有这个描述符。

### 验证

```powershell
mingw32-make bin system-assets pack-system
Remove-Item -Recurse -Force release.windows.x86_64/runtime/rootfs -ErrorAction SilentlyContinue
Start-Process .\release.windows.x86_64\rvvm_ash_x86_64.exe -ArgumentList '--serve' -RedirectStandardError "$env:TEMP\p1.err" -WindowStyle Hidden
Start-Sleep 25
ssh -p 7900 -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL 127.0.0.1 "echo HELLO"
```

判据：客户端收到 `HELLO`。失败时按 `wf-debug-userland-fd.md` 的四步顺序读 `$env:TEMP\p1.err`。

## P2：锚号独立号段 + socket 移出宿主 poll

见 `todo-fd-ownership.md`。开工前先 `rg "win_socket_|wsock_fd_get"` 全量过一遍，补全调用点清单。

## P3：删除剩余防御机制

见 `todo-fd-ownership.md`。P1、P2 稳定后执行。

## 进度

- [x] P1 fork 私有化
- [x] P2 锚号独立 + poll 归类（socket 通道已通，见下）
- [x] P3 删除 `host_fd_refs` 与 stale 判定（2026-11-18，见下）

### 2026-11-18 P2 实测

改动：`win_socket.c` 的锚号改为 `WSOCK_ANCHOR_BASE + index`，NUL 句柄存进 `wsock_nul[]`；`userland_own_fd()` 纳入 socket（poll/select 不再把锚号交给宿主），新增 `userland_sim_fd()` 只用于槽的 backend 判定；`dup3` 的「共享对象」分支改回 `userland_sim_fd`。

- **socket 写通道已通**：`sys_write(guest fd=1 host=0x40000002)` 向客户端成功写出 120、52 字节，SSH 握手不再 reset，`session 102 exited 0`
- 过程中修掉两个由号段变更暴露的分类错误：`install` 曾把 socket 槽标成 `FD_BACKEND_SIM`（close 不再走 `win_socket_close`）；`dup3` 曾把锚当共享对象复制、不取锚引用，导致槽立刻 `fd_stale`
- **仍未通过**：sh（104）的 `sys_write(fd=2 host=10)` 写 37 字节后 EBADF。宿主 10 是父在 fork 后关闭 ch 端时 `_close` 掉的，而子 `dup(ch[2])` 恰好在同一窗口拿到 10——`host_fd_release` 的 `_close` 与另一线程的 `_dup` 之间没有原子性，这是 P1 保留引用计数时就存在的窗口，不是 P2 引入的
- 结论：P2 的目标（锚号不混 CRT、socket 不进宿主 poll）达成；剩余阻断是 CRT 层的 `dup`/`close` 原子性，需要在 P3 之前单独处理

### 2026-11-18 方案 A 实测（未闭合）

改动：新增 `crt_lock`，`host_fd_release()` 的 `close()` 与四处 `dup()`（install 挪号、dup、dup2/dup3、inherit）都放进同一临界区；另给 `host_fd_acquire/release` 加了计数 trace（`host_ref host=N = M (acquire|release)`）。

结果：`ssh "echo HELLO"` 仍退出 1、无输出，`sys_write(fd=1)` 仍是第一次成功、第二次 EBADF。**A 没有闭合问题。**

计数 trace 暴露的真实形态（一次运行，时间戳毫秒）：

```
[102] host_ref host=8 = 1 (acquire)      fd=5 host=8 装槽
[102] host_ref host=8 = 0 (release, closing)   fd=5 关闭 → close(8)
[104] host_ref host=8 = 1 (acquire)      fd=1 拿到 8（父刚关闭的号）
[104] host_ref host=8 = 2 (acquire)      fd=3 open("/etc/passwd") 也拿到 8
[104] host_ref host=8 = 1 (release, kept)
[104] host_ref host=8 = 0 (release, closing)
```

- `open("/etc/passwd")` 返回 8，而 fd=1 的槽已经记着 8：**CRT 认为 8 空闲，槽却仍命名它**。这不是两个线程抢同一个号，而是**槽里存着一个已经关掉的号**（幻影），计数因此虚高（2），`open` 的号被当成共享
- 幻影的来源：fd=1 的 8 在父 `close(8)` 之后被写入，写的时候没有重新验证 8 还在。`crt_lock` 只能保证「分配/释放」互斥，管不到「槽里存的号是否还活着」
- 因此 A 的定位是对的（`dup` 与 `close` 确实需要串行），但它**不是充分条件**：只要槽的号与 CRT 状态之间还有一次未经检查的写入，幻影就会出现

- 结论：A 保留（无副作用，去掉一类真实竞争），但不闭合。剩下的缺口与设计文档一致——**号必须按 address space 私有**（B 方案），否则「槽里的号还有效吗」这个问题永远要靠引用计数猜。已登记为下一步

### 2026-11-18 方案 B1 实测（部分落地，未闭合）

B1 取「每槽独占一个 CRT 号」而非「自管 guest fd → HANDLE 映射」：后者要改写全部 `read`/`write`/`open` 的 IO 路径，而前者已经能保证「父关不掉子的号」，隔离效果等价。

改动：

- `inherit` 删掉「先把父槽的号整段复制给子槽、各类型分支再覆盖」的做法。memfs / pty / proc / dev 各自显式写槽，普通 HOST 号仍 `dup()`。此前那一行让两个 address space 短暂命名同一个号
- `host_fd_acquire/release` 改名为 `host_fd_take/give`：`give` 不再看计数，槽不再命名它就关闭。`host_fd_refs[]` 降级为**检测器**——`take` 时若发现计数已非零就 `rvvm_warn`，把「一个号被两个槽命名」当场报出来（同时打印两个槽号与 backend）
- 新增 `host_fd_alive()`（Windows 走 `_get_osfhandle`），在 `fds_write` 末尾校验「写进槽的号是否还开着」
- **锚彻底退出 CRT 号空间**：`wsock_nul[]` 与每锚一次 `_open("NUL")` 全部删除。该句柄自 P2 锚号虚拟化后只被写入和关闭、从未被等待（`win_socket_wait` 走 SOCKET），却占着一个真实 CRT 号，且它的 `_close()` 不在 `crt_lock` 内。索引占用改由 `wsock_anchor_taken[]` 记录

验证：

| 轮次 | shared 告警 | `echo HELLO` |
|---|---|---|
| 5 轮 | 1–2 次（约 1/3 运行复现） | 始终 EXIT=1、无输出 |

- `session 102 exited 0` 稳定，SSH 握手正常
- 关键反证：**`shared=0` 的那一轮，`fd=1 host=5` 仍然 EBADF**。所以「两槽同号」不是这条失败的充分原因
- 该轮：`sys_write(fd=1 host=5)` 请求 6 字节返回 `-9`（EBADF），`sys_write(fd=2 host=7)` 请求 37 字节返回 `-22`（EINVAL），随后 `exit_group(1)`。104 自身在这之前没有任何 close 调用
- `fds_write` 末尾的 `host_fd_alive()` 在装槽时通过——号在写槽那一刻是活的，之后被外部关掉

已排除的关闭路径（逐个加标记验证，均未命中 host 5）：

- `host_fd_give()`（`crt_lock` 内，唯一的「正常」关闭）
- `userland_fd_host_off_console()`（只关 0/1/2）
- `userland_fd_add()` 表满
- `vps_close_master()` / `vps_close_slave()`（Windows 侧不走裸 close）
- asset open 失败路径

- 结论：B1 的号私有化把「父子共享号」这一类真实清掉了，也让锚不再占用 CRT 号，但**未闭合会话**。剩下的形态是「号在装槽之后被某个尚未定位的路径关掉」，与共享无关。下一步需要在 `_close` 上做带符号解析的探针（打印 `__builtin_return_address` 并用 `addr2line` 还原调用点），而不是继续给已知路径加标记

### 2026-11-18 B1 后续：未知关闭源定位与闭合（B2）

上一节留下的「装槽后号被外部关闭」已定位并修复，会话闭合。

**探针方法**（`wf-debug-userland-fd.md` 探针规则的延伸，已整体回退）：

- shim `close()`（`posix_shim.c`）是 exe 内所有 POSIX 拼写关闭的唯一咽喉（hostfs / vp_* / guest close(2) 全走它），在入口加一条 `RVVM_TRC(RVVM_TRC_FD, "shim_close fd=%d ra=%p rva=0x%llx", ...)`，`RVVM_TRACE=fd` 门控、零副作用、不占 fd
- `ra` 用 `GetModuleHandleExA(FROM_ADDRESS)` 求出所在模块基址，打印 `rva = ra - base`；两轮运行 RVA 一致而 exe 每次启动 ASLR 基址不同，可据此确认 ra 属于哪个模块
- release 构建的 DWARF 被 LTO 裁得只剩残片，`addr2line` 大面积 `??`；按 `rvvm_user.c` 注释里登记的配方改用 `mingw32-make bin CFLAGS=-g`（保持 LTO）——代码布局与无 -g 完全一致，且符号可解析。`USE_DEBUG=1 USE_LTO=0` 反而链接失败：shim 的 `ntohs/htonl` 与 ws2_32 导入库 multiple definition（release 一直靠 LTO 掩盖），此配方当前不可用
- 无行号的地址用 `nm -n` 找所属符号区间；`addr2line -i` 在 LTO 分区上仍不可靠

**根因**（`sys_close` case 57，`rvvm_user.c`）：

- `userland_fd_close()` 对未跟踪号返回 false 后，else 分支把 guest 号**原样裸 `close()` 透传给宿主 CRT**。这个透传原本只为 asset 挂载 fd 服务（mount 直接发裸宿主号、不进槽表）
- tinyssh 会话子进程的 `p_closefrom(3..255)` 对 3..255 逐个 `close(i)`：其中 pa 端槽已在前一步清掉的号（如 3/4/5）是未跟踪号 → 走透传 → `close(5)` 正好命中 fd=1 刚 `dup(ch[1])` 拿到的宿主 5。**杀 fd=1 的不是别人，是子进程自己的 sweep**
- 证据链（`RVVM_TRACE=fd,sys`，一轮运行内）：`[104] take host=5`（fd=1）→ 2ms 后 `[104] shim_close fd=5`，RA 落在 `rvvm_user_thread_wrap` 区间（sweep 的透传 close，且**无 give 痕迹**）→ 退出时 `[104] give fd=1 host=5` 计数剩 1（102 的槽后来复用了 5）→ `sys_write(fd=1 host=5)` 先「成功」6 字节（写进回收后的别的对象，字节没到会话）再 `-9`
- 修正一处旧结论：`shared` 告警与「两槽同号」确实是幻影的下游表现，不是独立缺陷

**修复**（B2）：

- 新增 `asset_fd_take()`（弹出式）：从 asset 表弹出并报告该号是否真是挂载发的宿主号
- case 57 的 else 分支改为：真 asset fd 才按号裸关闭；其余未跟踪号按 POSIX 回 `-EBADF`，不碰任何东西

**验证**：

| 项 | 修复前 | 修复后 |
|---|---|---|
| `ssh -p 7900 "echo HELLO"` | 5 轮全 EXIT=1 无输出 | **3 轮全 `HELLO`、EXIT=0**（含去掉探针后的最终轮） |
| trace 里 sweep 裸关闭（rva=0x21d05） | 每轮 1+ 次 | 0 次 |
| `fs_selftest.ps1` | PASS | PASS |
| `mount_e2e` / `memfs_e2e` / `procfs_e2e` / `openssh_e2e` | FAIL 57/63/25/17 | 与 `git stash` 基线**逐项相同**，均为本环境既有失败（`session_e2e` 环境缺陷同类），非本改动引入 |

剩余：P3（删除 `host_fd_refs` 检测器等防御机制）未动；fork 继承 `ctx->asset_fds` 表时父子同号的边界未重新审计（P3 时一并看）。

### 2026-11-18 P3 实测（完成）

todo P3 的三个目标全部落地（提交见进度表；`crt_lock` 不在 P3 范围，保留——它仍在串行化 `_dup` 与 `close` 的分配/归还对）：

- **`host_fd_refs` 计数表 + `host_fd_ref_lock` 删除**，`host_fd_take` 整体删除，`host_fd_give` 只保留关闭动作（`HOST_FD_REF_MAX` 更名 `HOST_FD_MAX`，作为 CRT 号归属上限）
- **`userland_fd_host` stale 判定删除**，退化为纯查表（`design-fd-ownership.md` §3.4）
- **close 分派删除 `anchor_gen` 猜测分支**：P2 后锚号在独立号段，`win_socket_is_fd()` 是区间判定，不再有「普通文件描述符 5 被当成锚 5」的问题
- `anchor_gen` 字段按设计保留为**诊断字段**（fds_write 记录 + fd_wr trace 打印，无任何判定读它）；`host_fd_alive` 装槽断言保留（warn-only，B2 的根因正是它先暴露的形态）
- todo 验收「`rg "host_fd_ref"` 无残留」达成；`anchor_gen` 有残留属「退化为诊断」分支，符合 todo 原文

验证：

| 项 | 结果 |
|---|---|
| `ssh -p 7900 "echo HELLO"` | 3 轮全 `HELLO`、EXIT=0 |
| trace 告警（not open / stale / shared） | 0 |
| `fs_selftest` | PASS |
| mount/memfs/procfs/openssh e2e | 与基线逐项相同（57/63/25/17，环境既有） |

**fork 继承 `ctx->asset_fds` 的边界**（本次未动，登记为遗留）：asset fd 表按 ctx 存，fork 时若父 ctx 的 asset fd 号被复制给子 ctx，子按号关闭会关掉父子共享的宿主描述符。tinyssh 链路不开 asset 挂载，未触发；vpsessiond 链路（bundle app）用到 asset 时需复核。

### 2026-11-18 P1 实测

- 会话不再以 111 退出（此前子进程 `dup(ch[i]) != i`），日志变为 `session 102 exited 0`
- 子进程拿到独立管道号：父 8/9/10 → 子 16/17/18，证明 `inherit_dup` 生效
- **P1 暴露了一个既有缺口**：`inherit` 原本没有 `/dev` 分支。`/dev` 是合成号（`RVVM_DEV_FD_BASE 0x7C000000`），以前靠 `dup` 失败后的 `shared` 兜底才「能用」，改成 drop 后子进程直接丢了 stderr（101/103 的 fd=2），`channel_fork` 因此 111。已补 `userland_dev_by_fd` 分支（只复制号，无需持有）
- **仍未通过**：sh 的 `sys_write(fd=1)` 第一次成功 6 字节，紧接着第二次 EBADF；父进程早于 sh 退出（102 比 104 早 25ms）。子 `dup(ch[1])` 拿到的宿主号 10，正是父刚释放的号——父的 `close(ch[i])` 与子的 `dup(ch[i])` 仍落在同一 CRT 号上
- 结论：P1 是净改进（111 消失、子侧不再 shared），但不闭合；剩余竞争要等 P2 的号段隔离，或对 fork 后父侧的 close 与子侧 dup 做串行化
