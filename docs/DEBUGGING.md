# MIPI-DSI 绿屏与帧完成超时：调查记录

本记录对应 Waveshare ESP32-P4-WIFI6、ESP32-P4 rev 3.2、ESP-IDF 5.5.5 和 WX101BH020I-40Z / JD9365D。不是所有 JD9365 屏幕通用的补丁。

## 现象与日志含义

硬件彩条完整显示，CPU 帧缓冲显示绿屏、底部杂色；打印 `No LCD frame completion; stop before reusing a buffer`。这来自应用 `present_frame()` 的 1000 ms 信号量等待，保护旧缓冲不被过早复用。日志中的 `E (3129)` 表示错误级别及启动后约 3129 ms，不是驱动错误编号。

`on_frame_buf_complete` 来自 DMA 整帧完成；当前直接帧缓冲路径中，`on_color_trans_done` 可以在提交函数内同步调用，不能拿它替代扫描完成。VSYNC 增长也不能独自证明像素数据已传完。

## 逐步验证

| 阶段 | 修改/观察 | 结果与判断 |
|---|---|---|
| 初版 | 初始化面板、显示 DSI 硬件彩条，随后提交 CPU 五色图 | 彩条正常，CPU 帧超时；硬件彩条不足以证明帧缓冲路径正常 |
| V2 | 跳过硬件彩条，直接使用 CPU framebuffer | 仍绿屏和杂色；模式切换不是充分解释 |
| V3 | 解码输出、PSRAM 缓冲、DSI 全部统一 RGB888 | `frame=0->0`、`vsync=9->67`；颜色格式统一没有修复缺失的整帧事件 |
| V4 | 增加 DMA/DSI 寄存器快照；断电后按原方向重新安装 22 针排线 | `frame=0->0`、`vsync=14->73`；DMA 源地址已推进 2,849,216 字节但不再推进，一帧应为 3,072,000 字节 |
| V5 | 对照分频与行时序补偿，DPI 请求改为 60 MHz，增加行/场时序读回 | 用户确认 CPU 红绿蓝白黑正确；JPEG 方向与边框正确，红蓝互换 |
| V6 | JPEG 解码字节顺序改为 BGR | 用户确认 JPEG 颜色正确、视频循环播放正常 |
| V7 | JPEG 直接解码至空闲 LCD framebuffer，减少整帧 CPU 复制 | 连续多轮 12 fps 素材实测 11.98～11.99 fps |

V4 的 DMA `status=0x10` 是目的端事务完成状态，不能当作整帧完成或总线错误；日志没有证明对应总线错误位。早期出现过本地 ELF 与已烧录固件不一致，之后核对版本与 ELF 哈希再解释新日志，避免混淆旧固件。

## 时序缺陷与最小修复

面板宏原配置为：有效宽度 800、HSYNC 20、HBP 20、HFP 40；有效高度 1280、VSYNC 4、VBP 10、VFP 30。请求 68 MHz，默认 DPI 源为 PLL240。

IDF 5.5.5 中的 `mipi_dsi_hal_host_dpi_calculate_divider()` 使用四舍五入选整数分频；`mipi_dsi_hal_host_dpi_set_horizontal_timing()` 按真实/请求频率之比补偿行总长：

```text
round(240 / 68) = 4 → 实际 60 MHz
原 HTOTAL = 880
round(60 / 68 × 880) = 776
补偿值 = 776 - 880 = -104
HFP = 40 - 104 = -64
```

这违反了行总长必须容纳有效像素、同步及后沿的要求。应用创建面板前设置 `dpi.dpi_clock_freq_mhz = 60`，使 PLL240/4 恰好匹配请求；保留 RGB888、960 Mbps/lane、供应商 porch 和初始化序列。

修复后 HTOTAL=880、HFP=40、VTOTAL=1324，计算扫描刷新约 51.4968 Hz。[计算文件](evidence/dpi_timing_check.json)可供检查。没有修改系统 IDF 或供应商默认宏，也没有通过删除超时保护强行继续运行。

失败版未打印实际行时序读回，因此“776/-64”是本机源码和配置推算；修复后的正确五色图、JPEG 和视频有用户实测反馈。结论的适用条件包括此时钟源、此 IDF 实现与此面板配置，移植时需重新核对。

## 颜色与性能是后续独立问题

JPEG 红蓝互换用 `.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR` 修复。文字方向和四边框正确，说明预旋转与显示范围也得到了画面验证。

V6 解码到中间 RGB888 缓冲，再复制到 LCD framebuffer；每帧复制 3,072,000 字节。12 fps 素材的实际基线为 60/60 帧、8.15 秒/轮、7.36 fps，平均 decode=14.3 ms、present=118.0 ms。

V7 直接解码到空闲 LCD framebuffer；使用双缓冲，提交后保留两次新的 DMA 帧完成等待，避免过早复用。输入缓冲仍单独分配及复制。实测 60/60 帧、5.005～5.008 秒/轮、11.98～11.99 fps、decode=13.7 ms、present 约 29.5 ms，多轮空闲堆保持一致。

24/30 fps 是后续素材对照；作者报告顺畅，但未取得足够统计来声称稳定实现 30 fps。显示扫描约 51.5 Hz、素材 30 fps、日志的 submitted_fps 应分别理解。

## 容量与工作流的收尾

MP4 自动转 MJPEG 后体积明显增加；固定 30 fps、1280×720 视频区域、横屏补边后预旋转到 800×1280。转码器检查所有 JPEG、AVI 帧数与尺寸，再发布生成素材。

20 秒视频嵌入应用触发了 esptool 单段 16 MiB 限制，单纯扩大应用分区无效。改为 2 MiB factory 程序与 29 MiB video 数据分区，普通完整 flash 清单包含原始 AVI，启动检查长度后 mmap 读取。[当前验证记录](evidence/video_partition_validation.json)证明 26.01 MiB 视频可通过电脑构建和布局检查；这一版的长视频硬件播放仍待验证。

## 参考

- [本机对应 IDF 5.5.5 HAL 实现](https://github.com/espressif/esp-idf/blob/v5.5.5/components/hal/mipi_dsi_hal.c)
- [DPI 帧事件实现](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_lcd/dsi/esp_lcd_panel_dpi.c)
- [Espressif LCD FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html)
- [相关 JD9365 问题 #259](https://github.com/esp-arduino-libs/ESP32_Display_Panel/issues/259)：是排查参考，不将本项目结论表述为该问题的官方解决方案。
