# task: userland supervisor（S1 daemon + S2 CLI）

> 执行记录与恢复点。设计见 `design-userland-supervisor.md`，计划见 `todo-userland-supervisor.md`。
> 分支：`android-abi-host`（fd-ownership 已并入）。

## 起点

- 2026-11-18：用户要做 userland-as-service 预览，定案 supervisor + SQLite + CLI，语言 Python 仅标准库
- 协议收敛决议（对外面统一 tinyssh）用户决定**保持现状**，后续必要时再动

## S1/S2 交付（本轮完成）

- `tools/supervisor/supervisor.py`：daemon（SQLite WAL、状态机、reconcile、HTTP API、token、
  事件审计表）
- `tools/supervisor/us.py`：CLI（list/start/stop/rm/logs/exec/health，thin HTTP client）
- `tools/supervisor/web/index.html`：web UI v1（S3 第一刀，无依赖）：daemon 在 `GET /`
  服务单文件静态页——实例表轮询、起停/删除、日志面板、一次性命令框；API 鉴权不变
- 宿主最小改动：rvvm_ash 新增 `--rootfs <dir>`（`ash_main.c` 解析、`ash_client.c` 锁与
  mode-store 随目录收放、`win32_cmdpost_bridge.c` mount dest 覆盖）；默认路径行为不变

## 关键实现事实

- `.core` 注册文件格式：`key=value` 行（`pid=`/`shell=`/`endpoint=tcp:127.0.0.1:<port>`）
- `ash_rootfs_lock` 原为一树一 core、且 `--memfs` 不绕锁 → per-instance 目录方案才有
  `--rootfs` 改动；锁文件落在 rootfs 目录内（默认路径保留 `runtime/ash-core.lock` 历史名）
- exec 走 core 的 ssh 端点：`ssh -p N -o BatchMode=yes -o UserKnownHostsFile=NUL 127.0.0.1 <cmd>`
  （Windows NUL / POSIX /dev/null），vp 从管理面退役
- 启动宽限 25s（注册文件在 guest 起机后即写，rootfs 首次解包约 20s）
- Windows 强杀（TerminateProcess）绕过 core 的清理 handler：stop() 对自己杀的 core 回收残留
  `.core`（与 C 侧 stale 回收同一语义）

## 验收（design §8，全绿）

1. `us start` → running + tcp 可达（rootfs 模式）
2. `us exec 7910 "ls /"` → 真实 guest 输出（ssh 通道）
3. `us rm` → 端口释放、`.core` 回收、`roots/<port>-<uuid>` 目录清理
4. 外部杀 core → list 检出 `crashed`；daemon 重启 reconcile 三态保留、端口不撞
5. 两实例并发（各自 rootfs 目录）ssh 各自可达（`A1`/`B2` 双通）
6. 事件审计链完整（start/running/crashed/orphan/stopped/deleted 全程可溯）

## 过程坑（都已修）

- 换 schema 后旧 DB 文件被旧 daemon 锁着没删净 → `no column named rootfs`；停 daemon 再删
- 强杀后 `.core` 残留 → stop() 误报 500；已修（自回收）
- `start()` 中 Popen 成功、DB insert 失败会漏孤儿 core → 已加护栏（记录失败即终止 core）
- 孤儿实例的 rootfs 目录 DB 无归属，`rm` 清不掉（orphan 行 rootfs=None）→ 预览接受，
  手工清理；改进方向：reconcile 从 core 侧无从得知目录，需 core 注册表记 rootfs（后议）

## 遗留

- `--memfs` guest 退出码 1：`issues/INDEX.md` NEW，阻塞 memfs 隔离形态
- run 目录（`runtime/run`）按树共享，实例日志文件名若不按端口区分会互写——预览未触发，
  上量前要核对（vpshhd/tinysshd 的落盘名）
- S3 web 后台未动工（挂本 API，另立项）

## 恢复方式

1. 构建：`mingw32-make bin CFLAGS=-g`
2. 起 daemon：`Start-Process python -ArgumentList 'tools/supervisor/supervisor.py','--bind','127.0.0.1:7911'`
3. `python tools/supervisor/us.py health` → list/start/exec/rm

## S3 v2：交互终端（2026-11-18 第二轮）

- **鉴权前置已落地**：vpshhd 见 run 目录有 `auth.pub` 即自种 `~/.ssh/authorized_keys`
  并以 `tinysshd`（公钥模式）拉起；daemon 生成 `id_ed25519` 并投递公钥；无 `auth.pub`
  时保持 noneauth（dev 行为不变）。双向验证：带密钥 `auth-ok`、无密钥
  `Permission denied (publickey)`。
- **daemon ws 桥**：stdlib 最小 RFC6455（`GET /ws/<port>` → `ssh -tt -i <key>` 字节泵）；
  前端 `web/vendor/`（xterm.js v6 + fit，免构建链），detail 面板移出表格重渲染范围。
- **顺带修掉两个 RVVM 既有缺陷**（阻塞 pty-req，详见 `fix-userland-pty-req.md`）：
  ioctl 请求号符号扩展（case 29 截断为 uint32）；pty fd 的 fstat 身份（st_dev/st_ino
  与同名路径 stat 一致）。修复前 **vpssh 链的 pty-req 从未成功过**。
- 验收：`ws_smoke.py` `WS-BRIDGE-OK`（pty 回显/行规程/求值）；页面与 vendor 资源全 200；
  `fs_selftest` PASS；openssh/sshd_crash e2e 与 stash 基线逐项一致（既登记的环境问题）。

### 过程坑（本轮新增）

- **双 daemon 脑裂**：两个 `supervisor.py` 同时声称绑 7911（SO_REUSEADDR 在 Windows 上
  允许绑正在 LISTEN 的端口），请求按连接分落到两个进程——`rm` 时好时坏
  （RemoteDisconnected）。已确证两者答的 DB 内容不同（杀其一后 list 由 4 行变空），
  即两者持有的不是同一文件实例；具体机制待证（候选与矛盾见 issues 条目）。
  处置：只留托盘登记的一个；已登记 issues（daemon 应检测端口已占即退出）。
- **watch 包装吞输出**：多命令拼接的输出只剩第一行，且后续命令实际都执行了（一次起了
  4 个实例）。判据看落盘文件，一条命令一个文件。

## S3 v3：终端剪贴板键位（2026-11-18 第三轮）

目标：Linux 习惯为主、控制台习惯兜底，只走 xterm.js 的公开扩展点，不手搓协议细节。

| 键 / 鼠标 | 行为 |
|---|---|
| `Ctrl+Shift+C`，或有选区时的 `Ctrl+C` | 复制选区 |
| `Ctrl+C`（无选区） | 送 `^C`（SIGINT） |
| `Ctrl+V` / `Ctrl+Shift+V`（浏览器映射时 `Shift+Insert`） | 粘贴 |
| 右键 | 复制 / 粘贴 / 全选 菜单（替代页面菜单的 Inspect 之类） |

依据（`xtermjs/xterm.js` master 源码，非压缩码）：

- `src/common/input/Keyboard.ts` 的 default 分支把 `Ctrl+字母` 变成 `keyCode-64`，即
  `Ctrl+V` = `0x16`、`Ctrl+C` = `0x03`；`Ctrl+Shift+字母` 走
  `else if (ev.key && ev.ctrlKey && ev.shiftKey)`，只识别 `Minus/Digit2/Digit6`，
  其余不产生键。
- `src/browser/CoreBrowserTerminal.ts` 的 `_keyDown` 在 `triggerDataEvent` 之后对
  `ctrlKey` 一律 `preventDefault`；自定义键处理返回 `false` 时直接早退，**不**取消默认动作。
- 所以：`Ctrl+V` 必须显式丢弃键事件，浏览器原生 paste 才会落到
  `handlePasteEvent`；`Ctrl+Shift+V` 不用写代码（xterm 不产生键、不拦默认）。
- 粘贴一律交给 `Clipboard.paste()` / `Terminal.paste()`：CRLF→CR、ESC 消毒、
  远端开了 bracketed paste 时加 `\x1b[200~ … \x1b[201~`。上一版右键粘贴手写
  `replace(/\r?\n/g,"\r")` + `send()` 绕过了这些，已删除。
- 右键菜单的粘贴没有 paste 事件，才读剪贴板后交给 `term.paste()`；读被拒时提示改按
  `Ctrl+V`（零权限路径）。

未验证：浏览器侧 `document.execCommand`/权限弹窗行为未实测（按用户要求不做 web 测试），
当前实现刻意不依赖它——复制用 `navigator.clipboard.writeText`，粘贴主路径走浏览器 paste。

## S3 v4：daemon 防呆（2026-11-19）

事故形态：托盘里的 daemon 自动选中了一个只编了 `bin` 的试验目录
（`release.cc2.windows.x86_64`，为绕开被占用 exe 而临时建），它没有 `bundle/`，
于是「启动实例」25 秒后变 `failed`，界面上只看到状态，看不到原因；同时因为 DB 落在
release 树内，换树还等于换了一整套实例状态。

落地（设计与规则见 `design-userland-supervisor.md` §4.1）：

- `find_release_dir()` 要求 `rvvm_ash` + `bundle/{rootfs,system,apps}.tar.gz` 齐全；
  显式 `--release` 不合格立即退出并列出缺项；自动发现跳过不合格项（日志写原因），
  多候选取 `rvvm_ash` 最新者并打印全部候选。
- `Supervisor.start()` 启动前再查一次同一组文件，缺就 `500` + 明确消息，
  不再“起了必死、25 秒后 failed”。
- `main()` 绑定前 `GET /api/health` 探测：已有 daemon 应答就退出并报出对方
  version/release，避免两个 daemon 共用一个端口、各持一份 DB。
- `us.py health` 的 `%d` 格式化崩溃顺手修掉（JSON 里 pid 是字符串）。

验证：daemon 在跑时再启动一个 → 立即退出并打印 `daemon 0.2.0, release ...`；
伪造 `release.zz.windows.x86_64`（只有 exe）→ 被跳过并报 `missing bundle/...`，
仍选 `release.test`；重启托盘任务后 `us health` / `us start` / `us exec` 正常，
实例 7916 `running`。

## 本机部署（traycmd 托盘，2026-11-18）

daemon 已注册为托盘命令（webui-demo 实例在 daemon 换手后由 reconcile 无缝保留）：

- 托盘「网站」分区「userland supervisor」（proj rvvm，short id `7aec2859`），
  URL 固定 `http://127.0.0.1:7911/`（仅本机可访问，非公网）
- 启动：托盘点菜单项，或 `quickcmd run-job 7aec2859`；停止：托盘「结束任务」或
  `quickcmd kill <实例ID>`；日志：`quickcmd logs 7aec2859`
- daemon 自身跨手是安全的：实例真值在 `runtime/cores/*.core` + pid，新 daemon 起来
  即 reconcile（本次验证：webui-demo 保留为 running）
