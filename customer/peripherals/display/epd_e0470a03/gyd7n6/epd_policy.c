/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <rthw.h>
#include "../epd_waveform.h"
#include "epd_waveform_bin_reader.h"
#include "reference_waveform.h"

#define MODE_QUEUE_SIZE 2u
static EpdDrawMode modes[MODE_QUEUE_SIZE];
static unsigned head, count, since_full;
static int period = 10, forced, manual = -1;
static int displayed;
static bool frame_pending, frame_auto;
static unsigned cadence_version, frame_version;

void epd_wave_table(void)
{
    head = count = since_full = 0;
    forced = displayed = 0;
    manual = -1;
    frame_pending = frame_auto = false;
    cadence_version = frame_version = 0;
    ref57_wave_init();
}

void epd_wave_set_part_times(int value)
{
    rt_base_t level = rt_hw_interrupt_disable();
    value = value > 0 ? value : 1;
    if (period != value) { period = value; since_full = 0; ++cadence_version; }
    rt_hw_interrupt_enable(level);
}

int epd_wave_get_part_times(void) { return period; }
int epd_wave_get_mode(void) { return manual; }
void epd_wave_set_mode(int mode)
{
    if (mode == -1 || mode == 0 || mode == 1 || mode == 3 ||
        mode == EPD_WAVE_GL16_FAST || mode == WAVE_MODE_PARTIAL || mode == EPD_WAVE_TEXT)
        manual = mode;
}

/* The composite is fitted to the BIN's [27,30) table, selected at 28 C. */
int epd_wave_get_tempzone(void) { return 10; }
void epd_wave_set_tempzone(int zone) { (void)zone; }

void epd_wave_request_full(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    forced = 1;
    rt_hw_interrupt_enable(level);
}

void epd_wave_submit_frame(EpdDrawMode mode)
{
    rt_base_t level = rt_hw_interrupt_disable();
    RT_ASSERT(count < MODE_QUEUE_SIZE);
    if (forced) { mode = EPD_DRAW_MODE_FULL; forced = 0; }
    modes[(head + count) % MODE_QUEUE_SIZE] = mode;
    ++count;
    rt_hw_interrupt_enable(level);
}

void epd_wave_begin_frame(void) {}

static bool skip_unchanged_frame(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    int skip = displayed && manual < 0 && !forced && count &&
               modes[head] != EPD_DRAW_MODE_FULL;
    if (skip) { head = (head + 1) % MODE_QUEUE_SIZE; --count; }
    rt_hw_interrupt_enable(level);
    return skip;
}

uint32_t epd_wave_table_get_frames(int temperature, EpdDrawMode mode)
{
    (void)temperature; /* This board uses the calibrated 28 C table. */
    rt_base_t level = rt_hw_interrupt_disable();
    if (count)
    {
        mode = modes[head];
        head = (head + 1) % MODE_QUEUE_SIZE;
        --count;
    }
    /* A mode change can request a full scan after the UI queued this frame. */
    if (forced) { mode = EPD_DRAW_MODE_FULL; forced = 0; }
    int selected = manual;
    if (selected < 0)
    {
        int full = !displayed || mode == EPD_DRAW_MODE_FULL;
        if (!full && mode == EPD_DRAW_MODE_AUTO)
            full = since_full + 1u >= (unsigned)period;
        selected = WAVE_MODE_FULL;
        if (!full)
            selected = mode == EPD_DRAW_MODE_INPUT ? 1 : EPD_WAVE_GL16_FAST;
    }
    /* Commit the cadence only after all panel scans complete successfully. */
    RT_ASSERT(!frame_pending);
    frame_pending = true;
    frame_auto = manual < 0 && mode == EPD_DRAW_MODE_AUTO;
    frame_version = cadence_version;
    rt_hw_interrupt_enable(level);
    if (ref57_wave_prepare_mode(selected)) return 0;
    return ref57_wave_get_frames();
}

bool epd_wave_binary_target(void)
{
    return displayed && manual < 0 && !forced && count &&
           modes[head] == EPD_DRAW_MODE_INPUT;
}

bool epd_wave_quantize_input(uint8_t *pixels, size_t pixel_count)
{
    bool changed = false;
    for (size_t i = 0; i < pixel_count; ++i) {
        unsigned old = pixels[i] >> 4, target = pixels[i] & 15;
        /* DU keeps the actual previous gray and only quantizes new ink. */
        if (old != target) target = target < 8 ? 0 : 15;
        pixels[i] = (uint8_t)((old << 4) | target);
        changed |= old != target;
    }
    return changed;
}

int epd_wave_prepare_frame(bool changed)
{
    if (!changed && skip_unchanged_frame()) return 0;
    uint32_t frames = epd_wave_table_get_frames(28, EPD_DRAW_MODE_AUTO);
    return frames ? (int)frames : -RT_ERROR;
}

void epd_wave_table_fill_lut(uint32_t *lut, uint32_t frame)
{
    ref57_wave_fill_lut(lut, frame);
}

void epd_wave_finish_frame(uint8_t *pixels, size_t pixel_count, bool success)
{
    (void)pixels;
    (void)pixel_count;
    rt_base_t level = rt_hw_interrupt_disable();
    if (frame_pending && success)
    {
        int selected = ref57_wave_get_selected_mode();
        if (selected == 0) since_full = 0;
        else if (selected != 3 && frame_auto && frame_version == cadence_version) ++since_full;
        if (selected != 3) displayed = 1;
    }
    if (!success) forced = 1;
    frame_pending = false;
    rt_hw_interrupt_enable(level);
}
