# 任务：给 guest 的系统层铺基础工具（sqlite / curl / python3）

> 执行层文档。2026-11-20，分支 `android-abi-host`，起点 commit `1866bd0`。

## 1. 目标

guest 只有 Alpine minirootfs + busybox + 我们的系统程序（`vpshhd`/`tinysshd`）。要让它能当"跑用户程序的机器"用，
至少要有 `sqlite3`、`curl`，最好能跑 `python3`。

## 2. 关键事实（决定做法）

- guest 的 rootfs 是 Alpine minirootfs，里面的 busybox **是动态链接的**（`PT_INTERP=/lib/ld-musl-riscv64.so.1`），
  而且 `apk --version` 能在 guest 里跑通——所以**发行版的 riscv64 二进制可以直接用**，不需要交叉编译。
- 因此不移植源码，而是把 Alpine 的 `.apk` 铺进 **system 层**（`system.tar.gz`，启动时叠加在 rootfs 之上），
  依赖由镜像自己的 `APKINDEX` 解析。

## 3. 改动

| 文件 | 改动 |
|---|---|
| `tools/pack_system.py` | `--pkgs` / `--mirror` / `--branch` / `--arch` / `--cache`；按 `APKINDEX`（main+community）解析依赖闭包（`D:` 空格分隔、版本约束、`p:` 提供的 `so:`/`cmd:`），下载 `.apk`、按成员类别（目录/普通文件/符号链接）写进输出 tar；`.SIGN.*`、`.PKGINFO` 丢弃；mtime/uid 归零保持确定性。已在 rootfs 里的包（musl/busybox/apk-tools 等）跳过 |
| `project.mk` | `SYS_PKGS ?= sqlite`、`SYS_PKG_CACHE := $(BUNDLE_DIR)/.apkcache`；`pack-system` 传 `--pkgs`/`--cache`。逗号分隔，避免 make 配方里的引号问题 |
| `tools/INDEX.md` | 登记 `pack_system.py --pkgs` |

缓存放在 bundle 目录下：第一次打包联网，之后重打不必联网。

## 4. 验证（本机实测）

```
make pack-system                          # 默认 sqlite
# 新实例（全新 rootfs）里：
which sqlite3                             -> /usr/bin/sqlite3
sqlite3 --version                         -> 3.53.4
cd /persist && sqlite3 d.db "PRAGMA journal_mode=DELETE; CREATE TABLE t(v);
                             INSERT INTO t VALUES(42); SELECT v FROM t;"
                                          -> delete / 42
us stop <port>; POST /api/instances/<port>/start; sqlite3 /persist/d.db "select v from t"
                                          -> 42          # 跨 core 生命周期存活
```

同一镜像下另测（`SYS_PKGS=sqlite,curl,python3` 重打后）：

- `curl --version` → 8.23.0（OpenSSL 3.5.8、c-ares、nghttp2 全在）
- `python3 -V` → 3.14.10
- `busybox wget http://example.com/` → 成功（网络与 DNS 本身没问题）

## 5. 两个模拟器缺口（阻塞 curl/python3，非打包问题）

1. **已修复：`eventfd2`（syscall 19）在非 Linux 宿主缺失** → 原来 curl 任何请求都是
   `(27) Out of memory`（c-ares 用它做唤醒 fd）。`rvvm_user.c` 现在在非 Linux 分支用合成 fd
   实现计数器（读消费/写相加、poll 就绪、fstat/lseek、close/dup/fork/F_SETFL 全套分派）；
   Linux 分支仍走宿主 `eventfd()`。验证：`Unknown syscall 19!` 消失、OOM 消失、
   `curl http://1.1.1.1/` 得 301。
2. **已修复：c-ares 的域名解析失败**（`Could not contact DNS servers`）。core 的 verbose 日志
   显示 c-ares 建 UDP socket 后 `fcntl(F_SETFL, 0x8802)` 得到 EBADF：socket anchor 的号段不是
   宿主 CRT fd，不能交给宿主 `fcntl()`。`userland_fd_serves_own()` 现在把 anchor 算作本层对象，
   F_SETFL 从 guest 标志字回答，宿主侧用 `win_socket_set_nonblock()`。
   验证：`curl http://example.com/` → **200**。
3. **已修复：`python3` 在解释器初始化阶段 `a_crash()`**。`reloc: 1ae06` → ld-musl 的
   `__stack_chk_fail`；mmap trace 给出 libpython 基址 `0x84a0000`，返回地址 `0x867c4f6` →
   `PyOS_getsig+0x42`，即栈保护金丝雀校验失败。根因：`rt_sigprocmask`（syscall 135）把 guest 的
   8 字节 `oldset` 交给宿主 `sigprocmask()`（宿主 `sigset_t` 为 16 字节），多写的 8 字节打坏
   相邻 canary。改为 guest 形状的 `uint64_t sig_mask` 自己实现 BLOCK/UNBLOCK/SETMASK，
   `oldset` 只写 8 字节。验证：`python3 -c "print(1+1)"` → 2 / rc=0；
   `import sqlite3,ssl,json,urllib.request` 正常；python 在 `/persist/py.db` 建表/插入/读回
   `[(42,)]`；`test_std` 159 checks / 0 failed。

三个模拟器缺口都已修好，默认 `SYS_PKGS` 恢复为 `sqlite,curl,python3`。

## 6. 恢复点

| 步骤 | 状态 |
|---|---|
| `pack_system.py --pkgs`（依赖解析 + 缓存 + 成员类型） | 完成 |
| `project.mk` 接线、默认只 sqlite | 完成 |
| guest 内 sqlite 端到端（含跨重启） | 完成 |
| eventfd2（非 Linux 合成 fd） | 完成；curl OOM 修复 |
| c-ares DNS（F_SETFL on socket anchor） | 完成；`curl http://example.com/` → 200 |
| python3 a_crash（rt_sigprocmask 8→16 字节溢出） | 完成；python3 跑代码、import、/persist 上写库全通过 |

## 6. 默认集合

`project.mk` 的 `SYS_PKGS ?= sqlite,curl,python3`；`--pkgs` 依赖解析与缓存见 §3。
`make pack-system SYS_PKGS=sqlite` 可只打最小集合。
