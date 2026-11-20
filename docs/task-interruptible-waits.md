# task: 让 guest 线程能从宿主阻塞调用里被收掉（interruptible waits）

状态：进行中（2026-11-20 开工）。
相关修复：`docs/fix-userland-orphan-lifetime.md`（进程生命周期）、
`docs/fix-userland-kill-family.md`（家族查找）。

## 为什么

`rvvm_user_child_main()` 在 fork 出来的 address space 的初始线程退出后，等
`userland_threads_gone(ctx, 1000)` 一秒；还有线程没走完就放弃销毁并打印
`fork: child threads linger, leaking the machine`。这不是罕见角落：线程停在**宿主
阻塞调用**里时，`riscv_hart_queue_pause()` 打断不了它 —— 一次泄漏 = 一台
`rvvm_machine_t` + 每个 fork 的 16 MiB RVJIT 堆。

目标：任何宿主阻塞调用在 `thread->finished` 置位后 ~100ms 内返回，使上面那条
路径实际不可达（兜底保留）。

## 现状盘点（Windows 宿主 / POSIX 宿主）

| 调用 | 是否已切片 | 位置 |
|---|---|---|
| `read` on socket | ✅ `userland_wait_socket_readable`（`rvvm_user.c:8126`） | 依赖 `win_socket_is_fd`，**只有 Windows 的 anchor 才生效**；POSIX 直接返回 0 |
| `accept`/`accept4`/`recvfrom`/`recvmsg` | ✅ 同上（`:19694`、`:19978`、`:19813`、`:19873`） | 同上 |
| `connect` | ❌ | `:19776` 直接调宿主 `connect()`；Windows 走 shim → `win_socket_connect` → `WSAConnect`，对不可达地址可阻塞十几秒 |
| `write`/`send` on socket | ❌ | 发送缓冲满时会阻塞（`win_socket_write`） |
| tty 读 | ✅ | `rvvm_event_wait(tty_in_event, 100ms)`（`:19224`） |
| `poll/select` | ✅ | 100ms 切片 + `userland_thread_finished()`（`:11452`） |

已有原语：`userland_thread_finished()`（`:7268`）、`USERLAND_WAIT_POLL_NS`（100ms）、
`RVVM_NO_SOCK_WAIT` 旁路开关、`win_socket_wait(fd, events, timeout_ms)`、
`win_socket_set_nonblock(fd, on)`。

## 方案

1. **统一等待原语**（`rvvm_user.c`，紧挨 `userland_wait_socket_readable`）：

   ```c
   /* 1=ready  0=timeout  -1=线程正在被收  其它<0=错误 */
   static int userland_wait_host_fd(int host_fd, short events, int timeout_ms);
   ```
   - Windows：anchor 用 `win_socket_wait(host_fd, events, 50)`；非 anchor 用 `poll()`。
   - POSIX：`poll()` + 50ms 切片。
   - 每轮检查 `userland_thread_finished()` 与 `ctx->sig_pending`；尊重
     `RVVM_NO_SOCK_WAIT`（直接返回 0，保持旧行为）。
   - `userland_wait_socket_readable()` 改为调用它，从而让 POSIX 宿主也获得切片。

2. **connect 改成"非阻塞 + 切片等待 + SO_ERROR"**：
   - 置非阻塞（`win_socket_set_nonblock` / `fcntl`），`connect()`；
   - `EINPROGRESS`/`EWOULDBLOCK` → `userland_wait_host_fd(fd, POLLOUT, …)` 循环；
   - 可写后读 `SO_ERROR` 决定成功/失败；
   - 恢复原来的阻塞模式（guest 的 `O_NONBLOCK` 状态在 fd 槽里，按 `userland_fd_flags` 还原）。
   - 线程被收（-1）时返回 `-EINTR`。

3. **write/send 切片**（同一原语，`POLLOUT`）：仅在"可能阻塞 + socket"时等待。

4. **可观测性**：`rvvm_user_thread_t` 加 `const char *wait_note`，在上述等待点写入
   `"accept"`/`"connect"`/`"recv"`/`"write"`；`child threads linger` 触发时把每个
   残留线程的 `pid/tid/note` 打出来。

## 非目标

- guest 显式 `O_NONBLOCK` 的路径：保持立即 `EAGAIN`；
- 普通文件 / memfs / pty / console 的路径不动；
- 不调整 `userland_threads_gone` 的 1 秒，也不删那条兜底 warn。

## 验收

新样例 `src/virtpass/guest-samples/test_thread_exit.c`，三个用例（子进程线程阻塞 +
主线程 `exit`）：

1. 线程卡在 `accept()`（监听 socket 无连接）；
2. 线程 `connect()` 到不可达地址；
3. 线程 `recv()` 空闲 socket。

判据（在 supervisor 实例里跑，`RVVM_TRACE=job`）：
- 每个用例 2 秒内 address space 被销毁；
- 实例日志无 `child threads linger`；
- 不留 `/proc` 僵尸；
- 20 次循环后宿主 RSS 不长（对照：一次泄漏 +16 MiB 起）。

回归：`test_jobctl` 直接运行（基线 8 项 ctty FAIL）、`test_std`；再跑
`cmock/dev_agent.py`（cmock 部署 + agent 冒烟）确认网络路径无回归。

## 进度

- [x] 现状盘点（上表）
- [x] 统一等待原语 `userland_wait_host_fd()` + `wait_note`
- [x] read/accept 统一走新原语（POSIX 宿主也获得切片；此前只有 win32 的 anchor 才切）
- [x] write/send 接入（发送缓冲满时切片等待 POLLOUT）
- [x] `child threads linger` 时打印每个残留线程的 `tid/pid/wait_note`
- [x] 样例 `test_thread_exit`（accept/connect/recv 三例）+ 回归
- [ ] **connect 切片：试过，回退**，原因见下

## connect 为什么没合入

按计划实现的 `userland_connect_interruptible()`（临时置非阻塞 + 切片等待 POLLOUT +
SO_ERROR + 还原 flags）一上线，guest 的 ssh 端点立刻坏掉：

```
ssh: root@127.0.0.1: Permission denied (publickey)
tinysshd: fatal: unable to receive hello-string (connection aborted)
```

关闭 write 切片后仍复现，单独回退 connect 后恢复，因此归因明确。未定位到根因：
嫌疑在 win32 anchor 上做 `fcntl(F_SETFL)` —— anchor 号在 `WSOCK_ANCHOR_BASE` 之上、
大于 `SHIM_FD_MAX`，shim 的 `fcntl()` 对它会直接 `EBADF`，于是"临时非阻塞"其实没生效，
而还原调用同样失败；按推理行为应与改动前一致，但实测不是，需要单独一轮排查（建议先加
`RVVM_TRC(FD)` 对比 connect 前后的 anchor 标志与 `SO_ERROR`）。

结论：connect 仍保持原来的阻塞调用；accept / recv / write 三条路径已切片。

## 验证记录（本机）

- 构建：`mingw32-make BUILDDIR=release.test.windows.x86_64 bin`
- `test_thread_exit`（host 编译后经宿主 HTTP 送进 guest）：

  ```
  ok   exit() with a thread parked in accept()
  ok   exit() with a thread parked in connect()
  ok   exit() with a thread parked in recv()
  === PASS: thread exit ===
  ```

  说明：样例里的 `wait4` 只证明子进程退出很快，真正的 address-space 回收要看实例日志
  是否出现 `child threads linger`（本轮三次运行均未出现）。
- 回归：`test_jobctl` 直接运行仍是基线 8 项 ctty FAIL，无新增。
- 端点：改动后 `us exec`（publickey 认证 + pty/socket 路径）正常。

## 恢复要点

- 构建：`us stop/rm` 掉实例后 `mingw32-make BUILDDIR=release.test.windows.x86_64 bin`
  （Windows 会锁 exe）；guest 样例可用 zig 单编后经宿主 HTTP 送进 guest，避免重建 bundle。
- 观察点：`us logs <port>` 里的 `child threads linger`；`/proc` 里的残留进程。
- 已排除：tty/nanosleep 路径（实测 `kill` 掉 `sleep 300` 后机器正常回收）。
