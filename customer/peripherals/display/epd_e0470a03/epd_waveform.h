/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __EPD_WAVEFORM_H__
#define __EPD_WAVEFORM_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum
{
    EPD_DRAW_MODE_INVALID = 0,
    EPD_DRAW_MODE_AUTO = 1,
    EPD_DRAW_MODE_FULL = 2,
    EPD_DRAW_MODE_PARTIAL = 3,
    EPD_DRAW_MODE_INPUT = 4,
} EpdDrawMode;

void epd_wave_table(void);//初始化wave table

uint32_t epd_wave_table_get_frames(int temperature, EpdDrawMode mode);

void epd_wave_table_fill_lut(uint32_t *p_argb8888_lut, uint32_t frame_num);

/* 手动设置面板驱动支持的 WAVE_MODE；取值由驱动定义，-1 表示 AUTO 模式。 */
void epd_wave_set_mode(int mode);

/* 获取当前波形模式 */
int epd_wave_get_mode(void);

/* 手动设置温度区间 (0~13)，设置为 -1 表示自动遍历 */
void epd_wave_set_tempzone(int zone);

/* 获取当前温度区间索引 */
int epd_wave_get_tempzone(void);

/* 设置全刷周期（默认 10）；val<=0 等效 1。周期变化或全刷后重新计数。 */
void epd_wave_set_part_times(int val);

/* 获取当前全刷周期（每 N 次刷新一次全刷） */
int epd_wave_get_part_times(void);

/* 请求下一次刷新强制全刷（一次性，之后恢复周期；用于清残影/手动全刷） */
void epd_wave_request_full(void);

/* Queue one mode per assembled LVGL frame, in LCD submission order. */
void epd_wave_submit_frame(EpdDrawMode mode);

#ifdef LCD_USING_EPD_E0470A03_V10_57
void epd_wave_begin_frame(void);
bool epd_wave_binary_target(void);

/* Quantize one dirty INPUT span, preserving the physical source and unchanged
 * gray pixels. Return whether any source/target differences remain. */
bool epd_wave_quantize_input(uint8_t *pixels, size_t pixel_count);
/* After pixel preparation, return the number of scans, 0 for an unchanged
 * frame, or a negative error. */
int epd_wave_prepare_frame(bool changed);
/* GL16 indices for repainted pixels. The target gray remains in the low nibble. */
const uint8_t *epd_wave_gray_repaint_indices(void);
/* Report the whole request, including recovery scans, exactly once. */
void epd_wave_finish_frame(uint8_t *pixels, size_t pixel_count, bool success);
#endif

#endif /* __EPD_WAVEFORM_H__ */
