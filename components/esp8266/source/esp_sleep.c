// Copyright 2019-2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sys/param.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "FreeRTOS.h"
#include "freertos/task.h"
#include "driver/soc.h"
#include "esp8266/timer_struct.h"
#include "esp8266/uart_struct.h"
#include "esp8266/rom_functions.h"
#include "driver/rtc.h"


#define MIN_SLEEP_US            (3 * 1000)
#define WAKEUP_EARLY_US         (2520)

#define TAG                     "esp8266_pm"

typedef struct pm_soc_clk {
    uint32_t    ccount;
} pm_soc_clk_t;

static uint32_t s_lock_cnt = 1;
static uint32_t s_sleep_wakup_triggers;
static uint32_t s_sleep_duration;

static inline uint32_t save_local_wdev(void)
{
    extern uint32_t WDEV_INTEREST_EVENT;

    uint32_t reg = WDEV_INTEREST_EVENT;

    REG_WRITE(INT_ENA_WDEV, WDEV_TSF0_REACH_INT);

    return reg;
}

static inline void restore_local_wdev(uint32_t reg)
{
    REG_WRITE(INT_ENA_WDEV, reg);
}

static inline void save_soc_clk(pm_soc_clk_t *clk)
{
    clk->ccount = soc_get_ccount();
}

static inline uint32_t min_sleep_us(pm_soc_clk_t *clk)
{
    const int32_t os_sleep_us = ((int32_t)soc_get_ccompare() - (int32_t)clk->ccount) / g_esp_ticks_per_us +
                                        prvGetExpectedIdleTime() * portTICK_RATE_MS * 1000;
    const uint32_t ccompare_sleep_us = os_sleep_us > 0 ? os_sleep_us : 0;

    return MIN(ccompare_sleep_us, ccompare_sleep_us);
}

static inline void update_soc_clk(pm_soc_clk_t *clk, uint32_t us)
{
    const uint32_t os_ccount = us * g_esp_ticks_per_us + clk->ccount;

    if (os_ccount >= _xt_tick_divisor) 
        soc_set_ccompare(os_ccount + 32);
    soc_set_ccount(os_ccount);
}

esp_err_t esp_sleep_enable_timer_wakeup(uint32_t time_in_us)
{
    if (time_in_us <= MIN_SLEEP_US)
        return ESP_ERR_INVALID_ARG;

    s_sleep_duration = time_in_us;
    s_sleep_wakup_triggers |= RTC_TIMER_TRIG_EN;

    return ESP_OK;
}

esp_err_t esp_light_sleep_start(void)
{
    const uint32_t rtc_cal = esp_clk_cal_get(CRYSTAL_USED);
    const uint32_t sleep_rtc_ticks = rtc_time_us_to_clk(s_sleep_duration - WAKEUP_EARLY_US, rtc_cal);
    const rtc_cpu_freq_t cpu_freq = rtc_clk_cpu_freq_get();

    rtc_light_sleep_start(sleep_rtc_ticks, s_sleep_wakup_triggers, 0);

    rtc_clk_init();
    rtc_clk_cpu_freq_set(cpu_freq);

    return ESP_OK; 
}

void esp_sleep_lock(void)
{
    const esp_irqflag_t irqflag = soc_save_local_irq();
    s_lock_cnt++;
    soc_restore_local_irq(irqflag);
}

void esp_sleep_unlock(void)
{
    const esp_irqflag_t irqflag = soc_save_local_irq();
    s_lock_cnt--;
    soc_restore_local_irq(irqflag);
}

void IRAM_ATTR esp_sleep_start(void)
{
    if (s_lock_cnt) {
        soc_wait_int();
        return ;
    }

    int slept = 0;
    pm_soc_clk_t clk;
    const esp_irqflag_t irqflag = soc_save_local_irq();
    const uint32_t wdevflag = save_local_wdev();

    save_soc_clk(&clk);

    const uint32_t sleep_us = min_sleep_us(&clk);
    if (sleep_us > MIN_SLEEP_US) {
        const uint32_t rtc_cal = esp_clk_cal_get(CRYSTAL_USED);
        const uint32_t sleep_rtc_ticks = rtc_time_us_to_clk(sleep_us - WAKEUP_EARLY_US, rtc_cal);
        const rtc_cpu_freq_t cpu_freq = rtc_clk_cpu_freq_get();

        rtc_light_sleep_start(sleep_rtc_ticks, s_sleep_wakup_triggers | RTC_TIMER_TRIG_EN, 0);

        rtc_clk_init();
        rtc_clk_cpu_freq_set(cpu_freq);   

        update_soc_clk(&clk, sleep_us);

        slept = 1;
    }

    restore_local_wdev(wdevflag);
    soc_restore_local_irq(irqflag);

    if (!slept)
        soc_wait_int();
}
