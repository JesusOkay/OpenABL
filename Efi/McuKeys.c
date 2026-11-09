/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
  McuKeys - see McuKeys.h. Register values follow Linux's GENI UART driver
  (qcom_geni_serial, FIFO mode) and what it leaves on a Flip2 at 115200.
*/

#include "Efi/McuKeys.h"

#include "Core/Settings.h"
#include "Efi/DpProbe.h"
#include "Efi/Efi.h"

#define GCC          0x00100000u
#define GCC_VOTE     (GCC + 0x52010u)  // APCS_CLOCK_BRANCH_ENA_VOTE_2
#define GCC_S2_CBCR  (GCC + 0x1e26cu)  // qupv3_wrap2_s2
#define GCC_S2_RCG   (GCC + 0x1e270u)
#define TLMM_SOUTH   0x0f500000u
#define TLMM_NORTH   0x0f900000u
#define SE           0x00888000u       // QUP16 = wrap2 SE2

#define GENI_FORCE_DEFAULT 0x020u
#define GENI_OUTPUT_CTRL   0x024u
#define GENI_CGC_CTRL      0x028u
#define GENI_CLK_M         0x048u
#define GENI_CLK_S         0x04cu
#define SE_GENI_STATUS     0x040u
#define GENI_FW_REV        0x068u
#define DMA_MODE_EN        0x258u
#define UART_TX_TRANS_CFG  0x25cu
#define TX_PACKING_0       0x260u
#define TX_PACKING_1       0x264u
#define UART_TX_WORD_LEN   0x268u
#define UART_TX_STOP_LEN   0x26cu
#define UART_TX_LEN        0x270u
#define UART_RX_TRANS_CFG  0x280u
#define RX_PACKING_0       0x284u
#define RX_PACKING_1       0x288u
#define UART_RX_WORD_LEN   0x28cu
#define UART_RX_STALE      0x294u
#define UART_TX_PARITY     0x2a4u
#define UART_RX_PARITY     0x2a8u
#define M_CMD0             0x600u
#define M_IRQ_STATUS       0x610u
#define M_IRQ_CLEAR        0x618u
#define S_CMD0             0x630u
#define S_IRQ_CLEAR        0x648u
#define TX_FIFO            0x700u
#define RX_FIFO            0x780u
#define TX_FIFO_STATUS     0x800u
#define RX_FIFO_STATUS     0x804u
#define DMA_TX_IRQ_CLR     0xc44u
#define DMA_RX_IRQ_CLR     0xd44u
#define GSI_EVENT_EN       0xe18u
#define SE_IRQ_EN          0xe1cu
#define DMA_GENERAL_CFG    0xe30u

#define FRAME_MAX 72u

STATIC BOOLEAN mUp;
STATIC UINT8   mFrame[FRAME_MAX];
STATIC UINT32  mHave;
STATIC UINT32  mHeld;

STATIC UINT32
R (IN UINT32 Addr)
{
  return *(volatile UINT32 *)(UINTN)Addr;
}

STATIC VOID
W (
  IN UINT32 Addr,
  IN UINT32 Val
)
{
  *(volatile UINT32 *)(UINTN)Addr = Val;
  __asm__ volatile ("dsb sy" ::: "memory");
}

STATIC BOOLEAN
WaitSet (
  IN UINT32 Addr,
  IN UINT32 Mask,
  IN UINT32 Want
)
{
  for (UINT32 T = 0; T < 1000; T++) {
    if ((R (Addr) & Mask) == Want) {
      return TRUE;
    }
    gBS->Stall (10);
  }
  return FALSE;
}

// TLMM pin as a GPIO output, pulled down, 2 mA.
STATIC VOID
GpioOut (
  IN UINT32  Tile,
  IN UINT32  Pin,
  IN BOOLEAN High
)
{
  W (Tile + Pin * 0x1000u + 4, High ? BIT1 : 0);
  W (Tile + Pin * 0x1000u, BIT9 | 1);
}

STATIC VOID
StartRead (VOID)
{
  W (SE + S_IRQ_CLEAR, 0xffffffffu);
  W (SE + S_CMD0, (1u << 27) | BIT7);  // UART_START_READ, RFR open
}

// One frame out, a byte per FIFO word, then wait for the command to end.
STATIC BOOLEAN
Send (
  IN CONST UINT8 *Data,
  IN UINT32       Len
)
{
  W (SE + M_IRQ_CLEAR, 0xffffffffu);
  W (SE + UART_TX_LEN, Len);
  W (SE + M_CMD0, 1u << 27);  // UART_START_TX
  for (UINT32 I = 0; I < Len; I++) {
    UINT32 T = 0;

    // The TX FIFO is 16 words deep on this SE (SE_HW_PARAM_0).
    while ((R (SE + TX_FIFO_STATUS) & 0x0fffffff) >= 16) {
      if (++T > 1000) {
        return FALSE;
      }
      gBS->Stall (10);
    }
    W (SE + TX_FIFO, Data[I]);
  }
  if (!WaitSet (SE + M_IRQ_STATUS, BIT0, BIT0)) {
    return FALSE;
  }
  W (SE + M_IRQ_CLEAR, 0xffffffffu);
  return TRUE;
}

STATIC VOID Bringup (VOID);

// A bus hang here would stop every boot, so the attempt is marked in misc
// first: after a forced restart (hold Power) the mark is still there and
// the MCU is left alone from then on.
VOID
McuKeysStart (VOID)
{
  STATIC CONST UINT8 One = 1;

  if (SettingsFlagIs (SETTINGS_FLAG_MCU_TRYING, 1)) {
    DpProbeMark ("mcu: an earlier start didn't finish, skipped\n");
    return;
  }
  if (EFI_ERROR (SettingsFlagSet (SETTINGS_FLAG_MCU_TRYING, &One, 1))) {
    return;
  }
  Bringup ();
  SettingsFlagSet (SETTINGS_FLAG_MCU_TRYING, NULL, 0);
}

STATIC VOID
Bringup (VOID)
{
  // The setup frames of ROCKNIX's driver, in order.
  STATIC CONST UINT8 F0[] = {0xa5, 0xd3, 0x5a, 0x3d, 0x0, 0x1, 0x2, 0x0, 0x7, 0x1, 0x5};
  STATIC CONST UINT8 F1[] = {0xa5, 0xd3, 0x5a, 0x3d, 0x1, 0x1, 0x1, 0x0, 0x6, 0x7};
  STATIC CONST UINT8 F2[] = {0xa5, 0xd3, 0x5a, 0x3d, 0x2, 0x1, 0x1, 0x0, 0x2, 0x0};
  STATIC CONST UINT8 F3[] = {0xa5, 0xd3, 0x5a, 0x3d, 0x3, 0x01, 0x0a, 0x00, 0x05, 0x01,
                             0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x07, 0x23};
  STATIC CONST UINT8 F4[] = {0xa5, 0xd3, 0x5a, 0x3d, 0x4, 0x1, 0x1, 0x0, 0x6, 0x2};
  STATIC CONST UINT8 F5[] = {0xa5, 0xd3, 0x5a, 0x3d, 0x5, 0x1, 0x1, 0x0, 0x2, 0x7};
  STATIC CONST struct { CONST UINT8 *P; UINT32 N; } Frames[] = {
    {F0, sizeof (F0)}, {F1, sizeof (F1)}, {F2, sizeof (F2)},
    {F3, sizeof (F3)}, {F4, sizeof (F4)}, {F5, sizeof (F5)}
  };
  UINT32 Fw;

  // Wrapper core, core 2x, S/M AHB and the SE clock; reads before this
  // would hang the bus.
  W (GCC_VOTE, R (GCC_VOTE) | BIT0 | BIT1 | BIT2 | BIT3 | BIT6);
  if (!WaitSet (GCC_S2_CBCR, BIT31, 0)) {
    DpProbeMark ("mcu: qup16 clock off %08x\n", R (GCC_S2_CBCR));
    return;
  }
  Fw = R (SE + GENI_FW_REV);
  if (((Fw >> 8) & 0xff) != 2) {
    DpProbeMark ("mcu: qup16 is not a UART (fw %x)\n", Fw);
    return;
  }
  // 7.3728 MHz = GPLL0 even (300 MHz) * 384 / 15625, dual-edge MND.
  W (GCC_S2_RCG + 4, 0x2601);
  W (GCC_S2_RCG + 8, 384);
  W (GCC_S2_RCG + 12, (~(15625u - 384u)) & 0xffff);
  W (GCC_S2_RCG + 16, (~15625u) & 0xffff);
  W (GCC_S2_RCG, R (GCC_S2_RCG) | BIT0);
  if (!WaitSet (GCC_S2_RCG, BIT0, 0)) {
    DpProbeMark ("mcu: qup16 rcg stuck %08x\n", R (GCC_S2_RCG));
    return;
  }

  // MCU power (vreg_mcu_3v3), then reset with boot low, as the driver.
  GpioOut (TLMM_SOUTH, 3, TRUE);
  GpioOut (TLMM_NORTH, 110, FALSE);
  GpioOut (TLMM_NORTH, 109, FALSE);
  gBS->Stall (100000);
  GpioOut (TLMM_NORTH, 111, TRUE);
  GpioOut (TLMM_NORTH, 109, TRUE);
  gBS->Stall (100000);
  // UART pins 50/51: qup16, 16 mA, pull-up.
  W (TLMM_SOUTH + 50 * 0x1000u, 0x1c7);
  W (TLMM_SOUTH + 51 * 0x1000u, 0x1c7);

  // geni_se_init in FIFO mode.
  W (SE + GSI_EVENT_EN, 0);
  W (SE + M_IRQ_CLEAR, 0xffffffffu);
  W (SE + S_IRQ_CLEAR, 0xffffffffu);
  W (SE + DMA_TX_IRQ_CLR, 0xffffffffu);
  W (SE + DMA_RX_IRQ_CLR, 0xffffffffu);
  W (SE + GENI_CGC_CTRL, R (SE + GENI_CGC_CTRL) | 0x7f);
  W (SE + DMA_GENERAL_CFG, R (SE + DMA_GENERAL_CFG) | 0xf);
  W (SE + GENI_OUTPUT_CTRL, 0x7f);
  W (SE + GENI_FORCE_DEFAULT, 1);
  W (SE + SE_IRQ_EN, R (SE + SE_IRQ_EN) | 0xf);
  W (SE + DMA_MODE_EN, R (SE + DMA_MODE_EN) & ~BIT0);
  // TX a byte per FIFO word; RX four, LSB first, for 64 bytes of FIFO.
  W (SE + TX_PACKING_0, 0xf);
  W (SE + TX_PACKING_1, 0);
  W (SE + RX_PACKING_0, 0x4380e);
  W (SE + RX_PACKING_1, 0xc3e0e);
  // 115200 = 7.3728 MHz / 4 / 16, 8N1, no flow control.
  W (SE + GENI_CLK_M, (4u << 4) | 1);
  W (SE + GENI_CLK_S, (4u << 4) | 1);
  W (SE + UART_TX_TRANS_CFG, 2);
  W (SE + UART_TX_WORD_LEN, 8);
  W (SE + UART_TX_STOP_LEN, 0);
  W (SE + UART_TX_PARITY, 0);
  W (SE + UART_RX_TRANS_CFG, 0);
  W (SE + UART_RX_WORD_LEN, 8);
  W (SE + UART_RX_PARITY, 0);
  W (SE + UART_RX_STALE, 0xa0);
  StartRead ();

  for (UINTN I = 0; I < ARRAY_SIZE (Frames); I++) {
    if (!Send (Frames[I].P, Frames[I].N)) {
      DpProbeMark ("mcu: frame %u not sent (tx %08x m %08x)\n", (UINT32)I,
                   R (SE + TX_FIFO_STATUS), R (SE + M_IRQ_STATUS));
      return;
    }
    gBS->Stall (100000);
  }
  mUp = TRUE;
  (VOID)McuKeysHeld ();
  DpProbeMark ("mcu: up, rx %08x keys %04x\n", R (SE + RX_FIFO_STATUS), mHeld);
}

// Takes one byte into the frame being assembled; a whole good command 2
// frame updates mHeld.
STATIC VOID
Take (IN UINT8 Byte)
{
  STATIC CONST UINT8 Sync[4] = {0xa5, 0xd3, 0x5a, 0x3d};
  UINT32             Len;
  UINT8              Sum;

  if (mHave < 4 && Byte != Sync[mHave]) {
    mHave = Byte == Sync[0];
    mFrame[0] = Byte;
    return;
  }
  mFrame[mHave++] = Byte;
  if (mHave < 8) {
    return;
  }
  Len = mFrame[6] | ((UINT32)mFrame[7] << 8);
  if (8 + Len + 1 > FRAME_MAX) {
    mHave = 0;
    return;
  }
  if (mHave < 8 + Len + 1) {
    return;
  }
  Sum = mFrame[4];
  for (UINT32 I = 5; I < mHave - 1; I++) {
    Sum ^= mFrame[I];
  }
  if (Sum == mFrame[mHave - 1] && mFrame[5] == 2 && Len >= 2) {
    mHeld = mFrame[8] | ((UINT32)mFrame[9] << 8);
  }
  mHave = 0;
}

// The MCU streams frames faster than the menu polls, so the FIFO is
// usually full: what it holds is the bytes right after the last drain,
// and a frame cut there must not be glued to them.
UINT32
McuKeysHeld (VOID)
{
  UINT32 St;
  UINT32 Words;

  if (!mUp) {
    return 0;
  }
  St    = R (SE + RX_FIFO_STATUS);
  Words = St & 0x1ffffff;
  if (Words >= 16) {
    mHave = 0;
  }
  for (UINT32 I = 0; I < Words; I++) {
    UINT32 V = R (SE + RX_FIFO);
    UINT32 N = 4;

    // A partial last word flushed by the stale timer.
    if (I + 1 == Words && (St & BIT31) && ((St >> 28) & 7)) {
      N = (St >> 28) & 7;
    }
    for (UINT32 B = 0; B < N; B++) {
      Take ((UINT8)(V >> (8 * B)));
    }
  }
  if (!(R (SE + SE_GENI_STATUS) & BIT12)) {
    StartRead ();  // the read ended (overrun): start another
  }
  return mHeld;
}
