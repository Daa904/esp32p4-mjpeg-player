# 首次上传 GitHub（本机操作说明）

发布源目录为 `D:\esp32p4\github\esp32p4-mjpeg-player`。运行工程和完整备份仍在工作区中；本发布目录包含源码、命令封装、使用与排错文档。上传通过 Git 完成，Git 会按 .gitignore 排除本地配置、用户视频、生成素材和构建缓存。

## 1. 在网页创建空仓库

GitHub 右上角 `+` → `New repository`。

- Owner：你的个人账号。
- Repository name：建议 `esp32p4-mjpeg-player`，也可以自己命名。
- Description：`ESP32-P4 MJPEG video player with MIPI timing debugging notes and a one-command MP4 flashing workflow.`
- Visibility：Public。
- 不勾选 Add a README，不在网页添加 .gitignore 或 License；本地已有文件，稍后一起上传。
- 点击 Create repository，复制 Quick setup 区域中的 HTTPS 仓库地址。

仓库创建方法与空仓库导入建议来自 [GitHub 官方文档](https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github)。

## 2. 在 PowerShell 上传

以下操作只在发布源目录执行。将最后部分的仓库地址换成刚复制的真实地址：

```powershell
cd D:\esp32p4\github\esp32p4-mjpeg-player
git init -b main
git add .
git status --short
```

检查清单应包含 README、源码、脚本、许可证及 docs；不应出现 local-config.ps1、mp4 视频、clip.avi、build、Flash 备份。根目录 LICENSE 按作者确认的许可证加入后再提交。

```powershell
git commit -m "Initial release: MIPI timing fix and MP4 flashing workflow"
git remote add origin https://github.com/你的用户名/你的仓库名.git
git push -u origin main
```

第一次 HTTPS 推送可能打开浏览器登录确认，使用自己的 GitHub 账号完成。不要将登录令牌复制到仓库文件中。若出现 `Author identity unknown`，按 Git 提示设置仅此仓库的名字和 GitHub 邮箱后重新提交；不必改全局设置。

如果再次执行提示 origin 已存在，先 `git remote -v` 核对地址，正确时直接执行 push，不重复添加 remote。若地址不符，先核对再使用 `git remote set-url origin 真实地址`。

## 3. 网页检查

刷新仓库，确认首页显示完整 README：时序缺陷与 60 MHz 修复、命令行封装、验证边界、使用步骤均应可见。检查 docs 内链接和诊断图；确认没有上传个人视频、本机配置或备份。

## 4. 后续更新

修改发布目录中的文件，检查变化再提交推送：

```powershell
cd D:\esp32p4\github\esp32p4-mjpeg-player
git status --short
git add .
git commit -m "Describe the change"
git push
```

本地开发工程与发布目录是两个目录，修改开发工程不会自动同步到发布副本。发布前同步对应源码和文档，再检查、测试与提交；不要把 build 或备份复制进去。
