# 第三方来源与本地修改

项目整体采用 Apache-2.0，许可证全文见根目录 LICENSE，版权与来源摘要见 NOTICE。以下第三方文件保留原许可证及版权声明；项目许可证不将第三方代码的版权归为作者所有。

## AVI 播放器

- 上游：https://github.com/espressif/esp-iot-solution
- 固定提交：`88152bff24d5fbc05ff2ffed3d5208c247711ded`
- 路径：`components/avi_player`
- 原许可证保留在 `project/flash_video_mvp/components/avi_player/license.txt`，原文件版权声明保留。
- 本地改动：离线组件构建与版本定义；修正 `movi` 列表结束边界，避免把 FFmpeg 后续 `idx1` 索引当成帧；检查零长度/超长读取。详情见该组件的 `UPSTREAM.txt` 与 `PATCHES.md`。

## JD9365 驱动

- 本地来源：开发期间既有 `ips_video_demo` 中的驱动副本；文件声明版权为 Espressif，SPDX 为 Apache-2.0。
- 未记录这份驱动的准确上游提交或组件版本，不将当前网络版本冒充此副本的版本。
- 保留供应商面板初始化序列；既有副本省略固定型号无需使用的 DSI RX ID 读取。本项目在应用层覆盖 DPI 请求为 60 MHz，避免当前 IDF 5.5.5 分频补偿形成无效行前沿。
- 驱动路径：`project/flash_video_mvp/components/esp_lcd_jd9365`。

## 运行时工具与素材

- ESP-IDF、FFmpeg、Python、Pillow 由使用者自行安装，本仓库不打包它们的二进制。
- 输入 MP4、转码 AVI、Flash 备份及供应商数据手册不作为本仓库的分发内容。
- 仓库诊断图由项目制作，用于颜色与方向检查；测试脚本通过 FFmpeg 生成测试图，不下载第三方视频。
