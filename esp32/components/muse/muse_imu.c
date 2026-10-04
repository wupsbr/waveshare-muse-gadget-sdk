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

#include "muse_imu.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "muse_imu";

/* Registers and values from QST's QMI8658A datasheet (Rev A), as SensorLib
 * and Waveshare's 92_qmi8658_imu example use them. */
#define ADDR_A 0x6B
#define ADDR_B 0x6A
#define REG_WHO_AM_I 0x00
#define WHO_AM_I 0x05
#define REG_CTRL1 0x02
#define CTRL1_VALUE 0x60        /* address auto-increment, as Waveshare's qmi8658 component sets it */
#define REG_CTRL2 0x03
#define CTRL2_8G_1KHZ 0x23      /* aFS 010 = +-8 g, aODR 0011 = 1 kHz */
#define REG_CTRL3 0x04
#define CTRL3_512DPS_1KHZ 0x43  /* gFS 100 = 512 dps, gODR 0011 = 1 kHz */
#define REG_CTRL7 0x08
#define CTRL7_AEN_GEN 0x03      /* accelerometer and gyroscope on */
#define REG_AX_L 0x35
#define REG_RESET 0x60
#define RESET_CMD 0xB0
#define RESET_MS 20             /* the datasheet's 15 ms, and some */
#define G_PER_LSB (8.0f / 32768.0f)

#define GRAVITY_ALPHA 0.06f     /* per sample: follows Muse being turned over in about 0.3 s */
#define SWING_END_G (MUSE_IMU_SWING_G * 0.5f)

static i2c_master_dev_handle_t s_dev;

/* The shake detector. */
static float s_gravity[3];
static bool s_settled;          /* s_gravity holds a reading */
static bool s_in_swing;
static float s_peak[3];         /* the swing so far: its strongest sample */
static float s_peak_g;
static float s_last[3];         /* the previous counted swing */
static int64_t s_swing_ms[MUSE_IMU_SHAKE_SWINGS];   /* when each counted swing ended, newest last */
static int s_swings;
static float s_shake_g;         /* strongest swing of the shake, for the log */
static int64_t s_hold_until_ms;
static bool s_read_failed;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t configure(void)
{
    esp_err_t err = reg_write(REG_RESET, RESET_CMD);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(RESET_MS));
    /*
     * Waveshare's qmi8658 component's sequence. With the accelerometer on its
     * own (CTRL7 = 0x01) the AMOLED 1.8's QMI8658 takes the settings but never
     * produces a sample (STATUS0 stays 0, data reads 0), so the gyroscope runs
     * too; it costs well under a milliamp. Only the accelerometer is read.
     */
    if ((err = reg_write(REG_CTRL7, 0)) != ESP_OK || (err = reg_write(REG_CTRL1, CTRL1_VALUE)) != ESP_OK
        || (err = reg_write(REG_CTRL2, CTRL2_8G_1KHZ)) != ESP_OK
        || (err = reg_write(REG_CTRL3, CTRL3_512DPS_1KHZ)) != ESP_OK) {
        return err;
    }
    return reg_write(REG_CTRL7, CTRL7_AEN_GEN);
}

esp_err_t muse_imu_init(i2c_master_bus_handle_t bus)
{
    const uint8_t addrs[] = { ADDR_A, ADDR_B };
    for (size_t i = 0; i < sizeof(addrs) && !s_dev; i++) {
        if (i2c_master_probe(bus, addrs[i], 50) != ESP_OK) {
            continue;
        }
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
            s_dev = NULL;
            continue;
        }
        uint8_t id = 0;
        if (reg_read(REG_WHO_AM_I, &id, 1) != ESP_OK || id != WHO_AM_I) {
            ESP_LOGI(TAG, "0x%02x answers but isn't a QMI8658 (WHO_AM_I 0x%02x)", addrs[i], id);
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            continue;
        }
        esp_err_t err = configure();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "QMI8658 at 0x%02x won't configure (%s): no shake reaction", addrs[i], esp_err_to_name(err));
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            return err;
        }
        ESP_LOGI(TAG, "QMI8658 at 0x%02x: accelerometer +-8 g, 1 kHz", addrs[i]);
    }
    if (!s_dev) {
        ESP_LOGI(TAG, "no QMI8658 IMU found: no shake reaction");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool muse_imu_present(void)
{
    return s_dev != NULL;
}

static bool read_accel(float a[3])
{
    uint8_t b[6];
    esp_err_t err = reg_read(REG_AX_L, b, sizeof(b));
    if (err != ESP_OK) {
        if (!s_read_failed) {
            ESP_LOGW(TAG, "read failed (%s)", esp_err_to_name(err));
            s_read_failed = true;
        }
        return false;
    }
    s_read_failed = false;
    for (int i = 0; i < 3; i++) {
        a[i] = (float)(int16_t)(b[2 * i] | b[2 * i + 1] << 8) * G_PER_LSB;
    }
    return true;
}

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* A swing ended. Counts it if it went the other way from the last one;
 * one the same way (a second tap, a bump) starts the count over. True once
 * there are enough of them close enough together. */
static bool count_swing(int64_t now_ms)
{
    if (s_swings > 0 && dot(s_peak, s_last) >= 0) {
        s_swings = 0;
    }
    if (s_swings == 0) {
        s_shake_g = 0;
    }
    if (s_swings == MUSE_IMU_SHAKE_SWINGS) {
        memmove(s_swing_ms, s_swing_ms + 1, sizeof(s_swing_ms) - sizeof(s_swing_ms[0]));
        s_swings--;
    }
    s_swing_ms[s_swings++] = now_ms;
    memcpy(s_last, s_peak, sizeof(s_last));
    if (s_peak_g > s_shake_g) {
        s_shake_g = s_peak_g;
    }
    return s_swings == MUSE_IMU_SHAKE_SWINGS && now_ms - s_swing_ms[0] <= MUSE_IMU_SHAKE_MS;
}

bool muse_imu_poll_shake(void)
{
    float a[3];
    if (!s_dev || !read_accel(a)) {
        return false;
    }
    if (!s_settled) {
        memcpy(s_gravity, a, sizeof(s_gravity));
        s_settled = true;
        return false;
    }
    /* What's left once gravity is taken out is Muse being moved. */
    float d[3];
    for (int i = 0; i < 3; i++) {
        d[i] = a[i] - s_gravity[i];
        s_gravity[i] += GRAVITY_ALPHA * d[i];
    }
    float g = sqrtf(dot(d, d));
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < s_hold_until_ms) {
        s_in_swing = false;
        s_swings = 0;
        return false;
    }

    bool shaken = false;
    if (s_in_swing && g > MUSE_IMU_SWING_G && dot(d, s_peak) < 0) {
        /* Straight from one way to the other between two samples. */
        shaken = count_swing(now_ms);
        s_in_swing = false;
    }
    if (!s_in_swing) {
        if (!shaken && g > MUSE_IMU_SWING_G) {
            s_in_swing = true;
            memcpy(s_peak, d, sizeof(s_peak));
            s_peak_g = g;
        }
    } else if (g > s_peak_g) {
        memcpy(s_peak, d, sizeof(s_peak));
        s_peak_g = g;
    } else if (g < SWING_END_G) {
        s_in_swing = false;
        shaken = count_swing(now_ms);
    }
    if (shaken) {
        ESP_LOGI(TAG, "shake: %d swings in %d ms, up to %.1f g", s_swings, (int)(now_ms - s_swing_ms[0]),
                 (double)s_shake_g);
        s_swings = 0;
        s_in_swing = false;
        s_hold_until_ms = now_ms + MUSE_IMU_HOLDOFF_MS;
    }
    return shaken;
}
