# wf-debug-userland-fd

guest fd 与宿主句柄不同步时的定位流程：写不出去、EBADF、会话静默退出。

## 适用条件

- guest 的 `write()` / `read()` 返回 EBADF，但槽表、`fd_dump`、`/proc/<pid>/fd` 看起来都正常
- 会话正常 0 退出却不产生输出；而 `cmd > file` 能落盘（说明进程在跑，只断在 0/1/2）
- fork / execve 之后父子某一侧的 fd 失效

不适用：路径解析问题（走 `mount_e2e.ps1`）、console 渲染问题（见 `../docs/arch-virtpass.md` §4）、单纯 socket 协议错误（走 `openssh_e2e.ps1`）。

## 前置

- 先读 `../docs/arch-userland-fd.md`：三层编号（guest 号 / CRT 号 / 锚号）与时序案例是本文的判读基础
- 需要能正常启动 core 的桌面环境；起不来时只能做静态分析，不要硬推
- 对 `third_party/tinyssh*` 的临时改动必须能整体回退（`git checkout --`）

## 环境

- Windows + `mingw32-make`（构建入口见 `wf-build-and-test-win32.md`）
- trace：`$env:RVVM_TRACE='fd'`（槽与宿主号）、`'sys'`（系统调用返回值）
- 宿主日志：把 core 的 stderr 重定向到 `$env:TEMP\core*.err`
- guest 侧日志：`release.windows.x86_64/runtime/run/*.log`（如 `vpshhd.log`、`tinysshd.err`）

## 命令

```powershell
# 1. 起带 trace 的 core。不要用 -c 客户端：它连已存在的 core，环境变量传不进去
Get-Process rvvm_ash_x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
$env:RVVM_TRACE='fd,sys'
Start-Process .\release.windows.x86_64\rvvm_ash_x86_64.exe -ArgumentList '--serve' `
  -RedirectStandardError "$env:TEMP\core1.err" -WindowStyle Hidden

# 2. 复现（tinyssh 会话链路）
ssh -p 7900 -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL 127.0.0.1 "echo HELLO"

# 3. 读 trace：先槽操作，再系统调用
Select-String -Path "$env:TEMP\core1.err" -Pattern "install|dup|close|host_release|sys_write" | Select-Object -Last 20
```

## 判定顺序（不要跳步）

1. **槽是否存在且有效**：`fd_dump`、`/proc/self/fd`，确认 guest 看到的号和 tracking 状态
2. **宿主号是否仍指向同一对象**：看 `fd_drop`、`host_release (last ref, closing)`、锚的 `anchor_gen`
3. **写端是否还活着**：poll 的 `revents` 是否为 0、直接 `read` 是否 `EAGAIN`（管道空）还是 `EOF`（写端已关）
4. 前三步都正常，才怀疑数据通路（管道/协议/缓冲）

跳过 1、2 直接看 3 会把「写端已死」误判成「读侧漏报」。

## 探针规则

- 探针**不得占用被测 fd**：曾用 `open_write()` 开文件，拿到 guest fd 1，把正常路径挤掉，得到假 EBADF
- 探针输出写文件，不写 stdout——stdout 正是被测通道
- 探针只加 `RVVM_TRC` 或写文件，不加有副作用的逻辑；收尾时整体回退，不留在提交里

## 常见误判

- **时间戳不能定序**：父子跑在两个线程，各自取时；某行打在前面不代表它先发生
- **宿主号跨运行不可比**：这次管道端是 8/10，下次是 11/13；拼两段日志会得出自相矛盾的结论
- **「槽里号正常」≠ 号有效**：槽存的是数字，不是身份；号可能已被回收给别的对象

## 退出条件

- 现象消失，且能写出完整因果链：谁、在什么时刻、关掉了哪个号
- 临时探针已回退，`git status` 无本轮调试残留
- 结论落到 `../docs/arch-userland-fd.md`（现状）或 `../issues/`（缺陷）；解法若涉及模型变更，同步 `../docs/design-fd-ownership.md` 与 `../docs/todo-fd-ownership.md`

## 执行经验

- 2026-11-18：用本流程定位到 `write(1) = EBADF` 的根因是「父线程释放了子进程刚 `dup` 到的宿主号」，判读依据只有 `host_release (last ref, closing)` 与 `install fd=1 host=N` 的先后关系，时间戳在此完全不可用。
- `vptrace` 类探针（改 `main_tinysshd.c` 打 poll 状态）对「读侧没数据」很有用，但必须记得回退，否则会污染后续会话行为。
