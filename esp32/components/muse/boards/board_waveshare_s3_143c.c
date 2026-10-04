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
 * Waveshare ESP32-S3-Touch-AMOLED-1.43C: ESP32-S3-PICO-1-N8R8 (8 MB flash,
 * 8 MB octal PSRAM in the package), round 1.43" 466 px SH8601 AMOLED on
 * QSPI, a touch controller at 0x15 with FocalTech-style registers, ES8311
 * speaker codec with an NS4150B amp, ES7210 mic ADC. BOOT (GPIO0) is the
 * only button software can read: PWR drives a power latch, not a GPIO.
 * There is no PMU or IO expander; the battery reaches GPIO4 through a
 * 200k/200k divider and the ETA6098 charger's STAT pin GPIO7.
 *
 * Pins, the panel's init sequence and the touch registers are from
 * Waveshare's examples for this board (ESP32-S3-Touch-AMOLED-1.43C,
 * arduino_v3.3.0 bsp_config.h and 05_LVGL_V8_Test) and its schematic. The
 * panel is mounted upside down: MADCTL turns it 180 degrees and touch
 * coordinates are flipped on both axes to match.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_console.h"
#include "muse_imu.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_RES 466
#define LCD_GAP_X 8                /* as Waveshare's examples and factory firmware set it */
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_15
#define LCD_SCLK GPIO_NUM_14
#define LCD_D0 GPIO_NUM_9
#define LCD_D1 GPIO_NUM_10
#define LCD_D2 GPIO_NUM_11
#define LCD_D3 GPIO_NUM_12
#define LCD_RST GPIO_NUM_13
#define DRAW_BUF_LINES 118         /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define I2S_MCLK GPIO_NUM_38
#define I2S_BCLK GPIO_NUM_39
#define I2S_WS GPIO_NUM_40
#define I2S_DOUT GPIO_NUM_41
#define I2S_DIN GPIO_NUM_42
#define SPK_PA GPIO_NUM_46
#define CODEC_EN GPIO_NUM_18       /* the audio LDO's enable; pulled up, driven high too */

#define TP_RST GPIO_NUM_16
#define TALK_GPIO GPIO_NUM_0       /* BOOT */

#define BATT_ADC ADC_CHANNEL_3     /* GPIO4, through a 200k/200k divider */
#define CHARGE_GPIO GPIO_NUM_7     /* the charger's STAT: low while charging, 100k pull-up */
#define BATT_FULL_MV 4080          /* the charger holds a full battery at 4.1-4.2 V */
#define CHARGE_HOLD_US (60 * 1000000LL)   /* STAT blinks with no battery */

#define TP_ADDR 0x15
#define TP_REG_POINTS 0x02         /* finger count, then X and Y, 12 bits each */
#define TP_EVENT_LIFT 0x01         /* event (top 2 bits of XH): finger lifted */
#define TP_REG_MODE 0xA5           /* 0x03: hibernate until reset */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tp;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static int64_t s_charged_us;       /* when STAT last read charging; 0 never */

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
    return i2c_master_transmit(dev, buf, sizeof(buf), 50);
}

/* Reset pulse on the touch controller, as Waveshare's example times it. */
static void tp_reset(void)
{
    gpio_set_level(TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* The battery's divider on ADC1 and the charger's STAT, as Waveshare's
 * 01_ADC_Test reads them. Without calibration the battery isn't shown, and
 * the board runs on. */
static void batt_init(void)
{
    const gpio_config_t stat = {
        .pin_bit_mask = BIT64(CHARGE_GPIO),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&stat);
    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_new_unit(&adc_cfg, &s_adc) != ESP_OK ||
        adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg) != ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no battery ADC: battery level disabled");
        s_cali = NULL;
    }
}

static esp_err_t init(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = BIT64(TP_RST) | BIT64(CODEC_EN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "gpio");
    gpio_set_level(CODEC_EN, 1);
    gpio_set_level(TP_RST, 1);

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t tp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &tp_cfg, &s_tp), TAG, "touch");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    /* A press that woke the board from deep sleep is still down; don't count it. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;

    batt_init();

    /* Not on Waveshare's list for this board; the probe costs nothing. */
    muse_imu_init(s_i2c);
    return ESP_OK;
}

/* Waveshare's sequence. The window is the panel's 466 columns from 6; the
 * driver adds LCD_GAP_X to each draw. Brightness starts dark until the UI
 * sets its own. */
static const sh8601_lcd_init_cmd_t s_lcd_init[] = {
    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },                    /* RGB565 */
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },                    /* tearing line on */
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },                    /* brightness control on */
    { 0x51, (uint8_t[]){ 0x00 }, 1, 0 },                    /* brightness */
    { 0x36, (uint8_t[]){ 0xC0 }, 1, 0 },                    /* MADCTL: turned 180 degrees */
    { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x2A, (uint8_t[]){ 0x00, 0x06, 0x01, 0xD7 }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xD1 }, 4, 0 },
    { 0x11, NULL, 0, 100 },                                 /* sleep out */
    { 0x29, NULL, 0, 0 },                                   /* display on */
};

/* The SH8601 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* LVGL polls this from its own task. */
static void tp_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t b[5];
    if (reg_read(s_tp, TP_REG_POINTS, b, sizeof(b)) != ESP_OK || (b[0] & 0x0F) == 0
        || (b[1] >> 6) == TP_EVENT_LIFT) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int x = LCD_RES - ((b[1] & 0x0F) << 8 | b[2]);
    int y = LCD_RES - ((b[3] & 0x0F) << 8 | b[4]);
    data->point.x = x < 0 ? 0 : x < LCD_RES ? x : LCD_RES - 1;
    data->point.y = y < 0 ? 0 : y < LCD_RES ? y : LCD_RES - 1;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* As on the StopWatch, the bands go out through fixed internal buffers. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus =
        SH8601_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);

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
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    tp_reset();
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
    /* SH8601 "write display brightness" (0x51). */
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

/* Plain SLPIN/SLPOUT over the QSPI command path, as on the StopWatch. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* ES8311 drives the speaker, ES7210 takes the mics, over one duplex I2S bus. */
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

    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    ESP_RETURN_ON_FALSE(data_if && gpio_if && dac_ctrl && adc_ctrl, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = SPK_PA,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");

    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

/*
 * The battery's voltage and the charger's STAT. Nothing senses USB itself, so
 * it's STAT charging (or within a minute of it), a computer reading the port,
 * or a battery the charger holds near full. On battery Muse dozes its Wi-Fi,
 * so a full battery counts as USB until it drops below 4.08 V. A wall charger
 * with a charged battery that has sagged below that, before the charger tops
 * it up, reads as battery.
 */
static esp_err_t read_power(muse_power_t *out)
{
    int64_t now = esp_timer_get_time();
    out->charging = gpio_get_level(CHARGE_GPIO) == 0;
    if (out->charging) {
        s_charged_us = now;
    }
    out->usb = out->charging || (s_charged_us && now - s_charged_us < CHARGE_HOLD_US) || muse_console_host();
    out->battery_pct = -1;
    out->battery_mv = 0;
    ESP_RETURN_ON_FALSE(s_cali, ESP_ERR_INVALID_STATE, TAG, "no ADC calibration");
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "adc cali");
        sum += mv;
    }
    int v = sum / 8 * 2;
    out->battery_mv = v;
    out->usb = out->usb || v >= BATT_FULL_MV;
    if (v < 2500) {
        return ESP_OK;
    }
    /* The Watcher's LiPo curve, as on the StickS3. */
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

static void panel_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

/*
 * The power latch is PWR's alone: screen, touch and amp off, then deep sleep
 * until BOOT is pressed.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    reg_write(s_tp, TP_REG_MODE, 0x03);
    gpio_set_level(SPK_PA, 0);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    rtc_gpio_pullup_en(TALK_GPIO);
    rtc_gpio_pulldown_dis(TALK_GPIO);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(BIT64(TALK_GPIO), ESP_EXT1_WAKEUP_ANY_LOW), TAG,
                        "button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.43C",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.43f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_CENTER, 0, 190 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
