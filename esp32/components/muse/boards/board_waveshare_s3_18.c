/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.8: ESP32-S3 (16 MB flash, 8 MB octal
 * PSRAM), 368x448 AMOLED on QSPI with touch, one ES8311 for speaker (NS4150B
 * amp on GPIO46) and mic, AXP2101 PMU, TCA9554 expander. BOOT (GPIO0) talks;
 * PWR reaches the ESP32 only through the AXP2101's key latch, and held long it
 * makes the PMU cut power. Both are on the right edge, as on the C6 version.
 *
 * One I2C bus (GPIO 15/14) carries touch, ES8311 (0x18), the expander (0x20)
 * and the AXP2101 (0x34). The expander's P0 resets the panel, P1 powers it
 * and P2 resets touch; P7 is the SD card's CS.
 *
 * Two revisions, told apart by the touch controller as Waveshare's own
 * board_variant.c does: the original has an SH8601 panel with FT3168 touch
 * (0x38), V2 a CO5300 with CST816 touch (0x15), drawn 16 columns in. Pins,
 * init sequences and the expander lines are from Waveshare's BSP
 * (waveshare/esp32_s3_touch_amoled_1_8 1.1.4 for the original, 2.0.3 for V2),
 * its examples (ESP32-S3-Touch-AMOLED-1.8, 90_axp2101_pmu) and xiaozhi-esp32's
 * esp32-s3-touch-amoled-1.8 boards. The BSP isn't used: each version drives
 * only one panel, and both bring esp_lvgl_port.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_imu.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define LCD_H_RES 368
#define LCD_V_RES 448
#define LCD_GAP_X_V2 16            /* V2's CO5300 starts 16 columns in */
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_12
#define LCD_SCLK GPIO_NUM_11
#define LCD_D0 GPIO_NUM_4
#define LCD_D1 GPIO_NUM_5
#define LCD_D2 GPIO_NUM_6
#define LCD_D3 GPIO_NUM_7
#define DRAW_BUF_LINES 112         /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_H_RES * 8 * 2)

#define I2C_SDA GPIO_NUM_15
#define I2C_SCL GPIO_NUM_14
#define I2S_MCLK GPIO_NUM_16
#define I2S_BCLK GPIO_NUM_9
#define I2S_WS GPIO_NUM_45
#define I2S_DOUT GPIO_NUM_8
#define I2S_DIN GPIO_NUM_10
#define SPK_PA GPIO_NUM_46
#define TALK_GPIO GPIO_NUM_0       /* BOOT */
#define PMU_KEY_EVERY 2            /* poll the PMU over I2C every 20 ms */

#define TP_ADDR_FT3168 0x38        /* original */
#define TP_ADDR_CST816 0x15        /* V2 */
#define TP_REG_POINTS 0x02         /* finger count, then X and Y, 12 bits each, on both */
#define TP_EVENT_LIFT 0x01         /* event (top 2 bits of XH): finger lifted */
#define TP_REG_NO_AUTO_SLEEP 0xFE  /* CST816: else it stops answering I2C when idle */

#define IOE_ADDR 0x20
#define IOE_OUT 0x01
#define IOE_CONFIG 0x03            /* 1 = input */
#define IOE_LCD_RST BIT(0)
#define IOE_LCD_PWR BIT(1)
#define IOE_TP_RST BIT(2)
#define IOE_SD_CS BIT(7)           /* held high: no card selected */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_ioe, s_tp;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;
static bool s_v2;

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit_receive(dev, &reg, 1, buf, n, 50);
    }
    return err;
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit(dev, buf, sizeof(buf), 50);
    }
    return err;
}

static esp_err_t add_device(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_i2c, &cfg, dev);
}

/*
 * Panel off with both resets held, then power on, then out of reset: the
 * panel keeps its power across a software restart otherwise, and would come
 * up in whatever state it was left.
 */
static esp_err_t panel_power_up(void)
{
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_OUT, IOE_SD_CS), TAG, "expander levels");
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_CONFIG, (uint8_t)~(IOE_LCD_RST | IOE_LCD_PWR | IOE_TP_RST | IOE_SD_CS)),
                        TAG, "expander outputs");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_OUT, IOE_SD_CS | IOE_LCD_PWR), TAG, "panel power");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_OUT, IOE_SD_CS | IOE_LCD_PWR | IOE_LCD_RST | IOE_TP_RST), TAG,
                        "panel reset");
    vTaskDelay(pdMS_TO_TICKS(150));    /* as Waveshare's board_variant.c waits */
    return ESP_OK;
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(add_device(IOE_ADDR, &s_ioe), TAG, "expander");
    ESP_RETURN_ON_ERROR(panel_power_up(), TAG, "panel power");

    /* Touch answers once out of reset, and says which revision this is. */
    s_v2 = i2c_master_probe(s_i2c, TP_ADDR_CST816, 100) == ESP_OK;
    if (!s_v2 && i2c_master_probe(s_i2c, TP_ADDR_FT3168, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no touch controller found; assuming the original SH8601 panel");
    }
    ESP_RETURN_ON_ERROR(add_device(s_v2 ? TP_ADDR_CST816 : TP_ADDR_FT3168, &s_tp), TAG, "touch");
    ESP_LOGI(TAG, "%s", s_v2 ? "V2: CO5300 panel, CST816 touch" : "original: SH8601 panel, FT3168 touch");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");

    /* Only PWR's PMU latch sees it: latch its edges for poll_buttons(). */
    esp_err_t err = muse_pmu_init(s_i2c, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): PWR button and battery unavailable", esp_err_to_name(err));
    } else {
        /* DCDC1 (3V3) and ALDO1 (the mic) are all the board uses; xiaozhi's
         * board turns the rest off too. */
        err = muse_pmu_keep_rails(BIT(0), BIT(0));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "unused rails left on (%s)", esp_err_to_name(err));
        }
    }

    /* Waveshare's docs and 92_qmi8658_imu example put a QMI8658 on this bus. */
    muse_imu_init(s_i2c);
    return ESP_OK;
}

/* The original's sequence (BSP 1.1.4, xiaozhi). COLMOD and MADCTL come from
 * the driver first. Brightness starts dark until the UI sets its own. */
static const sh8601_lcd_init_cmd_t s_sh8601_init[] = {
    { 0x11, NULL, 0, 120 },                                 /* sleep out */
    { 0x44, (uint8_t[]){ 0x01, 0xD1 }, 2, 0 },              /* tearing scanline */
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },                    /* tearing line on */
    { 0x53, (uint8_t[]){ 0x20 }, 1, 10 },                   /* brightness control on */
    { 0x2A, (uint8_t[]){ 0x00, 0x00, 0x01, 0x6F }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xBF }, 4, 0 },
    { 0x51, (uint8_t[]){ 0x00 }, 1, 10 },                   /* brightness */
    { 0x29, NULL, 0, 10 },                                  /* display on */
};

/* V2's sequence (BSP 2.0.3), but dark until the UI sets a brightness. */
static const co5300_lcd_init_cmd_t s_co5300_init[] = {
    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },                    /* RGB565 */
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },                    /* tearing line on */
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },                    /* brightness control on */
    { 0x51, (uint8_t[]){ 0x00 }, 1, 0 },                    /* brightness */
    { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x2A, (uint8_t[]){ 0x00, 0x00, 0x01, 0x6F }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xBF }, 4, 0 },
    { 0x11, NULL, 0, 100 },                                 /* sleep out */
    { 0x29, NULL, 0, 0 },                                   /* display on */
};

static esp_err_t panel_new(void)
{
    const esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io), TAG, "panel io");
    if (s_v2) {
        co5300_vendor_config_t vendor_cfg = {
            .init_cmds = s_co5300_init,
            .init_cmds_size = sizeof(s_co5300_init) / sizeof(s_co5300_init[0]),
            .flags.use_qspi_interface = 1,
        };
        const esp_lcd_panel_dev_config_t panel_cfg = {
            .reset_gpio_num = GPIO_NUM_NC,      /* the expander's P0, pulsed in init() */
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_cfg,
        };
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel), TAG, "co5300");
    } else {
        sh8601_vendor_config_t vendor_cfg = {
            .init_cmds = s_sh8601_init,
            .init_cmds_size = sizeof(s_sh8601_init) / sizeof(s_sh8601_init[0]),
            .flags.use_qspi_interface = 1,
        };
        const esp_lcd_panel_dev_config_t panel_cfg = {
            .reset_gpio_num = GPIO_NUM_NC,      /* the expander's P0, pulsed in init() */
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_cfg,
        };
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG, "sh8601");
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, s_v2 ? LCD_GAP_X_V2 : 0, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return ESP_OK;
}

/* Both panels need even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* LVGL polls this from its own task. Both controllers report one point the
 * FocalTech way. */
static void tp_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t b[5];
    if (reg_read(s_tp, TP_REG_POINTS, b, sizeof(b)) != ESP_OK || (b[0] & 0x0F) == 0
        || (b[1] >> 6) == TP_EVENT_LIFT) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int x = (b[1] & 0x0F) << 8 | b[2];
    int y = (b[3] & 0x0F) << 8 | b[4];
    data->point.x = x < LCD_H_RES ? x : LCD_H_RES - 1;
    data->point.y = y < LCD_V_RES ? y : LCD_V_RES - 1;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* As on the 1.75C, the bands go out through fixed internal buffers. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus =
        SH8601_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK || panel_new() != ESP_OK) {
        return NULL;
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    if (s_v2 && reg_write(s_tp, TP_REG_NO_AUTO_SLEEP, 0x01) != ESP_OK) {
        ESP_LOGW(TAG, "touch not answering");
    }
    *touch = lv_indev_create();
    if (!*touch) {
        return NULL;
    }
    lv_indev_set_type(*touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(*touch, disp);
    lv_indev_set_read_cb(*touch, tp_read);
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_brightness(void *level)
{
    /* "Write display brightness" (0x51), the same on both panels. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), level, 1);
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

/* Plain SLPIN/SLPOUT over the QSPI command path, as on the 1.75C. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* The one ES8311 drives the speaker and takes the mic, over a duplex I2S bus. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(data_if && gpio_if && ctrl_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = SPK_PA,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    unsigned ev = muse_gpio_button_poll(&s_talk);   /* BOOT talks, PWR is aux */
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        ev |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
              (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    }
    return ev;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.8",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 1.8f,
    .talk_button = "boot",
    .aux_button = "pwr",
    /* Both buttons are on the right edge, about 100 px from the top and
     * bottom, as on the C6 version. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -10, -124 },
    .aux_hint = { LV_ALIGN_RIGHT_MID, -10, 126 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
