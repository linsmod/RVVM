# fix: sendfile(2) 遇到 tmpfs 会静默截断，`cat/cp` 从 guest 复制大文件丢第一个 32K

最后核对：2026-11-19。

## 现象

guest 里从 hostfs/asset 复制大文件到 tmpfs（`/tmp` 默认就是 tmpfs）时，**目标文件比源文件
少 32768 字节，且从第 1 个字节起内容就是错的**，命令本身退出码 0、不报错：

```
$ cp /bin/busybox /tmp/bb
$ wc -c /tmp/bb            # 792320，源是 825088
$ cmp /bin/busybox /tmp/bb # differ: char 1
$ tail -c +32769 /bin/busybox > /tmp/expect
$ cmp /tmp/expect /tmp/bb  # 完全相同 —— 丢的是前 32K
```

顺带的症状：`cat /bin/busybox > /tmp/x` 之后执行 `/tmp/x` 会报
`Not an ELF file`，再往下还会在 core 日志里出现一条 guest 异常
（`Exception 5 ... at PC ...`）——那是 guest 在跑被截断的 ELF，不是模拟器崩了。
`sort` 在读者提前退出时也报过 `standard output: Invalid argument`（另一个问题，见 issues）。

## 根因

`busybox` 的 `cat`/`cp` 走 `bb_copyfd_eof()` → `sendfile(dst, src, NULL, 16MB)`。
模拟器的 `rvvm_sys_sendfile()` 用 `userland_read_fd()`/`userland_write_fd()` 搬字节，
而这两个 helper 只认得 host fd、pty、/dev、console，**不认 memfs（tmpfs）fd**：

- tmpfs fd 的“host 号”是个合成数字（实测 `0x7E000001`），`write()` 到它上面返回 EBADF；
- 但此时 sendfile 已经先从源读走了 32K（`SENDFILE_CHUNK`），于是返回负值；
- busybox 收到负值后 `sendfile_sz = 0` 退回自己 read/write 循环——从**已经前进过的**
  文件位置继续读，于是输出正好缺开头一个 chunk，且全程无错误。

普通 `read`/`write` 系统调用有自己的 memfs 分支（`memfs_fd_for_guest()` + `rvvm_memfs_*`），
所以 `dd` 一切正常——这也是为什么只有 `cat`/`cp` 这类走 sendfile 的路径出错。

## 修法

`rvvm_sys_sendfile()` 在搬第一个字节前先检查两端：任一端是 memfs fd 就返回 `-ENOSYS`。

- “没有 sendfile 的内核”是标准行为（2.6.33 之前），busybox 和其它复制循环收到 ENOSYS
  都会退回 read/write，语义正确；
- 关键是**在任何字节被消费之前**返回，这样回退从正确位置开始；
- 没有去在 `userland_read_fd/write_fd` 里复制一份 memfs 的 offset/append 逻辑：那会把
  写系统调用里的规则复制成第二份，长期必然漂移。

## 验证

同一个 guest（`--serve` + ssh）：

```
cp /bin/busybox /tmp/bb; wc -c /tmp/bb        → 825088
cmp -s /bin/busybox /tmp/bb && echo COPY-SAME → COPY-SAME
cat /bin/busybox > /tmp/cc; wc -c /tmp/cc     → 825088, CAT-SAME
/data/app/test_jobctl/bin/test_jobctl.exe     → 仍是基线的 8 项 ctty FAIL，无新增
```

对照修复前：`cp`/`cat` 均为 792320 且 `differ: char 1`。
