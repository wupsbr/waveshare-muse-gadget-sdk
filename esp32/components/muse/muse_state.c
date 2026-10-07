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

#include "muse_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "muse_pixel.h"
#include "muse_text.h"

#define HAPPY_SECS 1.6f
#define DROWSE_WAKE_US 300000   /* drowsing this long: being interrupted plays the waking reaction */
#define TICKLE_LINGER_US 400000  /* tickling counts as going on this long after its last touch */
/* Where tickle's progress waits while the tickling goes on: inside the giggle,
 * a little before MUSE_TICKLE_HOLD, so it winds down from there once it stops. */
#define TICKLE_WAIT (MUSE_TICKLE_HOLD - 0.05f)

#define AWAKE_BIT BIT0   /* while !s_asleep */
#define NUDGE_BIT BIT1

static volatile muse_mode_t s_mode = MUSE_MODE_BOOT;
static volatile int64_t s_mode_since_us;
static volatile float s_level;
static volatile float s_progress;
static volatile int64_t s_last_poke_us;
static volatile int64_t s_happy_until_us;
static volatile bool s_asleep;
/* Reactions: when each started, 0 when not happening. Under s_lock: int64
 * isn't one store on this CPU. */
static int64_t s_dizzy_us;
static int64_t s_drowse_us;
static int64_t s_waking_us;
static int64_t s_tickle_us;
static int64_t s_tickle_held_us;   /* the last touch of the tickling */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_caption[MUSE_CAPTION_MAX];
static uint32_t s_caption_version;
static SemaphoreHandle_t s_format_lock;
static EventGroupHandle_t s_wake;
static volatile int s_page_cols = 16, s_page_lines = 2;
static volatile int s_cjk_cols, s_cjk_lines;
static muse_power_t s_power = { .battery_pct = -1 };
static volatile bool s_as_if_battery;

static float secs_since(int64_t us)
{
    return (float)(esp_timer_get_time() - us) / 1e6f;
}

/* Activity during drowsing: back awake, with the waking reaction if Muse was
 * visibly nodding off. */
static void end_drowse_locked(int64_t now, bool react)
{
    if (s_drowse_us) {
        if (react && now - s_drowse_us > DROWSE_WAKE_US) {
            s_waking_us = now;
        }
        s_drowse_us = 0;
    }
}

/* 0..1 through a reaction of `secs` that started at `since`; 0 once over. */
static float through(int64_t since, float secs, int64_t now)
{
    if (!since) {
        return 0;
    }
    float t = (float)(now - since) / 1e6f / secs;
    return t < 0 ? 0 : (t >= 1 ? 0 : t);
}

void muse_state_init(void)
{
    static StaticSemaphore_t lock;
    s_format_lock = xSemaphoreCreateMutexStatic(&lock);
    static StaticEventGroup_t wake;
    s_wake = xEventGroupCreateStatic(&wake);
    xEventGroupSetBits(s_wake, AWAKE_BIT);
    int64_t now = esp_timer_get_time();
    s_mode_since_us = now;
    s_last_poke_us = now;
}

void muse_state_set_mode(muse_mode_t mode)
{
    if (mode == s_mode) {
        return;
    }
    /* Shutting down wins over the voice pipeline finishing a turn. */
    if (s_mode == MUSE_MODE_OFF && mode != MUSE_MODE_IDLE) {
        return;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    end_drowse_locked(now, true);
    portEXIT_CRITICAL(&s_lock);
    s_mode_since_us = now;
    s_mode = mode;
}

muse_mode_t muse_state_mode(float *secs_in_mode)
{
    if (secs_in_mode) {
        *secs_in_mode = secs_since(s_mode_since_us);
    }
    return s_mode;
}

void muse_state_set_level(float level)
{
    s_level = level < 0 ? 0 : (level > 1 ? 1 : level);
}

float muse_state_level(void)
{
    return s_level;
}

void muse_state_set_progress(float progress)
{
    s_progress = progress < 0 ? 0 : (progress > 1 ? 1 : progress);
}

float muse_state_progress(void)
{
    return s_progress;
}

void muse_state_set_caption(const char *fmt, ...)
{
    static char buf[sizeof(s_caption)];   /* too big for some callers' stacks */
    xSemaphoreTake(s_format_lock, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    muse_text_to_ascii(buf, sizeof(buf));   /* replies have curly quotes and dashes */

    portENTER_CRITICAL(&s_lock);
    if (strcmp(buf, s_caption) != 0) {
        memcpy(s_caption, buf, sizeof(s_caption));
        s_caption_version++;
    }
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_format_lock);
}

bool muse_state_caption(char *out, size_t out_len, uint32_t *version)
{
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    if (*version != s_caption_version) {
        strlcpy(out, s_caption, out_len);
        *version = s_caption_version;
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return changed;
}

void muse_state_set_page(int cols, int lines)
{
    s_page_cols = cols;
    s_page_lines = lines;
}

void muse_state_set_cjk_page(int cols, int lines)
{
    s_cjk_cols = cols;
    s_cjk_lines = lines;
}

void muse_state_page(bool cjk, int *cols, int *lines)
{
    bool own = cjk && s_cjk_cols > 0;
    *cols = own ? s_cjk_cols : s_page_cols;
    *lines = own ? s_cjk_lines : s_page_lines;
}

void muse_state_set_power(const muse_power_t *power)
{
    bool was = muse_state_on_battery();
    portENTER_CRITICAL(&s_lock);
    s_power = *power;
    portEXIT_CRITICAL(&s_lock);
    if (muse_state_on_battery() != was) {
        muse_state_nudge();
    }
}

muse_power_t muse_state_power(void)
{
    portENTER_CRITICAL(&s_lock);
    muse_power_t p = s_power;
    portEXIT_CRITICAL(&s_lock);
    return p;
}

bool muse_state_on_battery(void)
{
    muse_power_t p = muse_state_power();
    return s_as_if_battery || (!p.usb && p.battery_pct >= 0);
}

void muse_state_set_as_if_battery(bool on)
{
    if (on != s_as_if_battery) {
        s_as_if_battery = on;
        muse_state_nudge();
    }
}

void muse_state_poke(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_last_poke_us = now;
    end_drowse_locked(now, true);
    portEXIT_CRITICAL(&s_lock);
}

float muse_state_idle_secs(void)
{
    return secs_since(s_last_poke_us);
}

void muse_state_set_asleep(bool asleep)
{
    if (!asleep) {
        muse_state_poke();
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (asleep) {
        s_drowse_us = s_dizzy_us = s_waking_us = s_tickle_us = 0;
    } else if (s_asleep) {
        s_waking_us = now;
    }
    s_asleep = asleep;
    portEXIT_CRITICAL(&s_lock);
    if (asleep) {
        xEventGroupClearBits(s_wake, AWAKE_BIT);
    } else {
        xEventGroupSetBits(s_wake, AWAKE_BIT);
    }
}

bool muse_state_asleep(void)
{
    return s_asleep;
}

void muse_state_wait_awake(uint32_t timeout_ms)
{
    xEventGroupWaitBits(s_wake, AWAKE_BIT | NUDGE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    xEventGroupClearBits(s_wake, NUDGE_BIT);
}

void muse_state_nudge(void)
{
    xEventGroupSetBits(s_wake, NUDGE_BIT);
}

void muse_state_make_happy(void)
{
    s_happy_until_us = esp_timer_get_time() + (int64_t)(HAPPY_SECS * 1e6f);
    muse_state_poke();
}

float muse_state_happiness(void)
{
    float left = (float)(s_happy_until_us - esp_timer_get_time()) / 1e6f;
    if (left <= 0) {
        return 0;
    }
    /* Ease out over the last 0.4 s. */
    return left > 0.4f ? 1.0f : left / 0.4f;
}

void muse_state_start_dizzy(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    end_drowse_locked(now, false);
    s_waking_us = s_tickle_us = 0;
    s_dizzy_us = now;
    s_last_poke_us = now;
    portEXIT_CRITICAL(&s_lock);
}

/* Dizzy and waking are idle's: leaving idle hides them, and they run out. */
static float idle_reaction(const int64_t *since, float secs)
{
    if (s_mode != MUSE_MODE_IDLE) {
        return 0;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    float t = through(*since, secs, now);
    portEXIT_CRITICAL(&s_lock);
    return t;
}

float muse_state_dizzy(void)
{
    return idle_reaction(&s_dizzy_us, MUSE_DIZZY_S);
}

float muse_state_waking(void)
{
    return idle_reaction(&s_waking_us, MUSE_WAKING_S);
}

bool muse_state_start_drowsing(float idle_s)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (!s_drowse_us && !s_asleep && now - s_last_poke_us > (int64_t)(idle_s * 1e6f)) {
        s_drowse_us = now;
    }
    bool drowsing = s_drowse_us != 0;
    portEXIT_CRITICAL(&s_lock);
    return drowsing;
}

void muse_state_end_drowsing(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    end_drowse_locked(now, true);
    portEXIT_CRITICAL(&s_lock);
}

/* Unlike the others it stays at 1 at the end, until the screen sleeps. */
float muse_state_sleepy(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    int64_t since = s_drowse_us;
    portEXIT_CRITICAL(&s_lock);
    if (!since) {
        return 0;
    }
    float t = (float)(now - since) / 1e6f / MUSE_SLEEPY_S;
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

/*
 * Tickle's progress, without stepping anything: it runs at 1/MUSE_TICKLE_S
 * but waits at TICKLE_WAIT while the tickling goes on (up to TICKLE_LINGER_US
 * after its last touch), then runs on from there to 1. 0 when not tickled or
 * once over.
 */
static float tickle_locked(int64_t now)
{
    if (!s_tickle_us) {
        return 0;
    }
    int64_t until = s_tickle_held_us + TICKLE_LINGER_US;
    float t = (float)((now < until ? now : until) - s_tickle_us) / 1e6f / MUSE_TICKLE_S;
    if (t > TICKLE_WAIT) {
        t = TICKLE_WAIT;
    }
    if (now > until) {
        t += (float)(now - until) / 1e6f / MUSE_TICKLE_S;
    }
    if (t >= 1) {
        s_tickle_us = 0;
        return 0;
    }
    return t < 0 ? 0 : t;
}

/* Still giggling (not yet winding down), so a touch keeps it going. */
static bool tickle_holds_locked(int64_t now)
{
    float t = tickle_locked(now);
    return t > 0 && (now <= s_tickle_held_us + TICKLE_LINGER_US || t < TICKLE_WAIT);
}

static void tickle_touch_locked(int64_t now)
{
    s_tickle_held_us = now;
    s_last_poke_us = now;
    end_drowse_locked(now, false);
}

bool muse_state_start_tickle(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool start = !tickle_holds_locked(now);
    if (start) {
        s_tickle_us = now;
        s_waking_us = 0;
    }
    tickle_touch_locked(now);
    portEXIT_CRITICAL(&s_lock);
    return start;
}

bool muse_state_tickle_hold(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool held = tickle_holds_locked(now);
    if (held) {
        tickle_touch_locked(now);
    }
    portEXIT_CRITICAL(&s_lock);
    return held;
}

float muse_state_tickle(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    float t = tickle_locked(now);
    portEXIT_CRITICAL(&s_lock);
    return s_mode == MUSE_MODE_IDLE ? t : 0;
}
