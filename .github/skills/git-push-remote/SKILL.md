---
name: git-push-remote
description: '把本仓库推送到 GitHub / GitLab 远程。Use when: 推送代码、push、同步到 GitHub、同步到 GitLab、首次推送空仓库、把本地提交备份到内网。规则：每次执行必须先问用户推哪个远端（github / gitlab / both）；任何网络或认证问题只探测然后立即退出，不重试不绕过；只做 push，不 commit / 不 stash / 不 rebase / 不 force push。'
argument-hint: '<github|gitlab|both>'
---

# 推送到远程

## 何时使用

- 用户说"推一下""push""同步到 GitHub / GitLab""把代码传上去"。
- 第一次把本地仓库推到空的内网仓库。

## 本技能自包含

下面「远端事实」表里就是全部所需信息。**不要**从记忆文件、历史会话、旧终端输出里找补事实；
表里没有的信息（例如新远端地址）→ 直接问用户，不要猜。

## 远端事实

| 代号 | remote 名 | 地址 | 说明 |
|---|---|---|---|
| `github` | `origin` | `https://github.com/zagerx/EmbeddedCATS.git` | **保持不动**：不新建、不改地址、不切 SSH |
| `gitlab` | `gitlab` | `ssh://git@10.0.8.33:2222/zhangge/embeddedcats.git` | 内网 GitLab |

GitLab 三条硬性事实（写死，别现场试错）：

1. SSH 端口是 **2222**，22 端口在该网络**不可达**；地址必须写成 `ssh://git@10.0.8.33:2222/...` 的 URI 形式，不能用 `git@host:path` 简写（那默认 22）。
2. 该 GitLab 的 Web 端口 443 / 3000 均不可达，**只有 2222 能用**，别去试 HTTP 方式。
3. 目标仓库初始为空 ⇒ 首次 `push main` 会直接创建分支，属正常。

## 执行流程

### 1. 先问用户（强制，不可跳过）

用提问工具让用户三选一：**GitHub / GitLab / 两个都要**。

- 不许默认、不许沿用上次选择、不许从上下文推断。
- 拿不到用户答复（无交互）→ 直接结束，不推送。

### 2. 只读预检（只检选中的远端）

```bash
ROOT="$(git rev-parse --show-toplevel)" && cd "$ROOT"    # 仓库路径现场取，不写死
BR="$(git rev-parse --abbrev-ref HEAD)"                  # 当前分支

# gitlab 选中时
timeout 15 ssh -p 2222 -o BatchMode=yes -T git@10.0.8.33
timeout 20 git ls-remote ssh://git@10.0.8.33:2222/zhangge/embeddedcats.git

# github 选中时
timeout 20 git ls-remote origin
```

每条命令都带 `timeout`，**绝不允许无限等待**。

期望结果：GitLab 的 `ssh -T` 返回 `Welcome to GitLab, @zhangge!`；`ls-remote` 返回码 0
（输出为空 = 空仓库，正常）。

### 3. 命中任一条 → 立即退出（不救）

- 超时、`No route to host`、`Connection refused`、`Connection closed by remote host`
- `Permission denied (publickey)`、`401`、`403`、`Repository not found`
- 任何 `timeout 15/20` 触发

处理方式（固定）：

1. 打印判定表：**哪条命令、完整输出原文、结论一句话**。
2. 给出需要用户做的一件事（如"接公司内网""把公钥加到 GitLab"）。
3. **立刻结束本轮**。

禁止：重试、换端口（22↔2222）、开关代理/VPN、改 `/etc/hosts`、改 SSH 配置、`sudo`、猜测原因后继续推。

### 4. 推送

```bash
# 选中的远端，逐个推（both 时依次 github → gitlab）
timeout 120 git push gitlab "$BR"      # 或 git push origin "$BR"
```

- 只推**当前分支**：不加 `--all`、不加 `--tags`、不加 `--force`。
- 未提交改动：**不 commit、不 stash**，留着；在报告里列出 `git status --short` 原文。
- 远程已有内容且与本地历史无关（push 被拒，`non-fast-forward` / `unrelated histories`）→
  **停止并报告**，把"merge / rebase / force"三种选项交给用户选，自己不动手。

### 5. 推完回读验证

```bash
timeout 20 git ls-remote <remote> | head -n 5
timeout 10 git rev-parse HEAD
```

远程分支 SHA 必须等于本地 `HEAD`；不相等就报"推送未生效"，不要宣称成功。

## 报告格式（4～6 行）

- 推了哪个远端、哪个分支
- 提交范围：远程原 SHA（空则写"空仓库"）→ 本地 HEAD
- 新增提交数
- 未提交改动（原样列出，声明"仍在本地，未提交"）
- 结论：成功 / 已在第几步退出

## 铁律

1. 每次都问用户推哪个远端，不猜不改默认。
2. 任何网络 / 认证问题：探测、报告、退出一气呵成，**不做任何补救动作**。
3. 只 push：不 commit、不 stash、不 rebase、不 merge、不 reset、不 clean、不 force push。
4. 不动 `origin` 的地址与配置；GitLab 用单独的 remote 名 `gitlab`。
5. 不写没验证过的结论（"应该能通""大概推上去了"禁止出现）。
