# workflows 索引

工作流文档的写法要求见 `../AGENTS.md`「工作流」章节：适用条件、前置、环境、相关文件、经验、常用命令、工具引用、退出/切换条件。

| 工作流 | 适用场景 | 状态 |
|---|---|---|
| `wf-build-and-test-win32.md` | Windows 上构建 VirtPass 宿主 + userland 改动，并用 `tools/*_e2e.ps1` 验证 | 已建立（2026-11-17，命令未实测） |

## 尚未落地的工作流

`../AGENTS.md` 列出的常见场景（`wf-bugfix-*`、`wf-full-dev-pipe`、`wf-user-*`、`wf-add-new-tests`、
`wf-update-test-cases-or-suites`、`wf-update-workflows-itself`、`wf-design-and-impl-test-suite` 等）
本仓库**尚无对应文件**。按 AGENTS 规则「工作流不存在时按通常流程执行，完成后按实际过程初始化」：
哪个场景第一次真正发生，就在那次收尾时把真实过程抽成 `workflows/wf-<场景>.md` 并登记到本表，
不要凭空预先编造。
