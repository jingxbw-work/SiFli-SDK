/* SPDX-License-Identifier: Apache-2.0
 * E0470A03 only. 28 C is the validated table setting, not a sensor reading.
 */
#include <stdbool.h>
#include <rtthread.h>
#include "mem_map.h"
#include "mem_section.h"
#include "../epd_waveform.h"
#include "reference_waveform.h"
#include "epd_waveform_bin_reader.h"

#define TEXT_MAX_FRAMES 64
#define TEXT_ERASE_FRAMES 30
#define GL16_FRAMES 44u
#define GL16_WHITE_START 21u
#define GL16_WHITE_END 39u
#define GL16_ERASE_EXTRA 9u
#define GL29_FRAMES 29u
#define GL29_WHITE_END (GL29_FRAMES - 2u)
#define GL29_WHITE_SHIFT 6u

typedef struct
{
    uint8_t start;
    uint8_t white_start;
} erase_pulse_t;

static int ready;
static int selected_mode = 3;
/* Prepared before HV; four two-bit drive codes share each cached byte. */
static uint8_t text_luts[TEXT_MAX_FRAMES][256 / 4];
static uint32_t text_frames;
L2_RET_BSS_SECT_BEGIN(epd_gl16)
static uint8_t gray_repaint[256] L2_RET_BSS_SECT(epd_gl16);
static erase_pulse_t erase[15] L2_RET_BSS_SECT(epd_gl16);
static erase_pulse_t fast_erase[15] L2_RET_BSS_SECT(epd_gl16);
static uint32_t fast_draw_hold[15] L2_RET_BSS_SECT(epd_gl16);
static uint32_t fast_draw_dark[15] L2_RET_BSS_SECT(epd_gl16);
L2_RET_BSS_SECT_END
static bool fast_partial_ready, gray_repaint_ready, erase_ready, fast_erase_ready;

static unsigned gray_repaint_index(unsigned gray)
{
    /* Avoid using the diagonal entry as either member of the equivalent pair. */
    return ((gray == 6u || gray == 7u ? 5u : 7u) << 4) | gray;
}

static bool prepare_gray_repaint(void)
{
    uint32_t frames = waveform_bin_reader_get_frames(28, WAVE_MODE_PARTIAL);
    if (!frames) return false;
    uint32_t raw[256];
    /* In this BIN, the 6 and 7 source levels have identical sequences for
     * these targets. Use the 4 and 5 pair for targets 6 and 7. */
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        waveform_bin_reader_fill_lut(raw, frame);
        for (unsigned gray = 0; gray < 15; ++gray)
        {
            unsigned index = gray_repaint_index(gray);
            if (((raw[index] ^ raw[index - 16u]) & (3u << 3)) != 0) return false;
        }
    }
    for (unsigned i = 0; i < 256; ++i) gray_repaint[i] = (uint8_t)i;
    for (unsigned gray = 0; gray < 15; ++gray)
    {
        unsigned index = gray_repaint_index(gray);
        gray_repaint[index] = (uint8_t)(index - 16u);
        gray_repaint[gray * 17u] = (uint8_t)index;
    }
    return true;
}

static bool prepare_balanced_erase(void)
{
    if (waveform_bin_reader_get_frames(28, WAVE_MODE_PARTIAL) != GL16_FRAMES) return false;
    uint32_t raw[256];
    uint8_t dark_start[15];
    for (unsigned gray = 0; gray < 15; ++gray) dark_start[gray] = GL16_WHITE_START;
    for (unsigned frame = 0; frame < GL16_FRAMES; ++frame)
    {
        waveform_bin_reader_fill_lut(raw, frame);
        for (unsigned gray = 0; gray < 15; ++gray)
        {
            unsigned code = (raw[(gray << 4) | 15u] >> 3) & 3u;
            if (frame < GL16_WHITE_START)
            {
                if (code == 1u && dark_start[gray] == GL16_WHITE_START)
                    dark_start[gray] = frame;
                if (code != (frame >= dark_start[gray] ? 1u : 0u)) return false;
            }
            else if (code != (frame < GL16_WHITE_END ? 2u : 0u)) return false;
        }
    }
    /* Extend both polarities by the same count, using the leading HOLD phases.
     * Scale the extra drive down for lighter source grays. */
    for (unsigned gray = 0; gray < 15; ++gray)
    {
        unsigned extra = ((15u - gray) * GL16_ERASE_EXTRA + 7u) / 15u;
        if (extra * 2u > dark_start[gray]) extra = dark_start[gray] / 2u;
        erase[gray].start = dark_start[gray] - 2u * extra;
        erase[gray].white_start = GL16_WHITE_START - extra;
    }
    return true;
}

static bool prepare_fast_erase(void)
{
    if (waveform_bin_reader_get_frames(28, (WAVE_TABLE_MODE_T)EPD_WAVE_GL16_FAST) != GL29_FRAMES)
        return false;

    uint32_t raw[256];
    uint8_t dark[15] = {0}, white[15] = {0};
    uint8_t draw_start[15];
    uint32_t prewhite[15] = {0};
    for (unsigned gray = 0; gray < 15; ++gray) draw_start[gray] = GL29_FRAMES;
    for (unsigned frame = 0; frame < GL29_FRAMES; ++frame)
    {
        waveform_bin_reader_fill_lut(raw, frame);
        for (unsigned gray = 0; gray < 15; ++gray)
        {
            unsigned code = (raw[(gray << 4) | 15u] >> 3) & 3u;
            if (code == 3u || (code && frame >= GL29_WHITE_END)) return false;
            dark[gray] += code == 1u;
            white[gray] += code == 2u;

            code = (raw[0xf0u | gray] >> 3) & 3u;
            if (draw_start[gray] != GL29_FRAMES) continue;
            if (code == 1u) draw_start[gray] = frame;
            else if (code == 2u) prewhite[gray] |= 1u << frame;
            else if (code == 3u) return false;
        }
    }

    /* Shift the white endpoint by the same amount on entry and departure,
     * so every closed path through the gray levels retains its net drive. */
    for (unsigned gray = 0; gray < 15; ++gray)
    {
        if (white[gray] <= dark[gray]) return false;
        unsigned net = white[gray] - dark[gray] + GL29_WHITE_SHIFT;
        /* Use the first phase for mid/light gray edges to fit one more
         * balanced pair where the net drive leaves room. */
        unsigned lead_hold = gray < 7u ? 1u : 0u;
        unsigned white_count = (GL29_WHITE_END - lead_hold + net) / 2u;
        /* Avoid darkening faint edges with a longer reverse pulse. */
        if (gray >= 9u && white_count > net + 8u)
            white_count = net + 8u;
        if (white_count < white[gray] || net > white_count || draw_start[gray] >= GL29_WHITE_END)
            return false;
        fast_erase[gray].white_start = GL29_WHITE_END - white_count;
        fast_erase[gray].start = fast_erase[gray].white_start - (white_count - net);

        unsigned remaining = GL29_WHITE_SHIFT;
        uint32_t hold = 0, draw = 0;
        for (unsigned frame = draw_start[gray]; frame-- > 0 && remaining;)
        {
            uint32_t bit = 1u << frame;
            if (!(prewhite[gray] & bit)) continue;
            hold |= bit;
            --remaining;
        }
        for (unsigned frame = draw_start[gray]; frame-- > 1 && remaining;)
        {
            uint32_t bit = 1u << frame;
            if ((prewhite[gray] & bit) && !(hold & bit)) continue;
            draw |= bit;
            --remaining;
        }
        if (remaining) return false;
        fast_draw_hold[gray] = hold;
        fast_draw_dark[gray] = draw;
    }
    return true;
}

static void apply_erase(uint32_t *lut, uint32_t frame, unsigned end, const erase_pulse_t *pulses)
{
    if (frame >= end) return;
    for (unsigned gray = 0; gray < 15; ++gray)
    {
        const erase_pulse_t *pulse = &pulses[gray];
        unsigned code = 0;
        if (frame >= pulse->start)
            code = frame < pulse->white_start ? 1u : 2u;
        lut[(gray << 4) | 15u] = code << 3;
    }
}

const uint8_t *epd_wave_gray_repaint_indices(void)
{
    return selected_mode == WAVE_MODE_PARTIAL && gray_repaint_ready ? gray_repaint : NULL;
}

static void text_lut_set(uint32_t frame, unsigned index, unsigned code)
{
    unsigned shift = (index & 3u) * 2u;
    uint8_t *packed = &text_luts[frame][index >> 2];
    *packed = (uint8_t)((*packed & ~(3u << shift)) | ((code & 3u) << shift));
}

static int prepare_text(void)
{
    uint32_t raw[256], gray_frames, du_frames, f, i;
    unsigned white_count[16] = {0}, black_count = 0;
    uint8_t white_code = 0, black_code = 0;
    int white_done = 0, black_done = 0;

    if (text_frames) return 0;
    gray_frames = waveform_bin_reader_get_frames(28, WAVE_MODE_PARTIAL);
    if (gray_frames < TEXT_ERASE_FRAMES + 2 || gray_frames > TEXT_MAX_FRAMES)
        return -RT_ERROR;
    for (f = 0; f < gray_frames; ++f)
    {
        waveform_bin_reader_fill_lut(raw, f);
        for (i = 0; i < 256; ++i)
        {
            unsigned old = i >> 4, next = i & 15;
            text_lut_set(f, i, old == next || next == 0 || next == 15
                              ? 0 : ((raw[i] >> 3) & 3));
        }
    }

    du_frames = waveform_bin_reader_get_frames(28, (WAVE_TABLE_MODE_T)1);
    if (!du_frames || du_frames > gray_frames) return -RT_ERROR;
    for (f = 0; f < du_frames; ++f)
    {
        uint8_t white, black;
        waveform_bin_reader_fill_lut(raw, f);
        white = (raw[0x0f] >> 3) & 3;
        black = (raw[0xf0] >> 3) & 3;
        if (!f)
        {
            white_code = white; black_code = black;
            if (!white || !black || white == 3 || black == 3 || white == black)
                return -RT_ERROR;
        }
        /* The fitted pulse assumes a single leading DU pulse, then neutral. */
        if ((white && white_done) || (black && black_done)) return -RT_ERROR;
        if (!white) white_done = 1;
        if (!black) black_done = 1;
        if (black) ++black_count;
        for (i = 0; i < 16; ++i)
        {
            unsigned b = (raw[i << 4] >> 3) & 3;
            unsigned w = (raw[(i << 4) | 15] >> 3) & 3;
            if ((b && b != black_code) || (w && w != white_code)) return -RT_ERROR;
            if (i) text_lut_set(f, i << 4, b);
            if (w) ++white_count[i];
        }
    }
    if (white_count[0] != 18 || black_count != 18) return -RT_ERROR;
    /* Preserve the DU's relative erase duration for lighter old gray levels. */
    for (i = 0; i < 15; ++i)
    {
        unsigned count;
        if (!white_count[i] || white_count[i] > white_count[0]) return -RT_ERROR;
        count = (white_count[i] * TEXT_ERASE_FRAMES + white_count[0] / 2) / white_count[0];
        for (f = 0; f < count; ++f) text_lut_set(f, (i << 4) | 15, white_code);
    }
    text_frames = gray_frames;
    return 0;
}

void ref57_wave_init(void)
{
    text_frames = 0;
    selected_mode = 3;
    ready = waveform_bin_reader_init(CUSTOM_EPD_WAVE_TABLE_SIZE) == 0;
    fast_partial_ready = ready && waveform_bin_reader_get_frames(28, (WAVE_TABLE_MODE_T)EPD_WAVE_GL16_FAST);
    gray_repaint_ready = ready && prepare_gray_repaint();
    erase_ready = ready && prepare_balanced_erase();
    fast_erase_ready = fast_partial_ready && prepare_fast_erase();
}

int ref57_wave_prepare_mode(int mode)
{
    selected_mode = -1;
    if (!ready) return -RT_ERROR;
    if (mode == EPD_WAVE_GL16_FAST && !fast_partial_ready) mode = WAVE_MODE_PARTIAL;
    if (mode != 0 && mode != 1 && mode != 3 &&
        mode != EPD_WAVE_GL16_FAST && mode != WAVE_MODE_PARTIAL && mode != EPD_WAVE_TEXT)
        return -RT_EINVAL;
    selected_mode = mode;
    return mode == EPD_WAVE_TEXT ? prepare_text() : 0;
}

int ref57_wave_get_selected_mode(void) { return selected_mode; }

uint32_t ref57_wave_get_frames(void)
{
    if (!ready || selected_mode < 0) return 0;
    if (selected_mode == EPD_WAVE_TEXT) return text_frames;
    return waveform_bin_reader_get_frames(28, (WAVE_TABLE_MODE_T)selected_mode);
}

void ref57_wave_fill_lut(uint32_t *lut, uint32_t frame_num)
{
    unsigned i;
    if (selected_mode == EPD_WAVE_TEXT)
    {
        if (frame_num >= text_frames)
        {
            for (i = 0; i < 256; ++i) lut[i] = 0xff000000u;
            return;
        }
        const uint8_t *codes = text_luts[frame_num];
        for (i = 0; i < 256; i += 4)
        {
            unsigned packed = *codes++;
            lut[i]     = 0xff000000u | (packed & 3u);
            lut[i + 1] = 0xff000000u | ((packed >> 2) & 3u);
            lut[i + 2] = 0xff000000u | ((packed >> 4) & 3u);
            lut[i + 3] = 0xff000000u | ((packed >> 6) & 3u);
        }
        return;
    }
    waveform_bin_reader_fill_lut(lut, frame_num);
    const uint8_t *indices = epd_wave_gray_repaint_indices();
    if (indices)
    {
        for (unsigned gray = 0; gray < 15; ++gray)
            lut[gray_repaint_index(gray)] = lut[gray * 17u];
    }
    if (selected_mode == WAVE_MODE_PARTIAL && erase_ready)
        apply_erase(lut, frame_num, GL16_WHITE_END, erase);
    else if (selected_mode == EPD_WAVE_GL16_FAST && fast_erase_ready && frame_num < GL29_WHITE_END)
    {
        apply_erase(lut, frame_num, GL29_WHITE_END, fast_erase);
        uint32_t bit = 1u << frame_num;
        for (unsigned gray = 0; gray < 15; ++gray)
        {
            if (fast_draw_dark[gray] & bit) lut[0xf0u | gray] = 1u << 3;
            else if (fast_draw_hold[gray] & bit) lut[0xf0u | gray] = 0;
        }
    }
    /* Keep unchanged pixels steady during a partial scan of the whole panel. */
    bool partial = selected_mode == 1 || selected_mode == WAVE_MODE_PARTIAL ||
                   selected_mode == EPD_WAVE_GL16_FAST;
    for (i = 0; i < 256; ++i)
    {
        /* Vendor bits 4:3 -> EPD_8BIT/F2_SWAP bits 1:0. */
        lut[i] = (lut[i] >> 3) | 0xff000000u;
        if (partial && (i >> 4) == (i & 15)) lut[i] = 0xff000000u;
    }
}
