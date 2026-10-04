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

#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * QMI8658 6-axis IMU, accelerometer only, for telling when Muse is shaken.
 * A board with one on its I2C bus calls muse_imu_init() from its init(); it
 * looks for the chip at 0x6B, then 0x6A, and without one the rest does nothing.
 * The i2c_master driver serializes each bus, so polling from muse_input's task
 * shares it safely with touch (LVGL's task) and the codecs.
 */

/* ESP_ERR_NOT_FOUND (logged once) when no QMI8658 answers. */
esp_err_t muse_imu_init(i2c_master_bus_handle_t bus);

bool muse_imu_present(void);

/*
 * Reads one sample and returns true once a vigorous shake has been seen:
 * MUSE_IMU_SHAKE_SWINGS back-and-forth swings of more than MUSE_IMU_SWING_G
 * (gravity taken out) within MUSE_IMU_SHAKE_MS. A tap, or picking Muse up, is
 * one swing in one direction. After a shake it holds off for
 * MUSE_IMU_HOLDOFF_MS. Call it every MUSE_IMU_POLL_MS or so.
 */
bool muse_imu_poll_shake(void);

#define MUSE_IMU_POLL_MS 20
#define MUSE_IMU_SWING_G 1.2f
#define MUSE_IMU_SHAKE_SWINGS 3
#define MUSE_IMU_SHAKE_MS 700
#define MUSE_IMU_HOLDOFF_MS 1500
