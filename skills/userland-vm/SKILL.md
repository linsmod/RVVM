---
name: userland-vm
description: Start, drive and tear down an RVVM userland VM (a RISC-V Linux guest run by `rvvm_ash --serve`) through the userland supervisor daemon, and test inside it over its own ssh endpoint. Covers `tools/supervisor/us.py` (list/start/exec/stop/rm/logs/health), the daemon-owned ssh key and the exact ssh spelling (pty vs one-shot), running guest test apps, probing interactive terminal behaviour (Ctrl+C, Ctrl+D, window size), reading guest logs, and the rules that keep user-started instances untouched. Use whenever the task needs a real guest to run something ("跑一下 guest 里的 X", "在 VM 里验证", "起个实例测 terminal/pty/ssh"), or when a change to `src/core/rvvm_user*.c` / guest samples must be exercised end to end without booting a full QEMU machine.
---

# userland-vm — 起一个 userland VM，用 ssh 在里面测

一个实例 = 一个 port = 一个 `rvvm_ash --serve` core = 一个 Alpine 用户态 VM。
core 暴露一个 ssh 端点（vpshhd → tinysshd，密钥认证），**没有 `vp` 客户端**，一切交互都走 ssh。

- 端口从 7910 起自动分配。实测（2026-11-19）：`us start` 约 1.3–1.5 秒返回，
  **返回时 ssh 已经可用**（guest 的 vpshhd 在 listen 成功后写就绪标记；
  2026-11-19 起每实例自己的 `<release>/runtime/run/<port>/<port>.ready`，
  daemon 仍兼容旧的共享 `<release>/runtime/run/<port>.ready`）；
  `us rm`（停 + 删 rootfs）约 9–11 秒。`list` 里的 `ssh_ready` 字段就是该标记。
- 实例 rootfs 在 `<release>/runtime/roots/<port>-<uuid>/`，`us rm` 会一并删除；
  mode store / rootfs lock 是 rootfs 外的 sidecar：`<rootfs>.modes` / `<rootfs>.lock`，
  `us rm` 也会清掉。
- **daemon 永远要求 token**：显式 `--token`/`SUPERVISOR_TOKEN`，否则自动生成到
  `<release>/runtime/supervisor/token`。loopback 不再是免鉴权例外（guest 的 socket
  就是宿主 socket）。
- 每个 release 树有自己的 `runtime/`、`runtime/supervisor/supervisor.db` 和 token。
- 设计/接口文档：`docs/design-userland-supervisor.md`（§4.1 release 选取、§5 API、
  鉴权与 per-instance run 目录）。

## 前置检查（先看，别乱起）

```powershell
# 1) daemon 在不在（托盘「userland supervisor」或手工起的都算）
python tools/supervisor/us.py health

# 2) 有没有现成实例能用
python tools/supervisor/us.py list
```

- `health` 里 `release` 是 release basename（不再回绝对路径）；ssh 私钥固定在该树下
  `<release>/runtime/supervisor/id_ed25519`。
- `us.py` 的 token 解析顺序：`--token` > `US_TOKEN` > `SUPERVISOR_TOKEN` >
  `--token-file`/`US_TOKEN_FILE` > 仓库下最新的 `release.*/runtime/supervisor/token`。
  树名不是 `release.*` 时自动发现不适用，用 `--token-file <path>`。
- daemon 不在：`python tools/supervisor/supervisor.py --bind 127.0.0.1:7911`
  （前台，仅供本任务；别杀掉用户已在跑的实例）。不传 `--token` 时它自动生成上面那个文件。
- 再启动一个 daemon 不会出错：它会打印 `already running at http://127.0.0.1:7911/`
  （对方已有 token 时探测结果会是 `HTTP 401 (token required)`）并以 0 退出。
- 该 release 树必须同时有 `rvvm_ash_x86_64.exe` 和 `bundle/{rootfs,system,apps}.tar.gz`，
  否则 daemon 会跳过它（只编了 `bin` 的试验目录不能用来跑实例）。
- 一次安全/隔离自测：`python tools/supervisor/selftest.py --release <test tree>`；
  它会起临时 daemon/DB、两个实例，验证 token、API 去路径、per-instance run、
  hostfs 默认拒绝、statfs/inode/uptime、uname/mounts，再清理（约 34s）。
  **它会调用 `prune --yes`**：该 release 树 `runtime/roots/` 下所有停止实例的
  rootfs 目录（凡 DB 里没有活端口记录的）都会被删掉。用专门的试验树，别拿有
  你要保留的停止实例的树去跑。

## 基本操作

```powershell
python tools/supervisor/us.py list                     # 端口/状态/期望/TCP/启动时间
python tools/supervisor/us.py start                    # 起一个，自动分配端口
python tools/supervisor/us.py start --port 7920 probe  # 指定端口 + label
python tools/supervisor/us.py exec  7920 "uname -a"    # 一次性命令（自动用 daemon 的 key）
python tools/supervisor/us.py logs  7920 -n 40         # 该实例的 core 日志
python tools/supervisor/us.py stop  7920               # 停止（保留 rootfs 与记录）
python tools/supervisor/us.py rm    7920               # 删除（连 rootfs、sidecar、run 目录一起清）
# token 自动发现只认 release.* 树；自定义树/多 daemon 时显式指定：
python tools/supervisor/us.py --token-file <release>/runtime/supervisor/token list
python tools/supervisor/us.py --token <token> health
```

**`us start` 是同步的，不需要再补一次调用**：请求会一直等到 guest 的 `vpshhd` 报就绪
（`<release>/runtime/run/<port>/<port>.ready`；旧 daemon 的共享标记也兼容）才返回。
返回 `instance up: port=N pid=P` 且退出码 0，就表示 **ssh 现在就能用**——直接
`us exec` / ssh，别再轮询 `list` 或 sleep。返回 `instance failed` 且退出码 1 才是真失败
（先 `logs <port>`，不要重试着猜）。`list` 的 `ssh_ready` 字段只用来查看**已经存在的**
实例（比如别人起好的、或你接手前就在跑的）。

API 等价入口（要别的语言/脚本时）：`http://127.0.0.1:7911/api/health`、
`/api/instances`、`POST /api/instances`、`POST /api/instances/<port>/exec`；
**所有 API/WS 都必须带 token**：HTTP 用 `Authorization: Bearer <token>`，
WS 用 `?token=<token>`。API 只回 release/rootfs/log_path basename，不回 endpoint
或绝对宿主路径；失败 `note` 里也不再拼全路径。

## 直接 ssh（不走 us.py 时）

私钥和选项都是固定的（`Supervisor._ssh_argv()` 同款）：

```powershell
$rel = "H:\github_repos\RVVM\release.test.windows.x86_64"   # 用 health 里那个
$key = "$rel\runtime\supervisor\id_ed25519"

# 一次性命令（无 pty）
ssh -i $key -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR `
    -o BatchMode=yes -o ConnectTimeout=10 -p 7920 root@127.0.0.1 "uname -a; echo EXIT=$?"

# 交互式会话（pty：行规程/回显/^C/窗口大小都在这里）
ssh -tt -i $key -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR `
    -o BatchMode=yes -p 7920 root@127.0.0.1
```

- 需要 pty 才有意义的测试（Ctrl+C、Ctrl+D、`stty`、`vi`、窗口尺寸）**必须 `-tt`**；
  一次性命令不要 `-tt`，否则输出会带 CRLF 和提示符。
- Windows 上 `UserKnownHostsFile=NUL`，Linux 上 `/dev/null`。
- 密钥模式只认 daemon 生成的那把；手边没有 key 时用 `us exec`。

## vm：像 `bash` 一样用实例（本机已装好）

`vm`（`tools/supervisor/vm.py`，包装 `vm.cmd`）是这一个动作的客户端，不必自己拼 ssh：

```powershell
vm                                   # 进交互 shell（pty：^C/^D/vi/窗口大小都正常）
vm -c "uname -m; ls /"               # 一次性执行，退出码就是 guest 命令的
vm --list                            # 有哪些 running 实例（含标签）
vm --label web -c "ls /data/app"     # 按标签选（标签在活实例间唯一，重名会被 409 拒绝）
vm --port 7929 -c "cat /etc/hostname"
vm --start                           # 没有 running 实例时先起一个
```

- 起实例时给名字：`python tools/supervisor/us.py start web`（label 是位置参数）。
  这个 label 会变成 guest 的 hostname：提示符 `web:~#`、`uname -n`、`hostname` 都是它
  （无标签仍是 `rvvm-user`）。guest 里 `hostname NAME` 也生效，且对同一 run 的所有进程可见。
- 默认连**端口最小（最早启动）**的 running 实例；`--port` / `--label` 二选一。
- token/key 与 `us.py` 同一套解析（`--url/--token/--token-file` → 环境变量 → 最新的
  `release.*/runtime/supervisor/token`，私钥在同目录 `id_ed25519`）。
- 连不上时（ssh 退出码 255）会提示还有哪些实例可换，不做静默切换。
- 在没有 `vm` 的机器上：`python tools/supervisor/vm.py ...` 等价。
- PowerShell 里装 `vm.ps1`（`@args` 原样传参，`vm -c 'echo "a b"'` 不会丢引号）；
  cmd.exe 用同目录 `vm.cmd`——它按 `%*` 重新解析命令行，命令里带引号会散架。

## 常见测试配方

**1. 跑 guest 内的测试程序**（bundle 里的 app 都在 `/data/app/<id>/bin/<id>.exe`）：

```powershell
python tools/supervisor/us.py exec 7920 "/data/app/test_jobctl/bin/test_jobctl.exe"
# 退出码经 exec 的 exit 字段；输出里 "=== PASS: ... ===" / "=== FAIL: N check(s) ==="
```

**2. 交互式终端行为（Ctrl+C / Ctrl+D）**：必须 pty。用 Python 驱动最稳
（`subprocess` + 读线程，Windows 上也能跑）：

```python
p = subprocess.Popen(["ssh", "-tt", "-i", key, "-o", "StrictHostKeyChecking=no",
                      "-o", "UserKnownHostsFile=NUL", "-o", "LogLevel=ERROR",
                      "-o", "BatchMode=yes", "-p", "7920", "root@127.0.0.1"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT)
# 起一个读线程把 p.stdout 收进 buffer；等 1.5s 拿提示符，再 p.stdin.write(b"\x03")
# 判据：空闲提示符上按 ^C 应得到 "^C" + 换行 + 新提示符，且随后还能执行命令。
```

- 发给 pty 的**回车是 `\r`**（或 `\n`，行规程 ICRNL 会转），命令以 `\n` 结尾即可。
- 连按两次 `^C` 后会话必须还活着——这是曾经卡死过的回归点（见 `docs/fix-userland-tty-cc.md`）。

**3. 直接看某个进程的行为**：`us exec 7920 "ps -o pid,ppid,stat,args"`，
或 ssh 里跑 `/bin/busybox <applet>`。guest 里只有 busybox + 打包进来的程序。

**4. websocket 终端桥**（页面用的那条路）：
`python tools/supervisor/ws_smoke.py <port> <token>`（token 也可走 `US_TOKEN` /
`SUPERVISOR_TOKEN` / 最新的 `release.*/runtime/supervisor/token`），
握手 → 发命令 → 断言回显，判据 `WS-BRIDGE-OK`。

**5. guest 自己的 mount -t hostfs** 现在默认 `EPERM`：宿主路径是宿主能力，不是
guest root 的自带权限。要测这个能力，直起 core 时加
`rvvm_ash --serve ... --allow-guest-hostfs`；supervisor 起的实例不带这个参数。
宿主预挂载（如 `/run/vpsessiond`）不受影响。

## 纪律与已知坑

- **只动自己起的实例**：先 `list` 看端口归属；用户的实例用 `stop`/`rm` 之前先问。
- 任务结束把实例删掉（`us rm <port>`），别留 core、rootfs、sidecar 和端口。
- 一个端口一个 core；同一个端口重复启动会被 daemon 拒绝（而不是覆盖）。
- 改的是 `rvvm_user.c` 这类 core 代码时，**先重新构建 release 树**（`mingw32-make BUILDDIR=... bin`），
  再 `us start`；daemon 和 core 用的是 exe 本体，不会热更新。构建目录必须带 `bundle/` 才算可用。
- daemon 的启动参数随源码演进（例如新增 `--run-dir`）：core/supervisor 改动后没重建时，
  旧 exe 会以 `unexpected argument '--run-dir'` 立刻退出，`us start` 很快报
  `core exited before sshd was ready`（不是 25 秒超时），实例日志里就是那条 usage。
- 网页 UI 现在必须填 token（头部输入框，存在 localStorage）：值在
  `<release>/runtime/supervisor/token`，和 `us` 自动读的是同一个。
- 别用 `release.` 开头命名一次性的试验构建目录：daemon 会自动发现 `release.*`，
  不合格的会被跳过并写日志（能跑，但会让人以为选错了树）。
- **`us` 返回 401 unauthorized**：daemon 现在强制 token。用
  `python tools/supervisor/us.py --token-file <release>/runtime/supervisor/token ...`，
  或设置 `SUPERVISOR_TOKEN`；网页 token 框粘贴同一个值。
- **新实例看不到旧的 `rootfs.modes`/`.ash.lock`**：它们现在是 rootfs 外的 sidecar；
  旧二进制起的实例要 `us rm`/重建才会清掉旧文件。
- **`df`/`stat -f` 是合成预算**（root 1 GiB / hostfs 64 MiB），不是宿主磁盘；
  `stat` 的 inode 是 hash 后的 guest 值，不要拿它和宿主 `fsutil file queryfileid` 比对。
- **guest 二进制默认 strip**（`GUEST_DEBUG=0`）：调试 guest 行表要显式
  `-MakeArgs GUEST_DEBUG=1`。
- 已知与实例无关的噪音：`--memfs` 形态起不来；`tools/*_e2e.ps1` 在本机大面积 FAIL
  （与 stash 基线一致）；`test_jobctl` 的 ctty 段 8 项基线就 FAIL；
  `test_std` 的 `a dup(2) of stdout keeps its /proc link name` 是基线 FAIL；
  Windows 上 `statx` 返回 ENOSYS，`test_std` 的 statx 段 SKIP；
  空闲提示符按 `^C` 比旧版 busybox 多一个空行——这是 Alpine busybox 1.37 自身行为
  （同一 binary 在真 Linux 上一致），不是模拟器问题。
- guest 里 root 无口令、ssh 只监听 127.0.0.1；不要把实例端口暴露出去。
