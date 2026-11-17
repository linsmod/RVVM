# fix：userland hart 未注册导致每次 fork 泄漏 16 MiB RVJIT 堆

本条是历史修复记录，**不是现状**。现状见 `arch-userland.md`；排查方法见 `hist-lessons.md` §8。

## 现象

反复 `rvvm_ash -c "true"`（每次 fork 一个 shell 再退出）后 core 的 commit 单调上涨、idle 不回收：

```
baseline 1053.6 MiB → +16.2, +16.1, +16.1, ... 
```

## 根因

`rvvm_create_user_thread()`（`src/core/rvvm.c`）创建 hart 但**没有** `vector_push_back(machine->harts, ...)`
（只有 `rvvm_create_machine()` 才 push）。于是：

- `rvvm_free_user_thread()` 按列表查找 → 找不到 → 不释放；
- `rvvm_free_machine()` 遍历空列表 → 同样释放不到。

连带 16 MiB RVJIT 堆、condvar、aia 一起泄漏，每个 fork 的机器退出漏一份。

## 修法

在 `rvvm_create_user_thread()` 的 `global_lock` 块内、**timer 初始化之后**把 hart 注册进
`machine->harts`，让既有释放路径生效。

首次修复尝试选择「在 timer 初始化之后」而非之前，是因为 hart 结构在那之前未完整，
提前入表会让并发的释放路径看到半成品。

## 验证

- 修复后 churn 平：`baseline 1053.6 → 1053.6, 1053.8, 1053.8, ...`（8 次会话，idle 后仍平）。
- 对照实验：`RVVM_NOJIT=1` 起 core，churn 前后 1037.5 → 1037.6（证明泄漏确实在 JIT 堆）。

数值来源：原 `dev_debug_notes.md` §8 的实测记录，迁入时未重跑。
