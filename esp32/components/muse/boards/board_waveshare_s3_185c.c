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
 * Waveshare ESP32-S3-Touch-LCD-1.85C: ESP32-S3 (16 MB flash, 8 MB octal PSRAM),
 * round 1.85" 360 px ST77916 LCD on QSPI with a PWM backlight, CST816 touch,
 * ES8311 speaker codec with an amp, ES7210 mic ADC, PCF85063 RTC and a
 * TCA9554 IO expander. BOOT (GPIO0) is the only button software can read: RST
 * resets the chip, and the slide switch cuts the battery. There is no PMU;
 * the battery reaches GPIO8 through a 200k/100k divider, and the ETA6098
 * charger's STAT pin only lights its LED.
 *
 * One I2C bus (GPIO 11/10) carries touch (0x15), ES8311 (0x18), the expander
 * (0x20), ES7210 (0x40) and the RTC (0x51). The expander's EXIO1 resets touch
 * and EXIO2 the panel.
 *
 * Pins and the panel's init sequence are from Waveshare's examples for this
 * board (ESP32-S3-Touch-LCD-1.85C, Arduino 01_lvgl_example and ESP-IDF test)
 * and its V2 schematic. The panel comes in two revisions: register 0x04 reads
 * 00 02 7f 7f on the one that needs the vendor sequence below.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_console.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_RES 360
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_21
#define LCD_SCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_BL GPIO_NUM_5
#define LCD_PROBE_HZ (5 * 1000 * 1000)    /* register reads need a slow clock */
#define DRAW_BUF_LINES 90          /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47
#define I2S_DIN GPIO_NUM_39
#define SPK_PA GPIO_NUM_15

#define TALK_GPIO GPIO_NUM_0       /* BOOT */

#define BATT_ADC ADC_CHANNEL_7     /* GPIO8, through a 200k/100k divider */
#define BATT_FULL_MV 4080          /* the charger holds a full battery at 4.1-4.2 V */
#define BATT_RISE_MV 60            /* above the lowest since it last fell: charging */
#define BATT_DROP_MV 40            /* below the highest since it last rose: on battery */

#define TP_ADDR 0x15
#define TP_REG_POINTS 0x02         /* finger count, then X and Y, 12 bits each */
#define TP_REG_SLEEP 0xE5          /* 0x03: deep sleep until reset */
#define TP_REG_NO_AUTO_SLEEP 0xFE  /* else it stops answering I2C when idle */

#define IOE_ADDR 0x20
#define IOE_OUT 0x01
#define IOE_CONFIG 0x03            /* 1 = input */
#define IOE_TP_RST BIT(0)          /* EXIO1 */
#define IOE_LCD_RST BIT(1)         /* EXIO2 */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_ioe, s_tp;
static uint8_t s_ioe_out = 0xFF;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static enum { BATT_UNKNOWN, BATT_RISING, BATT_FALLING } s_batt_trend;
static int s_batt_edge_mv;         /* highest while not falling, lowest while falling */

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

/* Only init() and the input task touch the expander, never at once. */
static esp_err_t ioe_set(uint8_t pins, bool high)
{
    s_ioe_out = high ? s_ioe_out | pins : s_ioe_out & ~pins;
    return reg_write(s_ioe, IOE_OUT, s_ioe_out);
}

static esp_err_t add_device(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(s_i2c, &cfg, dev);
}

/* Reset pulse on the touch controller, then keep it answering while idle. */
static void tp_reset(void)
{
    ioe_set(IOE_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    ioe_set(IOE_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (reg_write(s_tp, TP_REG_NO_AUTO_SLEEP, 0x01) != ESP_OK) {
        ESP_LOGW(TAG, "touch not answering");
    }
}

static void backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    const ledc_channel_config_t ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,      /* dark until the UI sets a level */
    };
    ledc_channel_config(&ch);
}

/* The battery's divider on ADC1, as Waveshare's ESP-IDF test reads it. Without
 * calibration the battery isn't shown, and the board runs on. */
static void batt_init(void)
{
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
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(add_device(IOE_ADDR, 400000, &s_ioe), TAG, "ioe");
    ESP_RETURN_ON_ERROR(add_device(TP_ADDR, 400000, &s_tp), TAG, "touch");

    /* Both resets driven high, then pulse the panel's; the touch one is
     * pulsed once LVGL is about to read it. The rest of the expander (EXIO3
     * is the SD card's D3) stays an input. */
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_OUT, s_ioe_out), TAG, "ioe levels");
    ESP_RETURN_ON_ERROR(reg_write(s_ioe, IOE_CONFIG, (uint8_t)~(IOE_TP_RST | IOE_LCD_RST)), TAG, "ioe outputs");
    ESP_RETURN_ON_ERROR(ioe_set(IOE_LCD_RST, false), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ioe_set(IOE_LCD_RST, true), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(50));

    backlight_init();

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    /* A press that woke the board from deep sleep is still down; don't count it. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;

    batt_init();
    return ESP_OK;
}

/* Waveshare's sequence for the panel revision that reads 00 02 7f 7f. */
static const st77916_lcd_init_cmd_t s_lcd_init_new[] = {
    { 0xF0, (uint8_t[]){ 0x28 }, 1, 0 },
    { 0xF2, (uint8_t[]){ 0x28 }, 1, 0 },
    { 0x73, (uint8_t[]){ 0xF0 }, 1, 0 },
    { 0x7C, (uint8_t[]){ 0xD1 }, 1, 0 },
    { 0x83, (uint8_t[]){ 0xE0 }, 1, 0 },
    { 0x84, (uint8_t[]){ 0x61 }, 1, 0 },
    { 0xF2, (uint8_t[]){ 0x82 }, 1, 0 },
    { 0xF0, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xF0, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xF1, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xB0, (uint8_t[]){ 0x56 }, 1, 0 },
    { 0xB1, (uint8_t[]){ 0x4D }, 1, 0 },
    { 0xB2, (uint8_t[]){ 0x24 }, 1, 0 },
    { 0xB4, (uint8_t[]){ 0x87 }, 1, 0 },
    { 0xB5, (uint8_t[]){ 0x44 }, 1, 0 },
    { 0xB6, (uint8_t[]){ 0x8B }, 1, 0 },
    { 0xB7, (uint8_t[]){ 0x40 }, 1, 0 },
    { 0xB8, (uint8_t[]){ 0x86 }, 1, 0 },
    { 0xBA, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xBB, (uint8_t[]){ 0x08 }, 1, 0 },
    { 0xBC, (uint8_t[]){ 0x08 }, 1, 0 },
    { 0xBD, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC0, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0xC1, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0xC2, (uint8_t[]){ 0x37 }, 1, 0 },
    { 0xC3, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0xC5, (uint8_t[]){ 0x37 }, 1, 0 },
    { 0xC6, (uint8_t[]){ 0xA9 }, 1, 0 },
    { 0xC7, (uint8_t[]){ 0x41 }, 1, 0 },
    { 0xC8, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xC9, (uint8_t[]){ 0xA9 }, 1, 0 },
    { 0xCA, (uint8_t[]){ 0x41 }, 1, 0 },
    { 0xCB, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xD0, (uint8_t[]){ 0x91 }, 1, 0 },
    { 0xD1, (uint8_t[]){ 0x68 }, 1, 0 },
    { 0xD2, (uint8_t[]){ 0x68 }, 1, 0 },
    { 0xF5, (uint8_t[]){ 0x00, 0xA5 }, 2, 0 },
    { 0xDD, (uint8_t[]){ 0x4F }, 1, 0 },
    { 0xDE, (uint8_t[]){ 0x4F }, 1, 0 },
    { 0xF1, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0xF0, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xF0, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0xE0, (uint8_t[]){ 0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34 }, 14, 0 },
    { 0xE1, (uint8_t[]){ 0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33 }, 14, 0 },
    { 0xF0, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0xF3, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0xE0, (uint8_t[]){ 0x07 }, 1, 0 },
    { 0xE1, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xE2, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xE3, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xE4, (uint8_t[]){ 0xE0 }, 1, 0 },
    { 0xE5, (uint8_t[]){ 0x06 }, 1, 0 },
    { 0xE6, (uint8_t[]){ 0x21 }, 1, 0 },
    { 0xE7, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xE8, (uint8_t[]){ 0x05 }, 1, 0 },
    { 0xE9, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0xEA, (uint8_t[]){ 0xDA }, 1, 0 },
    { 0xEB, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xEC, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xED, (uint8_t[]){ 0x0F }, 1, 0 },
    { 0xEE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xEF, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xF8, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xF9, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xFA, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xFB, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xFC, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xFD, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xFF, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x60, (uint8_t[]){ 0x40 }, 1, 0 },
    { 0x61, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0x62, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x63, (uint8_t[]){ 0x42 }, 1, 0 },
    { 0x64, (uint8_t[]){ 0xD9 }, 1, 0 },
    { 0x65, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x66, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x67, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x68, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x69, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x6A, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x6B, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x70, (uint8_t[]){ 0x40 }, 1, 0 },
    { 0x71, (uint8_t[]){ 0x03 }, 1, 0 },
    { 0x72, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x73, (uint8_t[]){ 0x42 }, 1, 0 },
    { 0x74, (uint8_t[]){ 0xD8 }, 1, 0 },
    { 0x75, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x76, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x77, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x78, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x79, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x7A, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x7B, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x80, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0x81, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x82, (uint8_t[]){ 0x06 }, 1, 0 },
    { 0x83, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0x84, (uint8_t[]){ 0xD6 }, 1, 0 },
    { 0x85, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0x86, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x87, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x88, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0x89, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x8A, (uint8_t[]){ 0x08 }, 1, 0 },
    { 0x8B, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0x8C, (uint8_t[]){ 0xD8 }, 1, 0 },
    { 0x8D, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0x8E, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x8F, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x90, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0x91, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x92, (uint8_t[]){ 0x0A }, 1, 0 },
    { 0x93, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0x94, (uint8_t[]){ 0xDA }, 1, 0 },
    { 0x95, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0x96, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x97, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x98, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0x99, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x9A, (uint8_t[]){ 0x0C }, 1, 0 },
    { 0x9B, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0x9C, (uint8_t[]){ 0xDC }, 1, 0 },
    { 0x9D, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0x9E, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x9F, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xA0, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0xA1, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xA2, (uint8_t[]){ 0x05 }, 1, 0 },
    { 0xA3, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0xA4, (uint8_t[]){ 0xD5 }, 1, 0 },
    { 0xA5, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0xA6, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xA7, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xA8, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0xA9, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xAA, (uint8_t[]){ 0x07 }, 1, 0 },
    { 0xAB, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0xAC, (uint8_t[]){ 0xD7 }, 1, 0 },
    { 0xAD, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0xAE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xAF, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xB0, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0xB1, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xB2, (uint8_t[]){ 0x09 }, 1, 0 },
    { 0xB3, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0xB4, (uint8_t[]){ 0xD9 }, 1, 0 },
    { 0xB5, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0xB6, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xB7, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xB8, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0xB9, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xBA, (uint8_t[]){ 0x0B }, 1, 0 },
    { 0xBB, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0xBC, (uint8_t[]){ 0xDB }, 1, 0 },
    { 0xBD, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0xBE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xBF, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC0, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0xC1, (uint8_t[]){ 0x47 }, 1, 0 },
    { 0xC2, (uint8_t[]){ 0x56 }, 1, 0 },
    { 0xC3, (uint8_t[]){ 0x65 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x74 }, 1, 0 },
    { 0xC5, (uint8_t[]){ 0x88 }, 1, 0 },
    { 0xC6, (uint8_t[]){ 0x99 }, 1, 0 },
    { 0xC7, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xC8, (uint8_t[]){ 0xBB }, 1, 0 },
    { 0xC9, (uint8_t[]){ 0xAA }, 1, 0 },
    { 0xD0, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0xD1, (uint8_t[]){ 0x47 }, 1, 0 },
    { 0xD2, (uint8_t[]){ 0x56 }, 1, 0 },
    { 0xD3, (uint8_t[]){ 0x65 }, 1, 0 },
    { 0xD4, (uint8_t[]){ 0x74 }, 1, 0 },
    { 0xD5, (uint8_t[]){ 0x88 }, 1, 0 },
    { 0xD6, (uint8_t[]){ 0x99 }, 1, 0 },
    { 0xD7, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xD8, (uint8_t[]){ 0xBB }, 1, 0 },
    { 0xD9, (uint8_t[]){ 0xAA }, 1, 0 },
    { 0xF3, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xF0, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x21, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x11, (uint8_t[]){ 0x00 }, 1, 120 },
    { 0x29, (uint8_t[]){ 0x00 }, 1, 0 },
};

/* Reads RDDID (0x04) at a slow clock to tell the panel revisions apart, as
 * Waveshare's examples do. Returns true for the one that needs s_lcd_init_new. */
static bool panel_is_new_revision(void)
{
    esp_lcd_panel_io_spi_config_t cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    cfg.pclk_hz = LCD_PROBE_HZ;
    esp_lcd_panel_io_handle_t io;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &cfg, &io) != ESP_OK) {
        return true;
    }
    uint8_t id[4] = { 0 };
    esp_err_t err = esp_lcd_panel_io_rx_param(io, (0x0B << 24) | (0x04 << 8), id, sizeof(id));
    esp_lcd_panel_io_del(io);
    ESP_LOGI(TAG, "panel id %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
    /* The older panel reads 00 7f 7f 7f; anything unexpected gets the vendor
     * sequence, which this board shipped with. */
    return err != ESP_OK || !(id[1] == 0x7F && id[2] == 0x7F && id[3] == 0x7F);
}

/* LVGL polls this from its own task. The CST816 reports one point. */
static void tp_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t b[5];
    if (reg_read(s_tp, TP_REG_POINTS, b, sizeof(b)) != ESP_OK || (b[0] & 0x0F) == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int x = (b[1] & 0x0F) << 8 | b[2];
    int y = (b[3] & 0x0F) << 8 | b[4];
    data->point.x = x < LCD_RES ? x : LCD_RES - 1;
    data->point.y = y < LCD_RES ? y : LCD_RES - 1;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* As on the StopWatch, the bands go out through fixed internal buffers. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus =
        ST77916_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    st77916_vendor_config_t vendor_cfg = {
        .flags.use_qspi_interface = 1,
    };
    if (panel_is_new_revision()) {
        vendor_cfg.init_cmds = s_lcd_init_new;
        vendor_cfg.init_cmds_size = sizeof(s_lcd_init_new) / sizeof(s_lcd_init_new[0]);
    }
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = 80 * 1000 * 1000;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,      /* the expander's EXIO2, pulsed in init() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
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

/* The LCD has no brightness register; the backlight is PWM on GPIO5. */
static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
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
 * The battery's voltage, after the slide switch: off, it reads 0 V and Muse
 * shows no battery. Nothing tells the ESP32 about USB, so it's a computer
 * reading the port, a battery held near full, or a voltage that rose; a
 * falling one is on battery. Until it has fallen it counts as USB, since on
 * battery Muse dozes its Wi-Fi. A wall charger with the battery part charged
 * and the voltage not yet risen reads as battery, and charging only shows
 * once the voltage has risen.
 */
static esp_err_t read_power(muse_power_t *out)
{
    out->usb = true;
    out->charging = false;
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
    int v = sum / 8 * 3;
    out->battery_mv = v;
    if (v < 2500) {
        s_batt_trend = BATT_UNKNOWN;
        s_batt_edge_mv = 0;
        return ESP_OK;
    }
    if (!s_batt_edge_mv) {
        s_batt_edge_mv = v;
    } else if (s_batt_trend != BATT_FALLING) {
        s_batt_edge_mv = v > s_batt_edge_mv ? v : s_batt_edge_mv;
        if (v <= s_batt_edge_mv - BATT_DROP_MV) {
            s_batt_trend = BATT_FALLING;
            s_batt_edge_mv = v;
        }
    } else {
        s_batt_edge_mv = v < s_batt_edge_mv ? v : s_batt_edge_mv;
        if (v >= s_batt_edge_mv + BATT_RISE_MV) {
            s_batt_trend = BATT_RISING;
            s_batt_edge_mv = v;
        }
    }
    out->usb = s_batt_trend != BATT_FALLING || v >= BATT_FULL_MV || muse_console_host();
    out->charging = s_batt_trend == BATT_RISING;
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
 * No PMU to cut power: screen, touch and amp off, then deep sleep until BOOT
 * is pressed. The slide switch disconnects the battery for good.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(panel_off, NULL);
    reg_write(s_tp, TP_REG_SLEEP, 0x03);
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
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85C",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_CENTER, 0, 150 },
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
