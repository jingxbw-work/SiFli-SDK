/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "epd_e0470a03_timing.h"
#include "system_bf0_ap.h"
#include "ptm_ins.h"

#ifndef HAL_LCDC_EPD_START_HOOK_SUPPORTED
#error "Missing LCDC EPD START hook"
#endif

#define PANEL_CKV_LOW_CLOCKS 55u
#define PANEL_SOURCE_CLOCK_HZ 24000000u
#define PANEL_ROWS 684u

const LCDC_InitTypeDef epd_e0470a03_gyd7n6_config = {
    .lcd_itf = LCDC_INTF_EPD_8BIT,
    .freq = PANEL_SOURCE_CLOCK_HZ,
    .color_mode = LCDC_PIXEL_FORMAT_F2_SWAP,
    .cfg = {.epd = {
        .SDMODE = 0, .SDCLK_polarity = 0, .GDSP_polarity = 0,
        .GDCLK_polarity = 1,
        .LSL = 20, .LBL = 64, .LEL = 20, .LDL = 1216u >> 2,
        .GSTA = 3, .FSL = 1, .FBL = 3, .FDL = PANEL_ROWS, .FEL = 1,
    }},
};

/* PTM1 core 0 starts the scan and generates SPV. Core 1 generates XLE
 * against CKV. The LCDC keeps ownership of XCL, XSTL, XOE and pixel data. */
#define SPV_PHASE_NS 7550u
/* The first native CKV low interval spans 1.0–3.3 us after LCDC BUSY. */
#define CKV_CONNECT_NS 1800u
#define XLE_SHIFT_NS 2000u
/* The native pulse rises 23 source clocks before the active CKV fall. */
#define XLE_BEFORE_CKV_NS 958u
#define XLE_WIDTH_NS 833u
#define XLE_FIRST_LINE 4u
#define SPV_PTM_CH (1u << 0)
#define XLE_PTM_CH (1u << 1)
#define PTM_CHANNELS (SPV_PTM_CH | XLE_PTM_CH)
#define EPD_PTM_ENABLE (PTM_CER_EN0 | PTM_CER_EN1)
#define EPD_PTM_RESET (PTM_CER_RST0 | PTM_CER_RST1)
#define SPV_PTM_FLAGS (PTM_ISR_EVT0 | PTM_ISR_ERR0 | PTM_ISR_TCM0)
#define XLE_PTM_FLAGS (PTM_ISR_EVT1 | PTM_ISR_ERR1 | PTM_ISR_TCM1)
#define EPD_PTM_IRQS (PTM_IER_EVTIE0 | PTM_IER_ERRIE0 | PTM_IER_EVTAUTO0 | \
                      PTM_IER_EVTIE1 | PTM_IER_ERRIE1 | PTM_IER_EVTAUTO1)

static LCDC_HandleTypeDef *timing_lcdc;
static uint32_t ckv_scan_pad, ckv_idle_pad;
static bool timing_armed, ptm_clock_was_enabled;

static uint32_t ptm_cycles(uint32_t hz, uint32_t ns)
{
    return ((uint64_t)hz * ns + 500000000u) / 1000000000u;
}

static void configure_spv_core(uint32_t last)
{
    hwp_ptm1->CER = PTM_CER_RST0;
    hwp_ptm1->ISR = SPV_PTM_FLAGS;
    MODIFY_REG(hwp_ptm1->IER, PTM_IER_EVTIE0 | PTM_IER_ERRIE0 | PTM_IER_EVTAUTO0,
               PTM_IER_EVTAUTO0);
    hwp_ptm1->CCR0 = (last << PTM_CCR0_END0_Pos) | (last << PTM_CCR0_END1_Pos);
    hwp_ptm1->ICR0 = 0;
    hwp_ptm1->OCR0 = 0;
}

void epd_e0470a03_timing_stop(void)
{
    if (!timing_lcdc) return;
    hwp_ptm1->CER = EPD_PTM_RESET;
    hwp_pinmux1->PAD_PA40 = ckv_idle_pad;
    hwp_ptm1->DOSR = SPV_PTM_CH;
    hwp_ptm1->DOCR = XLE_PTM_CH;
    timing_armed = false;
}

void epd_e0470a03_timing_release(void)
{
    if (!timing_lcdc) return;
    hwp_ptm1->CER = EPD_PTM_RESET;
    hwp_ptm1->DOSR = SPV_PTM_CH;
    hwp_ptm1->DOCR = XLE_PTM_CH;
    hwp_ptm1->DOECR = PTM_CHANNELS;
    CLEAR_BIT(hwp_ptm1->IER, EPD_PTM_IRQS);
    hwp_ptm1->ISR = SPV_PTM_FLAGS | XLE_PTM_FLAGS;
    timing_armed = false;
    timing_lcdc = NULL;
    if (!ptm_clock_was_enabled && !(hwp_ptm1->CER & 0xffu))
        HAL_RCC_DisableModule(RCC_MOD_PTM1);
}

bool epd_e0470a03_timing_prepare(LCDC_HandleTypeDef *lcdc)
{
    if (timing_lcdc) return false;
    uint32_t hz = HAL_RCC_GetHCLKFreq(CORE_ID_DEFAULT);
    uint32_t spv_delay = ptm_cycles(hz, SPV_PHASE_NS);
    uint32_t ckv_delay = ptm_cycles(hz, CKV_CONNECT_NS);
    uint32_t xle_delay = ptm_cycles(hz, XLE_SHIFT_NS - XLE_BEFORE_CKV_NS);
    uint32_t xle_width = ptm_cycles(hz, XLE_WIDTH_NS);
    if (!hz || !spv_delay || spv_delay > 65535u || !ckv_delay || ckv_delay > 65535u ||
        !xle_delay || xle_delay > 65535u || !xle_width || xle_width > 65535u)
        return false;
    if (!HAL_DBG_DWT_IsInit()) HAL_DBG_DWT_Init();
    if (!HAL_DBG_DWT_IsInit()) return false;
    ptm_clock_was_enabled = !!(hwp_hpsys_rcc->ENR1 & HPSYS_RCC_ENR1_PTM1);
    HAL_RCC_EnableModule(RCC_MOD_PTM1);
    if ((hwp_ptm1->CER & 0xffu) || (hwp_ptm1->DOER & PTM_CHANNELS))
    {
        if (!ptm_clock_was_enabled) HAL_RCC_DisableModule(RCC_MOD_PTM1);
        return false;
    }
    timing_lcdc = lcdc;

    configure_spv_core(1);
    volatile uint32_t *spv = PTM1_CORE0_TCM;
    spv[0] = PTM_SET(PTM_X, PTM_ZERO, OP_OR, 0x5757);
    spv[1] = PTM_SET(PTM_OS, PTM_ZERO, OP_OR, SPV_PTM_CH);
    __DSB();
    hwp_ptm1->CER = PTM_CER_EN0;
    uint32_t start = DWT->CYCCNT;
    while (!(hwp_ptm1->ISR & SPV_PTM_FLAGS))
    {
        if ((uint32_t)(DWT->CYCCNT - start) > hz / 10000u) goto fail;
    }
    if ((hwp_ptm1->ISR & SPV_PTM_FLAGS) != PTM_ISR_EVT0 ||
        hwp_ptm1->X0 != 0x5757 || !(hwp_ptm1->DOR & SPV_PTM_CH)) goto fail;

    ckv_scan_pad = hwp_pinmux1->PAD_PA40;
    GPIO_InitTypeDef ckv_output = {.Pin = 40, .Mode = GPIO_MODE_OUTPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_WritePin(hwp_gpio1, 40, GPIO_PIN_RESET);
    HAL_GPIO_Init(hwp_gpio1, &ckv_output);
    if (HAL_PIN_Set(PAD_PA40, GPIO_A40, PIN_NOPULL, 1) != 0) goto fail;
    HAL_PIN_SetMode(PAD_PA40, 1, PIN_DIGITAL_IO_NORMAL);
    ckv_idle_pad = hwp_pinmux1->PAD_PA40;

    /* Connect the native CKV signal during its first low interval. */
    const uint32_t spv_program[] = {
        PTM_SET(PTM_A, PTM_X, OP_OR, 0),
        PTM_SET(PTM_X, PTM_ZERO, OP_OR, LCD_IF_COMMAND_START),
        PTM_STM(PTM_X, (uint32_t)&lcdc->Instance->COMMAND),
        PTM_WAIT(PTM_TRIG_LCDC1_BUSY, 1, ckv_delay),
        PTM_STM(PTM_Y, (uint32_t)&hwp_pinmux1->PAD_PA40),
        PTM_WAIT(WAIT_PA40, 0, 0),
        PTM_WAIT(WAIT_PA40, 1, spv_delay),
        PTM_SET(PTM_OC, PTM_ZERO, OP_OR, SPV_PTM_CH),
        PTM_WAIT(WAIT_PA40, 0, 0),
        PTM_WAIT(WAIT_PA40, 1, spv_delay),
        PTM_SET(PTM_OS, PTM_ZERO, OP_OR, SPV_PTM_CH),
        PTM_WAIT(PTM_TRIG_LCDC1_BUSY, 0, 0),
        PTM_SET(PTM_X, PTM_A, OP_OR, 0),
        PTM_STM(PTM_X, (uint32_t)&hwp_pinmux1->PAD_PA40),
    };
    /* The first active XLE follows the falling edge after the four leading
     * CKV rises. Generate one pulse per data row from that same CKV edge. */
    const uint32_t xle_program[] = {
        PTM_SET(PTM_B, PTM_ZERO, OP_OR, XLE_FIRST_LINE),
        PTM_WAIT(WAIT_PA40, 1, 0),
        PTM_WAIT(WAIT_PA40, 0, 0),
        PTM_SET(PTM_B, PTM_B, OP_ADD, -1),
        PTM_JMP(-3, PTM_B, JMP_NEQ, PTM_ZERO, 0),
        PTM_SET(PTM_B, PTM_ZERO, OP_OR, PANEL_ROWS),
        PTM_WAIT(WAIT_PA40, 0, xle_delay),
        PTM_SET(PTM_OS, PTM_ZERO, OP_OR, XLE_PTM_CH),
        PTM_WAIT(WAIT_PA40, 0, xle_width),
        PTM_SET(PTM_OC, PTM_ZERO, OP_OR, XLE_PTM_CH),
        PTM_SET(PTM_B, PTM_B, OP_ADD, -1),
        PTM_JMP(4, PTM_B, JMP_EQ, PTM_ZERO, 0),
        PTM_WAIT(WAIT_PA40, 1, 0),
        PTM_WAIT(WAIT_PA40, 0, 0),
        PTM_JMP(-8, PTM_ZERO, JMP_EQ, PTM_ZERO, 0),
        PTM_SET(PTM_OC, PTM_ZERO, OP_OR, XLE_PTM_CH),
    };
    for (unsigned i = 0; i < sizeof(spv_program) / sizeof(spv_program[0]); ++i)
        spv[i] = spv_program[i];
    volatile uint32_t *xle = PTM1_CORE1_TCM;
    for (unsigned i = 0; i < sizeof(xle_program) / sizeof(xle_program[0]); ++i)
        xle[i] = xle_program[i];

    hwp_ptm1->DOSR = SPV_PTM_CH;
    hwp_ptm1->DOCR = XLE_PTM_CH;
    hwp_ptm1->DOESR = PTM_CHANNELS;
    if (HAL_PIN_Set(PAD_PA41, PTM1_CH0, PIN_NOPULL, 1) != 0 ||
        HAL_PIN_Set(PAD_PA25, PTM1_CH1, PIN_NOPULL, 1) != 0) goto fail;
    HAL_PIN_SetMode(PAD_PA41, 1, PIN_DIGITAL_IO_NORMAL);
    HAL_PIN_SetMode(PAD_PA25, 1, PIN_DIGITAL_IO_NORMAL);
    MODIFY_REG(lcdc->Instance->TCON_IF_CONF3, LCD_IF_TCON_IF_CONF3_GDH_Msk,
               PANEL_CKV_LOW_CLOCKS << LCD_IF_TCON_IF_CONF3_GDH_Pos);
    return true;
fail:
    epd_e0470a03_timing_release();
    return false;
}

bool epd_e0470a03_timing_arm(void)
{
    if (!timing_lcdc || (hwp_ptm1->CER & EPD_PTM_ENABLE)) return false;
    configure_spv_core(13);
    hwp_ptm1->CER = PTM_CER_RST1;
    hwp_ptm1->ISR = XLE_PTM_FLAGS;
    MODIFY_REG(hwp_ptm1->IER, PTM_IER_EVTIE1 | PTM_IER_ERRIE1 | PTM_IER_EVTAUTO1,
               PTM_IER_EVTAUTO1);
    hwp_ptm1->CCR1 = (15u << PTM_CCR1_END0_Pos) | (15u << PTM_CCR1_END1_Pos);
    hwp_ptm1->ICR1 = 0;
    hwp_ptm1->OCR1 = 0;
    hwp_ptm1->X0 = ckv_idle_pad;
    hwp_ptm1->Y0 = ckv_scan_pad;
    hwp_ptm1->DOSR = SPV_PTM_CH;
    hwp_ptm1->DOCR = XLE_PTM_CH;
    __DSB();
    timing_armed = true;
    return true;
}

bool HAL_LCDC_EPD_StartHook(LCDC_HandleTypeDef *lcdc)
{
    if (lcdc != timing_lcdc || !timing_armed) return false;
    timing_armed = false;
    hwp_ptm1->CER = EPD_PTM_ENABLE;
    return true;
}

bool epd_e0470a03_timing_complete(void)
{
    uint32_t status = hwp_ptm1->ISR;
    bool ok = (status & SPV_PTM_FLAGS) == PTM_ISR_EVT0 &&
              (status & XLE_PTM_FLAGS) == PTM_ISR_EVT1 &&
              !(hwp_ptm1->CER & EPD_PTM_ENABLE) &&
              (hwp_ptm1->DOR & PTM_CHANNELS) == SPV_PTM_CH;
    epd_e0470a03_timing_stop();
    return ok;
}
