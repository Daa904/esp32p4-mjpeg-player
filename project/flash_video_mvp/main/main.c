#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "avi_player.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "clip_info.h"
#include "soc/dw_gdma_struct.h"
#include "soc/mipi_dsi_bridge_struct.h"

#define FRAME_BYTES (CLIP_WIDTH * CLIP_HEIGHT * 3)

static const char *TAG = "flash_video";
extern const uint8_t diagnostic_start[] asm("_binary_diagnostic_jpg_start");
extern const uint8_t diagnostic_end[] asm("_binary_diagnostic_jpg_end");
static esp_lcd_panel_handle_t panel;
static jpeg_decoder_handle_t decoder;
static uint8_t *framebuffers[2];
static unsigned next_fb = 1;
static SemaphoreHandle_t frame_complete, play_complete;
static uint8_t *jpeg_input;
static size_t input_capacity;
static unsigned displayed_frames;
static int64_t decode_total_us, present_total_us;
static int64_t submit_total_us, wait_total_us;
static volatile unsigned frame_event_count, vsync_event_count;

static void log_display_dma_state(const char *stage)
{
    // Read-only snapshots: do not clear status or change DMA/DSI configuration.
    ESP_LOGI(TAG, "DMA SNAPSHOT %s: cfg=%08lx enabled=%08lx irq=%08lx common=%08lx",
             stage, (unsigned long)DW_GDMA.cfg0.val, (unsigned long)DW_GDMA.chen0.val,
             (unsigned long)DW_GDMA.int_st0.val, (unsigned long)DW_GDMA.common_int_st0.val);
    for (int i = 0; i < 4; i++) {
        volatile dmac_channel_reg_t *ch = &DW_GDMA.ch[i];
        ESP_LOGI(TAG, "DMA%d: src=%08lx dst=%08lx llp=%08lx block=%lu transferred=%08lx",
                 i, (unsigned long)ch->sar0.val, (unsigned long)ch->dar0.val,
                 (unsigned long)ch->llp0.val, (unsigned long)ch->block_ts0.val,
                 (unsigned long)ch->status0.val);
        ESP_LOGI(TAG, "DMA%d: status=%08lx signal=%08lx status_en=%08lx ctl=%08lx/%08lx cfg=%08lx/%08lx",
                 i, (unsigned long)ch->int_st0.val, (unsigned long)ch->int_sig_ena0.val,
                 (unsigned long)ch->int_st_ena0.val, (unsigned long)ch->ctl0.val,
                 (unsigned long)ch->ctl1.val, (unsigned long)ch->cfg0.val,
                 (unsigned long)ch->cfg1.val);
    }
    ESP_LOGI(TAG, "DSI: fifo=%08lx pixel=%08lx flow=%08lx enabled=%08lx dpi=%08lx",
             (unsigned long)MIPI_DSI_BRIDGE.fifo_flow_status.val,
             (unsigned long)MIPI_DSI_BRIDGE.pixel_type.val,
             (unsigned long)MIPI_DSI_BRIDGE.dma_flow_ctrl.val,
             (unsigned long)MIPI_DSI_BRIDGE.en.val,
             (unsigned long)MIPI_DSI_BRIDGE.dpi_misc_config.val);
    ESP_LOGI(TAG, "DSI timing: htotal=%u hdisp=%u hsync=%u hbp=%u vtotal=%u vdisp=%u",
             (unsigned)MIPI_DSI_BRIDGE.dpi_h_cfg0.htotal,
             (unsigned)MIPI_DSI_BRIDGE.dpi_h_cfg0.hdisp,
             (unsigned)MIPI_DSI_BRIDGE.dpi_h_cfg1.hsync,
             (unsigned)MIPI_DSI_BRIDGE.dpi_h_cfg1.hbank,
             (unsigned)MIPI_DSI_BRIDGE.dpi_v_cfg0.vtotal,
             (unsigned)MIPI_DSI_BRIDGE.dpi_v_cfg0.vdisp);
}

static bool on_frame_complete(esp_lcd_panel_handle_t handle,
                              esp_lcd_dpi_panel_event_data_t *event, void *ctx)
{
    BaseType_t woken = pdFALSE;
    frame_event_count++;
    xSemaphoreGiveFromISR(frame_complete, &woken);
    return woken == pdTRUE;
}

static bool on_vsync(esp_lcd_panel_handle_t handle,
                     esp_lcd_dpi_panel_event_data_t *event, void *ctx)
{
    vsync_event_count++;
    return false;
}

static void present_frame(void)
{
    unsigned frames_before = frame_event_count;
    unsigned vsyncs_before = vsync_event_count;
    int64_t start = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, CLIP_WIDTH, CLIP_HEIGHT,
                                            framebuffers[next_fb]));
    int64_t submitted = esp_timer_get_time();
    // Discard any completion predating this submission. Two subsequent DMA
    // completions cover an interrupt already in flight and the new buffer switch.
    xSemaphoreTake(frame_complete, 0);
    for (int i = 0; i < 2; i++) {
        if (xSemaphoreTake(frame_complete, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "No LCD frame completion; stop before reusing a buffer");
            ESP_LOGE(TAG, "events: frame=%u->%u vsync=%u->%u waits_completed=%d/2",
                     frames_before, frame_event_count, vsyncs_before, vsync_event_count, i);
            log_display_dma_state("timeout");
            ESP_LOGE(TAG, "HALTED: no more frame submissions; reset to retry");
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
    submit_total_us += submitted - start;
    wait_total_us += esp_timer_get_time() - submitted;
    next_fb ^= 1;
}

static void show_jpeg(const uint8_t *data, size_t size)
{
    assert(size <= input_capacity);
    memcpy(jpeg_input, data, size);
    jpeg_decode_cfg_t config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        // Match the B, G, R byte layout verified by the CPU color test.
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    jpeg_decode_picture_info_t info;
    ESP_ERROR_CHECK(jpeg_decoder_get_info(jpeg_input, size, &info));
    assert(info.width == CLIP_WIDTH && info.height == CLIP_HEIGHT);
    uint32_t decoded_size;
    int64_t start = esp_timer_get_time();
    // Decode into the idle LCD buffer. present_frame() waits before reusing
    // the previous buffer; the JPEG driver handles output cache invalidation.
    ESP_ERROR_CHECK(jpeg_decoder_process(decoder, &config, jpeg_input, size,
                                        framebuffers[next_fb], FRAME_BYTES, &decoded_size));
    assert(decoded_size == FRAME_BYTES);
    int64_t decoded = esp_timer_get_time();
    present_frame();
    decode_total_us += decoded - start;
    present_total_us += esp_timer_get_time() - decoded;
}

static void on_video_frame(frame_data_t *frame, void *ctx)
{
    assert(frame->video_info.frame_format == FORMAT_MJEPG);
    show_jpeg(frame->data, frame->data_bytes);
    displayed_frames++;
}

static void on_play_complete(void *ctx)
{
    xSemaphoreGive(play_complete);
}

#if CONFIG_MVP_DIAGNOSTIC_BOOT
static void wait_for_key(int key)
{
    ESP_LOGI(TAG, "VISUAL CHECK REQUIRED: send '%c' only if this screen is correct", key);
    while (getchar() != key) {
        clearerr(stdin); // UART's nonblocking read can set EOF while no key is available.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void run_diagnostics(void)
{
    ESP_LOGI(TAG, "DIAGNOSTIC V7: direct JPEG to LCD buffer, BGR, 60MHz, frame=%u vsync=%u",
             frame_event_count, vsync_event_count);
    ESP_LOGI(TAG, "framebuffers=%p/%p bytes=%u", framebuffers[0], framebuffers[1], (unsigned)FRAME_BYTES);
    log_display_dma_state("before submission");

    // Clockwise-rotated landscape bars, left to right: R, G, B, white, black.
    const uint32_t colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x000000};
    for (int y = 0; y < CLIP_HEIGHT; y++) {
        for (int x = 0; x < CLIP_WIDTH; x++) {
            uint32_t color = colors[y * 5 / CLIP_HEIGHT];
            uint8_t *pixel = &framebuffers[next_fb][(y * CLIP_WIDTH + x) * 3];
            pixel[0] = color & 0xff;
            pixel[1] = (color >> 8) & 0xff;
            pixel[2] = (color >> 16) & 0xff;
        }
    }
    present_frame();
    ESP_LOGI(TAG, "TEST 2: CPU framebuffer, landscape RED/GREEN/BLUE/WHITE/BLACK; frame=%u vsync=%u",
             frame_event_count, vsync_event_count);
    wait_for_key('n');

    show_jpeg(diagnostic_start, diagnostic_end - diagnostic_start);
    ESP_LOGI(TAG, "TEST 3: JPEG chart; check TOP/BOTTOM/LEFT/RIGHT and color labels");
    wait_for_key('p');
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "WX101BH020I-40Z native 800x1280, landscape 1280x800, %ds/%dfps", CLIP_SECONDS, CLIP_FPS);
    ESP_LOGI(TAG, "PSRAM=%u bytes, AVI=%u bytes", (unsigned)esp_psram_get_size(),
             (unsigned)CLIP_BYTES);
    const esp_partition_t *video = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, "video");
    if (video == NULL || CLIP_BYTES > video->size) {
        ESP_LOGE(TAG, "Video partition missing or too small; flash the complete project");
        return;
    }
    uint8_t header[12];
    ESP_ERROR_CHECK(esp_partition_read(video, 0, header, sizeof(header)));
    uint32_t riff_size = (uint32_t)header[4] | ((uint32_t)header[5] << 8) |
                         ((uint32_t)header[6] << 16) | ((uint32_t)header[7] << 24);
    if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "AVI ", 4) ||
        (uint64_t)riff_size + 8 != CLIP_BYTES) {
        ESP_LOGE(TAG, "Video data missing or mismatched; flash the complete project");
        return;
    }
    const void *clip_data;
    esp_partition_mmap_handle_t video_mapping;
    ESP_ERROR_CHECK(esp_partition_mmap(video, 0, CLIP_BYTES, ESP_PARTITION_MMAP_DATA,
                                      &clip_data, &video_mapping));
    // Keep this mapping alive throughout the playback loop; no whole-video PSRAM copy.
    ESP_LOGI(TAG, "VIDEO PARTITION: offset=0x%lx bytes=%u mapped=%p",
             (unsigned long)video->address, (unsigned)CLIP_BYTES, clip_data);
    frame_complete = xSemaphoreCreateBinary();
    play_complete = xSemaphoreCreateBinary();
    assert(frame_complete && play_complete);

    esp_ldo_channel_handle_t phy_power;
    esp_ldo_channel_config_t power = {.chan_id = 3, .voltage_mv = 2500};
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&power, &phy_power));
    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_config = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &bus));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t io_config = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &io_config, &io));

    esp_lcd_dpi_panel_config_t dpi = JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB888);
    // PLL240 / 4 is exactly 60MHz. Requesting 68MHz makes IDF 5.5.5
    // compensate HFP from 40 to -64, leaving HTOTAL=776 smaller than HDISP=800.
    dpi.dpi_clock_freq_mhz = 60;
    dpi.in_color_format = LCD_COLOR_FMT_RGB888;
    // Match the framebuffer and wire formats to isolate bridge conversion.
    dpi.out_color_format = LCD_COLOR_FMT_RGB888;
    dpi.num_fbs = 2;
    dpi.flags.use_dma2d = false;
    jd9365_vendor_config_t vendor = {.mipi_config = {.dsi_bus = bus, .dpi_config = &dpi}};
    esp_lcd_panel_dev_config_t config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9365(io, &config, &panel));
    esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_frame_buf_complete = on_frame_complete,
        .on_vsync = on_vsync,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, NULL));
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 2,
                                                    (void **)&framebuffers[0], (void **)&framebuffers[1]));
    ESP_LOGI(TAG, "Sending LCD software reset and supplier initialization");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    jpeg_decode_engine_cfg_t engine_config = {.timeout_ms = 1000};
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&engine_config, &decoder));
    jpeg_decode_memory_alloc_cfg_t in_config = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_input = jpeg_alloc_decoder_mem(CLIP_BUFFER_SIZE, &in_config, &input_capacity);
    assert(jpeg_input);

#if CONFIG_MVP_DIAGNOSTIC_BOOT
    run_diagnostics();
#else
    ESP_LOGI(TAG, "AUTOPLAY: starting video partition without serial confirmation");
#endif

    avi_player_handle_t player;
    avi_player_config_t player_config = {
        .buffer_size = CLIP_BUFFER_SIZE,
        .video_cb = on_video_frame,
        .avi_play_end_cb = on_play_complete,
        .priority = 5,
        .coreID = 0,
        .stack_size = 6144,
        .stack_in_psram = false,
    };
    ESP_ERROR_CHECK(avi_player_init(player_config, &player));
    for (unsigned loop = 1; ; loop++) {
        displayed_frames = 0;
        decode_total_us = present_total_us = 0;
        submit_total_us = wait_total_us = 0;
        int64_t start = esp_timer_get_time();
        ESP_ERROR_CHECK(avi_player_play_from_memory(player, (uint8_t *)clip_data, CLIP_BYTES));
        if (xSemaphoreTake(play_complete, pdMS_TO_TICKS((uint64_t)CLIP_SECONDS * 5000 + 10000)) != pdTRUE) {
            ESP_LOGE(TAG, "Playback timeout");
            abort();
        }
        int64_t elapsed = esp_timer_get_time() - start;
        ESP_LOGI(TAG, "loop=%u frames=%u/%u elapsed=%.3fs submitted_fps=%.2f decode=%.1fms present=%.1fms submit=%.1fms wait=%.1fms free_heap=%u",
                 loop, displayed_frames, CLIP_FRAMES, elapsed / 1000000.0,
                 displayed_frames * 1000000.0 / elapsed, decode_total_us / 1000.0 / CLIP_FRAMES,
                 present_total_us / 1000.0 / CLIP_FRAMES,
                 submit_total_us / 1000.0 / CLIP_FRAMES,
                 wait_total_us / 1000.0 / CLIP_FRAMES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        assert(displayed_frames == CLIP_FRAMES);
    }
}
