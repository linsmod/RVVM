# 任务：让 MAP_SHARED 真正共享（SQLite WAL 可用）

> 执行层文档。起点 2026-11-20，分支 `android-abi-host`，起点 commit `26f6a90`。
> 会话中断后按本文件恢复：读 §2 现状、§3 计划、§7 恢复点。

## 1. 目标与非目标

**目标**：guest 内 `mmap(..., MAP_SHARED, fd, ...)` 在两个 guest 进程之间真正共享同一块字节，
于是 SQLite 的 `-shm` wal-index 可以工作，`PRAGMA journal_mode=WAL` 不再损坏或失败。

**非目标（明确不承诺）**：

- 跨 core（跨宿主进程）共享同一 db 文件：锁表也不共享（`fix-fcntl-record-locks.md` 遗留），仍不支持。
- 崩溃安全：`MAP_SHARED` 数据文件只在 `msync` 与最后一次 `unmap` 写回，不是每页落盘。`PRAGMA mmap_size` 仍建议 0。
- memfs（`/tmp`）上的 `MAP_SHARED`：第一阶段返回 `ENODEV`。
- `MAP_SHARED|MAP_ANONYMOUS` 的跨进程共享（`shm_open` 类用法）：不在范围内，行为不变（现在各进程独立）。

## 2. 现状与根因（改动前的事实）

| 事实 | 位置 |
|---|---|
| 每个 guest 进程（`rvvm_userland_t`）有自己的 machine，`mem.data` 是一整块连续宿主分配 | `cow_pgt.h:20-23`；`riscv_init_ram()` |
| 文件映射只 `pread` 一份到 guest 私有内存，写回不模拟 | `rvvm_user.c:11893-11895` |
| `MAP_SHARED` 与 `MAP_PRIVATE` 在 mmap 路径上完全等价 | 同上 |
| 页表基础设施已写好但**未接线**：`mem->pgt` 全树唯一赋值是 NULL | `riscv_mmu.c:59` |
| paged 访问器在生产代码里没有调用者（只在 `mem->pgt != NULL` 时可达） | `rvvm.c:1262-1297`，`rvvm.h:246-282` |
| MMU 与解释器已经全部走 `rvvm_ram_ptr()` / `rvvm_ram_const_ptr()` | `riscv_mmu.c:166-192` |
| TLB 失效原语存在（整表 / 单页） | `riscv_mmu.c:83,97` |
| 页表的 TLB 一致性约定与危险点 | `cow_pgt.h:198-232, 273-289` |

**结论**：WAL 不可用的根因不是"没有 writeback"，而是**两个 guest 进程没有共同的物理页**。
要共享必须引入页级指针间接（页表），因此必须先接线 `cow_pgt`。

## 3. 计划（三步，各自可交付）

### P0 止血：可写 `MAP_SHARED` 明确失败（本次已做）

`mmap(MAP_SHARED, fd)` 且请求 `PROT_WRITE` 时返回 `ENODEV`，不再静默返回一份私有副本。
静默副本会让 SQLite 认为共享成功，最终表现为 `disk I/O error` / 数据库损坏，比失败更糟。
只读 `MAP_SHARED`（`PROT_READ`）保持现状（快照语义，没有写所以不会损坏）。

代码点：`rvvm_user.c` `rvvm_sys_mmap()`，在 `guest_range_alloc()` **之前**拦截（否则会泄漏 guest 地址区间）。

### P1 接线页表（前置，独立任务）

1. per-run（一个宿主进程内）创建 `cow_pgt_region`：大小 = guest RAM，只读 base。
2. 每个 guest 进程的 machine 用 `cow_pgt_machine_new()`；fork 出来的子进程用
   `cow_pgt_machine_fork()`（继承父的脏页集合）。
3. `mem.pgt` 在创建 hart **之前**设置（hart 按值拷贝 region，`rvvm.h:192-196`）。
4. `mem.data` 语义：保持为 region base 指针，供 `rvjit`/`elf_load` 的偏移算术使用；
   凡是用它当写指针的地方必须改走 `rvvm_ram_ptr(..., true)`。
5. **TLB 失效**：在每个 unshare 发生的边界调用 `cow_pgt_take_unshared()`，把
   `[first_page,last_page]` 转成地址区间后用 `riscv_tlb_flush_page()` 失效；
   批量场景退回 `riscv_tlb_flush()`。这是 `cow_pgt.h:273-289` 点名的唯一危险。
6. exec/exit：exec 清空页表（新地址空间）、machine free 释放页表与 arena。
7. JIT：`rvjit_tail_lookup()` 用 `ptr - vm->mem.data` 算物理 pc（`rvjit_emit.c:248`），
   页表下 `ptr` 可能落在 arena。若不能低成本修正，userland 路径强制解释器执行
   （`RVVM_NOJIT` 已存在），另登记条目跟进。

验收：既有全量回归 + `pwsh ./tools/pgt_smoke.ps1` + `pwsh ./tools/pgt_mem_probe.ps1`。

### P2 共享页（WAL 的真正修复）

1. `cow_pgt` 增加第三种页状态 **foreign run**：
   `{ first_page, pages, uint8_t* host, void* owner }` 的有序小数组（预期 < 16 条）。
   翻译：命中 run → 直接返回 `host + delta`；**永不 COW、永不被 unshare 覆盖**。
   新增 API：`cow_pgt_bind_shared()`、`cow_pgt_unbind_shared()`、`cow_pgt_images()`（区间报告，供 TLB 失效）。
2. `rvvm_user.c` 增加 **per-run 共享区域注册表**（全局 + 锁）：
   key = 宿主文件身份（dev+ino，复用 `fd_lock_file_info()` 那套）+ 偏移 + 长度；
   value = 堆分配的字节区（`vma_alloc`）、refcount、dirty 位。
   首次映射 `pread` 初始内容；最后一个 unmapper 写回并释放。
3. `rvvm_sys_mmap()`：`!(MAP_ANON) && fd >= 0 && (MAP_SHARED) && 宿主文件` → 绑定共享 run；
   `MAP_PRIVATE` 保持现有复制行为（动态加载 `.so` 不受影响）。
4. `munmap` 部分/全部释放 → 回 base（并清零，Linux 语义）；`msync` → 写回；
   `mremap` 命中 foreign run → 返回 `ENODEV`（不再静默复制）。
5. fork：子 machine 复制 foreign run 列表并 `refcount++`；exec/exit 释放。

验收：新探针 `probe_mmap_shared` + 真 sqlite3（两进程并发写 WAL）。

## 4. 为什么不用宿主真 mmap

- Windows `MapViewOfFile` 偏移必须 64K 对齐，SQLite wal-index 按 32K 区域映射；
- 截断映射中的文件会 SIGBUS；
- 它能提供的共享范围（同一宿主进程）与堆共享完全相同。

故选择堆共享 + 页表 foreign run，平台无关。

## 5. 探针设计（P0 即开始使用）

`src/virtpass/guest-samples/probe_mmap_shared.c`，`--probe` 模式下运行：

| 用例 | 期望 |
|---|---|
| P0-A `mmap(PROT_READ|PROT_WRITE, MAP_SHARED, fd)` | `MAP_FAILED` 且 `errno == ENODEV` |
| P0-B `mmap(PROT_READ, MAP_SHARED, fd)` | 成功，内容等于文件初始内容 |
| P0-C `mmap(MAP_PRIVATE|PROT_WRITE)` | 成功，写入后文件不变 |
| P2-A 父 + 两个 fork 子各写不同偏移 | 三方读到的都是对方写的值（互见） |
| P2-B `msync` + `munmap` 后新进程重新映射 | 内容持久 |
| P2-C fork 后父 unmap、子继续写 | 子不受影响，父持有引用时不提前释放 |

## 6. 测试与回归

- 主要判据：`probe_mmap_shared`（`--probe`）+ 真 sqlite3 的 WAL 并发写。
- 回归：`tools/fs_selftest.ps1`、`tools/hostfs_selftest.ps1`、`tools/memfs_selftest.ps1`、
  `tools/supervisor/selftest.py`；P1 之后加 `tools/pgt_smoke.ps1`、`tools/pgt_mem_probe.ps1`。
- 耗时登记到 `docs/test-timing.md`。

## 7. 恢复点

| 步骤 | 状态 | 验证 |
|---|---|---|
| P0 可写 MAP_SHARED 返回 ENODEV | **已完成 2026-11-20** | `probe_mmap_shared` PASS：`SKIP process-to-process sharing: writable MAP_SHARED is ENODEV`；只读共享快照、私有映射不回写均 PASS；0.04 s |
| P1 页表接线 | 未开始 | 全量回归 + pgt_smoke + pgt_mem_probe |
| P2 共享页 + 注册表 | 未开始 | probe P2-A/B/C + sqlite3 WAL |

### P0 恢复点（2026-11-20）

- 代码点：`rvvm_user.c` `rvvm_sys_mmap()` 在 `guest_range_alloc()` 之前拦截
  `MAP_SHARED && !MAP_ANON && PROT_WRITE` → `-UAPI_ENODEV`（放在分配前，否则泄漏 guest 地址区间）。
- 探针：`src/virtpass/guest-samples/probe_mmap_shared.c`（能力探测式，P2 落地后不改探针，
  自动从 SKIP 切到执行共享页用例）。
- 验证命令（本机实测 PASS，0.04 s）：
  ```
  mingw32-make guest-assets GUEST_SAMPLES=probe_mmap_shared
  cp release.windows.x86_64/guest-assets/probe_mmap_shared.exe \
     release.test.windows.x86_64/runtime/rootfs/probe_mmap_shared
  release.windows.x86_64/rvvm_user_x86_64.exe \
     -prefix release.test.windows.x86_64/runtime/rootfs /probe_mmap_shared
  ```
- 已知缺口：`msync`（riscv64 syscall 227）尚未实现，落到 unknown → `ENOSYS`；探针容忍它，
  P2 需补 flush。


## 8. 风险

- P1 动的是全树最核心的翻译路径，回归面最大；`cow_pgt.h` 自己写明 TLB 一致性是唯一危险点。
- fork 既有坑会被放大：`cow_pgt.h:98-102`（父 munmap 过的区间在子进程是私有 stale 页）必须在接线时清零。
- 测试需独立数据目录/端口（沿用 supervisor selftest 的范式）。
