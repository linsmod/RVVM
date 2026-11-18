# tinyssh-config — 构建配置与 overlay

`third_party/tinyssh` 是上游 submodule（`https://github.com/janmojzis/tinyssh.git`，
20261101 / `6a60c2a`）。本目录提供编译它所需的**平台答案**和**两处行为改动**，
让 submodule 本身保持干净（Windows 上 `git apply` 到 submodule 会因符号链接
被解引用而失败，所以不用补丁文件）。

## 1. `has*.h` — 平台探测结果

tinyssh 的 Makefile 用 `tryfeature.sh` 现场编译探测（`<pty.h>`、`<utmp.h>` …）。
交叉编译时那些答案必须是**目标平台**（riscv64-musl）的，不是构建机的，所以这里
把它们固定下来：

- `hasopenpty.h` → `#define HASOPENPTY 1`（musl 的 `openpty` 在 libc 里）
- 其余 26 个 → `#undef`（musl 没有 libutil.h，也没有 wtmp 记账）

`tryfeature.sh` 探测的宏名就是文件名大写；改动本目录等于改动「目标平台有什么」。

## 2. overlay 副本 — 两处改动

| 文件 | 改动 | 为什么 |
|---|---|---|
| `main_tinysshd.c` | 删除 none-auth 模式下「必须带 `-e`」和「拒绝 UID 0」两条检查 | guest 里只有 root 一个账户；`tinysshnoneauthd` 的这两条保护针对的是共享主机 |
| `packet_auth.c` | none-auth 时，客户端给的用户名在 `/etc/passwd` 里找不到，就解析成本进程 uid 对应的账户 | 让 `ssh 127.0.0.1 ls` 不必写用户名（WSL 的行为）；`channel_droppriv()` 仍需要账户存在才有 shell 和 home |

两个文件都是上游文件 + 顶部注释 + 上述改动。**没有别的差异**：
认证仍要求 `getpwnam()` 成功、`channel_droppriv()` 仍然 setuid/chdir，
也就是说 guest 的 `/etc/passwd` 必须有 `root:x:0:0:root:/root:/bin/sh`。

## 3. 启动（名字即行为）

tinysshd 是多调用二进制，`argv[0]` 的 basename 决定模式（`tinysshd.c`）：

| basename | 行为 |
|---|---|
| `tinysshd` | 公钥认证（`~/.ssh/authorized_keys`） |
| `tinysshnoneauthd` | 无认证（本项目用这个） |
| `tinysshd-makekey` | 生成 host key |
| `tinysshd-printkey` | 打印 host key |

所以 bundle 里是同一个 ELF，靠**硬链接/symlink 名字**分派，不重命名、不加包装层。

## 4. 升级 submodule 时

1. `git submodule update --remote src/virtpass/third_party/tinyssh`
2. `git -C src/virtpass/third_party/tinyssh log --oneline -1` 记下新 commit
3. 用上游的 `main_tinysshd.c` / `packet_auth.c` 覆盖本目录的两个副本，然后重新
   应用第 2 节的改动（两个文件顶部的注释说明了改哪里，diff 很小）
4. 若上游新增了 `tryfeature.sh` 探测项，在 `tinyssh-config/` 补对应的 `has*.h`
5. 重新编译，`ssh` 登录一次确认 none-auth 与退出码仍然正常
