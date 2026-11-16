/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EPD_E0470A03_TIMING_H
#define EPD_E0470A03_TIMING_H

#include "bf0_hal.h"

extern const LCDC_InitTypeDef epd_e0470a03_gyd7n6_config;

bool epd_e0470a03_timing_prepare(LCDC_HandleTypeDef *lcdc);
bool epd_e0470a03_timing_arm(void);
bool epd_e0470a03_timing_complete(void);
void epd_e0470a03_timing_stop(void);
void epd_e0470a03_timing_release(void);

#endif
