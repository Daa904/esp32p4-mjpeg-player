# 播放器固件

项目说明、MIPI 时序排错过程和用户命令在[仓库首页](../../README.md)。

日常从仓库根目录执行 `play.cmd`，它先生成视频素材，再编译并完整烧录。直接打开本目录编译前，也必须先转码生成 `main/clip.avi` 和 `main/clip_info.h`。

固件使用 ESP-IDF 5.5.5、esp32p4 目标；2 MiB factory 应用分区和 29 MiB video 数据分区。当前分区分离版的硬件验证状态见首页。
