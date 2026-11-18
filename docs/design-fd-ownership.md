# 设计：宿主 fd 的所有权模型

> **不是现状，且尚未实现。** 本文冻结于 2026-11-18 的设计决议。
> 当前实况见 `arch-userland-fd.md`；实施计划与验收见 `todo-fd-ownership.md`；问题证据见 `../issues/fork-shared-crt-fd-space.md`。

## 1. 要解决的问题

槽里存的是一个裸宿主号，而这个号落在**全进程共享**的 CRT 表里。于是「这个号还归我吗」在数据模型上没有答案，只能靠关闭时再问一次「这个号现在长什么样」。父子两个地址空间因此会关掉对方的号。

引用计数、全局锁、世代比较都是在这个前提下的补丁：它们让误关变成小概率，但都默认「号可能被别人关」。**本设计取消这个前提。**

## 2. 决议

**宿主号按 address space 私有。** 每个模拟进程的槽表独占它引用的每一个宿主号；号从不跨空间共享。对象（inode、pty、socket）可以共享，共享的是对象，不是号。

一句话概括三层职责：

| 层 | 拥有什么 | 共享吗 |
|---|---|---|
| address space | 槽表、每个槽的宿主号 | 否，永私有 |
| 对象 | memfs inode、pty、SOCKET | 是，带引用计数 |
| 号 | CRT fd / 锚号 | 归某一个槽，恰好一个 |

## 3. 模型

### 3.1 槽

```
rvvm_fd_entry_t {
    int  fd;              // 本空间独占的号：CRT fd，或锚号
    rvvm_fd_backend_t backend;
    uint64_t gen;         // 号的身份（锚用代数；CRT 号记 0）
    bool cloexec;
    char* path;
}
```

`shared` 字段删除。它存在的唯一理由是「这个号其实是别人的，别关」——私有化之后每个号都是自己的。

### 3.2 fork

`userland_fd_table_inherit(child, parent)` 逐槽处理，**子侧一律拿到自己的号**：

| 父槽内容 | 子槽得到 | 共享的层次 |
|---|---|---|
| memfs 项 | 新槽 + 同一 inode（pin） | 对象 |
| pty 端 | 新槽 + 同一 pty（`userland_pty_ref`） | 对象 |
| `/proc` 对象 | 新槽 + 同一生成器（ref） | 对象 |
| 普通 CRT fd | `dup()` 出的**新号** | 无（各持一号，指向同一 open file description） |
| socket 锚 | `win_socket_dup()` 出的**新锚号** | SOCKET 对象（`wsock_fds_refs`） |

`dup()` 失败时不再退化成 `shared=true`：子侧的槽直接标记为无效（`used=false`），guest 看到的是「这个 fd 没继承过来」，而不是一个随时会被别人关掉的号。这与 Linux 上 fork 失败时的行为一致：要么有号，要么没有。

### 3.3 close

槽的关闭就是关自己的号：

```
memfs    → 还 pin，释放槽
pty      → userland_pty_release
/proc    → userland_proc_fd_release
锚号     → win_socket_close(锚号)：关 SOCKET，释放锚号
CRT 号   → close(号)
```

没有「先问这是不是锚」，没有「先看引用计数」，没有「归还别人的引用」。因为号只有这一个槽持有，上述四步都是终局动作。

对象层仍保留引用计数（`rvvm_memfs_ref/unref`、`userland_pty_ref/release`、`wsock_fds_refs`）：父子共享 inode 时，一方关闭不能销毁对象。**计数管理对象，不管理号。**

### 3.4 解析

`userland_fd_host(ctx, fd)` 变成纯粹的查表：

```
return tracked(ctx, fd) ? ctx->fds[fd].fd : fd;
```

私有化之后，槽里的号在槽存活期间不可能被别人释放，所以不需要 stale 判定。`gen` 保留但退化为诊断值与断言：它只在「槽被复用后仍指向旧对象」这类编程错误时才有意义。

### 3.5 号段

锚号不再借 CRT 号：`win_socket_alloc_anchor()` 从独立号段分配（`ANCHOR_BASE + n`，远高于任何 CRT 号），NUL fd 只作为内部实现细节保存在锚表里，不再对外出现。

这样「误把锚号传给 CRT API」会立刻失败，而不是静默写进 NUL。判定「这是不是锚」也变成区间判断，不再依赖「某个 CRT 号恰好被登记过」。

## 4. 数据流

```
guest read/write(fd)
   └─ 槽查表 → 号
        ├─ 锚号   → win_socket_read/write → recv/send
        └─ CRT 号 → 宿主 _read/_write
```

`poll` 分三类，只有 CRT 号进入宿主 `poll()`：

| 类别 | 就绪来源 |
|---|---|
| console | 虚拟 TTY 的 ring |
| 自有对象（pty / dev / proc / memfs） | `userland_own_ready()` |
| socket | `WSAPoll` / `select` 对 SOCKET 本身（需要一个批量接口） |
| 其余（文件、管道） | 宿主 `poll()` |

## 5. 不变量

1. **一号一主**：任一宿主号同时只出现在一个 address space 的槽里。
2. **关闭即终局**：槽关闭直接关自己的号，不需要任何前置判定。
3. **计数只管对象**：对象引用归零才销毁；号的持有数恒为 1，不计数。
4. **锚号不在 CRT 号段**：任何 CRT API 收到锚号都是 bug，且会立即失败。
5. **继承要么有号要么没有**：`dup` 失败的槽不留在表里。

## 6. 改造前后

| 项 | 现状 | 改造后 |
|---|---|---|
| 号的所有者 | 多个空间可能同时命名同一号 | 恰好一个空间的一个槽 |
| `close` 的依据 | 按数字猜类型 + 查引用计数 | 槽的 `backend` |
| `userland_fd_host` | 查表 + 世代比较 + stale 返回 -1 | 查表 |
| `shared` 字段 | 有（表达「骑别人的号」） | 无 |
| `host_fd_ref_lock` | 有（跨槽计号引用） | 无 |
| fork 时 `dup` 失败 | 退化成 `shared` | 槽标为无效 |
| 锚号来源 | `_open("NUL")` 的 CRT 号 | 独立号段 |
| socket 是否进宿主 `poll` | 是 | 否 |

## 7. 为什么这是最优解而不是缓解

缓解措施的共同前提是「号可能被别人关」，于是只能在关闭时做更聪明的判断：查引用计数、比世代、加全局锁。判断永远发生在事件之后，窗口永远存在。

私有化取消了这个前提本身。代价是一次 fork 多几次 `dup`（每个 HOST 槽一次，会话场景是个位数），收益是一整类防御代码消失：`shared` 分支、`anchor_gen` 的误判分支、`host_fd_refs` 表与它的锁、stale 检查的误报路径。

对象层的共享不受影响——同一 inode 的两个游标、同一 pty 的两端、同一 socket 的两端都照旧工作，因为共享的是对象，不是号。

## 8. 落地顺序

两个阶段，各自可独立验证：

1. **fork 私有化**：`inherit` 对所有 HOST 槽 `dup`，socket 槽 `win_socket_dup`；删除 `shared`；`dup` 失败标无效。验证点：父 `close` 不影响子，`write(1)` 在 exec 后不再 EBADF。
2. **锚号独立 + `poll` 归类**：锚改号段、`sys_poll` 把 socket 归入自有就绪、移除 `host_fd_refs` 与 stale 分支。验证点：会话输出到达客户端，`fstat` 仍报 `S_IFSOCK`。

阶段 1 结束时阻断点应当消失；阶段 2 收掉剩余的猜测式代码。

## 9. 验收

- `ssh -p 7900 127.0.0.1 "echo HELLO"` 输出到达客户端；
- 同一会话内父子各自 `close()` 互不影响，重复 close 不误伤；
- `/proc/<pid>/fd/<n>` 与 `fstat` 对 socket 仍答 `S_IFSOCK`；
- `pwsh ./tools/session_e2e.ps1` 通过——**当前环境该脚本 22 项全 FAIL 且基线同样失败（core 起不来），需换环境验证**。

## 10. 为什么不走代理式转发

另一种落地方式是槽存 `(space_id, fd)`，跨空间访问经一层转发。概念上等价，但运行时多一层间接与一次查表，`poll` 仍需把号翻译回本线程可用的值。私有化在 fork 那一刻就把翻译做完了，运行时没有额外成本，因此这里直接采用后者。
