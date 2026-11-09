/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
  DpHw - SM8250 USB-C DisplayPort hardware after the alt mode is entered:
  SBU mux, USB3/DP QMP combo PHY in DP mode, DP controller (AUX, EDID,
  HBR2 x2 link training) and a DPU pipe scanning out 1080p60. Register
  use follows Linux phy-qcom-qmp-combo.c and drivers/gpu/drm/msm/dp, values
  as ROCKNIX leaves them on this console (2 lanes, normal orientation).
*/

#include "Efi/DpHw.h"

#include "Base/Memory.h"
#include "Efi/Efi.h"

#define GCC_BASE      0x00100000u
#define DISPCC_BASE   0x0af00000u
#define TLMM_NORTH    0x0f900000u
#define QMP_BASE      0x088e8000u
#define QMP_COM       (QMP_BASE + 0x0000u)
#define QMP_DP_SERDES (QMP_BASE + 0x2000u)
#define QMP_DP_TXA    (QMP_BASE + 0x2200u)
#define QMP_DP_TXB    (QMP_BASE + 0x2600u)
#define QMP_DP_PHY    (QMP_BASE + 0x2a00u)
#define DP_AHB        0x0ae90000u
#define DP_AUX        0x0ae90200u

// GCC (gcc-sm8250.c)
#define GCC_USB3_PHY_AUX_CBCR     0xf054u
#define GCC_USB3_PHY_COM_AUX_CBCR 0xf058u
#define GCC_USB3_PHY_PIPE_CBCR    0xf05cu
#define GCC_USB3_PHY_PRIM_BCR     0x50000u
#define GCC_USB3_DP_PHY_PRIM_BCR  0x50008u
// DISPCC (dispcc-sm8250.c)
#define DISPCC_MDSS_AHB_CBCR    0x2080u
#define DISPCC_DP_AUX_CBCR      0x2054u
#define DISPCC_DP_AUX_RCG_CMD   0x21dcu
#define DISPCC_DP_AUX_RCG_CFG   0x21e0u
#define DISPCC_MDSS_GDSCR       0x3000u
#define CBCR_OFF                BIT31

// QMP DP_COM (phy-qcom-qmp-dp-com-v3.h)
#define COM_PHY_MODE_CTRL  0x00u
#define COM_SW_RESET       0x04u
#define COM_POWER_DOWN     0x08u
#define COM_SWI_CTRL       0x0cu
#define COM_TYPEC_CTRL     0x10u
#define COM_RESET_OVRD     0x1cu
// DP PHY (phy-qcom-qmp-dp-phy.h, -v4.h)
#define DPPHY_CFG          0x010u
#define DPPHY_PD_CTL       0x018u
#define DPPHY_AUX_CFG0     0x020u
#define DPPHY_AUX_INT_MASK 0x054u
#define DPPHY_STATUS       0x0dcu
#define COM_BIAS_EN_CLKBUFLR_EN 0x044u

// DP controller (dp_reg.h)
#define DP_HW_VERSION      0x00u
#define DP_SW_RESET        0x10u
#define DP_INTR_STATUS     0x20u
#define DP_INTR_MASKS      0x24924920u  // every status's enable bit
#define DP_INTR_AUX_DONE   BIT3
#define DP_INTR_AUX_ERRORS (BIT6 | BIT9 | BIT12 | BIT15 | BIT18 | BIT21 | BIT27)
#define AUX_CTRL           0x30u
#define AUX_DATA           0x34u
#define AUX_TRANS_CTRL     0x38u
#define AUX_TIMEOUT        0x3cu
#define AUX_LIMITS         0x40u
#define AUX_STATUS         0x44u
#define AUX_PHY_INT_CLEAR  0x4cu
#define AUX_DATA_READ      BIT0
#define AUX_DATA_INDEX_WR  BIT31
#define AUX_TRANS_I2C      BIT8
#define AUX_TRANS_GO       BIT9

STATIC CONST DP_HW_IO *mIo;
#define SAY(...)  mIo->Say (__VA_ARGS__)
#define MARK(...) mIo->Mark (__VA_ARGS__)

STATIC UINT32
R (IN UINT32 Addr)
{
  UINT32 V = *(volatile UINT32 *)(UINTN)Addr;

  __asm__ volatile ("dsb sy" ::: "memory");
  return V;
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

// Turns a clock branch on and waits for it to report running.
STATIC BOOLEAN
Branch (
  IN UINT32       Addr,
  IN CONST CHAR8 *Name
)
{
  W (Addr, R (Addr) | BIT0);
  for (UINT32 T = 0; T < 200; T++) {
    if (!(R (Addr) & CBCR_OFF)) {
      return TRUE;
    }
    gBS->Stall (10);
  }
  SAY ("clk %s %08x still off\n", Name, R (Addr));
  return FALSE;
}

STATIC VOID
Snapshot (IN CONST CHAR8 *When)
{
  SAY ("hw %s: gcc aux=%08x com=%08x pipe=%08x bcr=%08x/%08x\n", When,
       R (GCC_BASE + GCC_USB3_PHY_AUX_CBCR), R (GCC_BASE + GCC_USB3_PHY_COM_AUX_CBCR),
       R (GCC_BASE + GCC_USB3_PHY_PIPE_CBCR), R (GCC_BASE + GCC_USB3_PHY_PRIM_BCR),
       R (GCC_BASE + GCC_USB3_DP_PHY_PRIM_BCR));
  SAY ("hw %s: dispcc gdsc=%08x ahb=%08x dpaux=%08x auxrcg=%08x/%08x tlmm8=%x/%x tlmm9=%x/%x\n",
       When, R (DISPCC_BASE + DISPCC_MDSS_GDSCR), R (DISPCC_BASE + DISPCC_MDSS_AHB_CBCR),
       R (DISPCC_BASE + DISPCC_DP_AUX_CBCR), R (DISPCC_BASE + DISPCC_DP_AUX_RCG_CMD),
       R (DISPCC_BASE + DISPCC_DP_AUX_RCG_CFG), R (TLMM_NORTH + 0x8000), R (TLMM_NORTH + 0x8004),
       R (TLMM_NORTH + 0x9000), R (TLMM_NORTH + 0x9004));
}

// One AUX transaction (Linux msm_dp_aux_write / _cmd_fifo_rx). Native or
// I2C-over-AUX; reads fill Buf, writes send it. Len 1..16.
STATIC BOOLEAN
AuxXfer (
  IN BOOLEAN    I2c,
  IN BOOLEAN    Read,
  IN UINT32     Addr,
  IN OUT UINT8 *Buf,
  IN UINT32     Len
)
{
  UINT8  Hdr[4];
  UINT32 Sts = 0;

  Hdr[0] = (UINT8)(((Addr >> 16) & 0xf) | (Read ? BIT4 : 0));
  Hdr[1] = (UINT8)(Addr >> 8);
  Hdr[2] = (UINT8)Addr;
  Hdr[3] = (UINT8)(Len - 1);
  for (UINT32 I = 0; I < 4 + (Read ? 0 : Len); I++) {
    UINT32 B = I < 4 ? Hdr[I] : Buf[I - 4];

    W (DP_AUX + AUX_DATA, ((B << 8) & 0xff00) | (I == 0 ? AUX_DATA_INDEX_WR : 0));
  }
  W (DP_AUX + AUX_TRANS_CTRL, 0);
  W (DP_AUX + AUX_PHY_INT_CLEAR, 0x1f);
  W (DP_AUX + AUX_PHY_INT_CLEAR, 0x9f);
  W (DP_AUX + AUX_PHY_INT_CLEAR, 0);
  W (DP_AHB + DP_INTR_STATUS, DP_INTR_MASKS | ((DP_INTR_AUX_DONE | DP_INTR_AUX_ERRORS) << 1));
  W (DP_AUX + AUX_TRANS_CTRL, AUX_TRANS_GO | (I2c ? AUX_TRANS_I2C : 0));
  for (UINT32 T = 0; T < 250; T++) {
    Sts = R (DP_AHB + DP_INTR_STATUS);
    if (Sts & (DP_INTR_AUX_DONE | DP_INTR_AUX_ERRORS)) {
      break;
    }
    gBS->Stall (1000);
  }
  W (DP_AHB + DP_INTR_STATUS, DP_INTR_MASKS | ((Sts & (DP_INTR_AUX_DONE | DP_INTR_AUX_ERRORS)) << 1));
  if (!(Sts & DP_INTR_AUX_DONE) || (Sts & DP_INTR_AUX_ERRORS)) {
    SAY ("aux %s %s %05x: intr=%08x auxsts=%08x\n", I2c ? "i2c" : "dpcd",
         Read ? "rd" : "wr", Addr, Sts, R (DP_AUX + AUX_STATUS));
    return FALSE;
  }
  if (Read) {
    W (DP_AUX + AUX_TRANS_CTRL, R (DP_AUX + AUX_TRANS_CTRL) & ~AUX_TRANS_GO);
    W (DP_AUX + AUX_DATA, AUX_DATA_INDEX_WR | AUX_DATA_READ);
    R (DP_AUX + AUX_DATA);  // the first is the reply's command byte
    for (UINT32 I = 0; I < Len; I++) {
      Buf[I] = (UINT8)(R (DP_AUX + AUX_DATA) >> 8);
    }
  }
  return TRUE;
}

STATIC VOID
Hex (
  IN CONST CHAR8 *What,
  IN CONST UINT8 *Buf,
  IN UINT32       Len
)
{
  SAY ("%s:", What);
  for (UINT32 I = 0; I < Len; I++) {
    SAY (" %02x", Buf[I]);
  }
  SAY ("\n");
}

STATIC BOOLEAN Video (VOID);

// Plug flipped (attached on CC2): the DP lanes leave on the other PHY half.
STATIC BOOLEAN mReverse;

BOOLEAN
DpHwRun (
  IN CONST DP_HW_IO *Io,
  IN BOOLEAN         Reverse
)
{
  UINT8  Buf[16];
  UINT32 V;

  mIo = Io;
  mReverse = Reverse;
  Snapshot ("before");
  if (R (DISPCC_BASE + DISPCC_MDSS_GDSCR) & BIT0) {
    // SW_COLLAPSE set: MDSS is off and its registers would hang the bus.
    SAY ("hw: MDSS power domain is off\n");
    return FALSE;
  }

  // SBU mux (gpio-sbu-mux): enable (active low) and orientation select.
  MARK ("hw: sbu mux\n");
  W (TLMM_NORTH + 0x9004, 0);
  W (TLMM_NORTH + 0x9000, 0x201);  // GPIO function, output, pull-down
  W (TLMM_NORTH + 0x8004, Reverse ? BIT1 : 0);
  W (TLMM_NORTH + 0x8000, 0x201);

  // PHY clocks (aux, com_aux from XO), then both PHY resets pulsed.
  MARK ("hw: phy clocks\n");
  if (!Branch (GCC_BASE + GCC_USB3_PHY_AUX_CBCR, "phy_aux") ||
      !Branch (GCC_BASE + GCC_USB3_PHY_COM_AUX_CBCR, "phy_com_aux")) {
    return FALSE;
  }
  W (GCC_BASE + GCC_USB3_PHY_PIPE_CBCR, R (GCC_BASE + GCC_USB3_PHY_PIPE_CBCR) | BIT0);
  W (GCC_BASE + GCC_USB3_DP_PHY_PRIM_BCR, 1);
  W (GCC_BASE + GCC_USB3_PHY_PRIM_BCR, 1);
  gBS->Stall (100);
  W (GCC_BASE + GCC_USB3_PHY_PRIM_BCR, 0);
  W (GCC_BASE + GCC_USB3_DP_PHY_PRIM_BCR, 0);
  gBS->Stall (100);

  // qmp_combo_com_init, DP only.
  MARK ("hw: qmp com\n");
  W (QMP_COM + COM_POWER_DOWN, R (QMP_COM + COM_POWER_DOWN) | BIT0);
  W (QMP_COM + COM_RESET_OVRD, R (QMP_COM + COM_RESET_OVRD) | 0x0f);
  W (QMP_COM + COM_TYPEC_CTRL, BIT1 | (Reverse ? BIT0 : 0));
  W (QMP_COM + COM_PHY_MODE_CTRL, BIT1);  // DP_MODE
  W (QMP_COM + COM_RESET_OVRD, R (QMP_COM + COM_RESET_OVRD) & ~0x0au);  // DP PHY out of reset
  W (QMP_COM + COM_SWI_CTRL, R (QMP_COM + COM_SWI_CTRL) & ~0x03u);
  W (QMP_COM + COM_SW_RESET, R (QMP_COM + COM_SW_RESET) & ~BIT0);
  SAY ("qmp com: mode=%x pd=%x typec=%x ovrd=%x\n", R (QMP_COM + COM_PHY_MODE_CTRL),
       R (QMP_COM + COM_POWER_DOWN), R (QMP_COM + COM_TYPEC_CTRL), R (QMP_COM + COM_RESET_OVRD));

  // qmp_v4_dp_aux_init.
  MARK ("hw: qmp aux\n");
  W (QMP_DP_PHY + DPPHY_PD_CTL, 0x01 | 0x02 | 0x04 | 0x20 | 0x40);
  W (QMP_DP_SERDES + COM_BIAS_EN_CLKBUFLR_EN, 0x17);
  {
    STATIC CONST UINT8 Aux[10] = {0x00, 0x13, 0xa4, 0x00, 0x0a, 0x26, 0x0a, 0x03, 0xb7, 0x03};

    for (UINT32 I = 0; I < 10; I++) {
      W (QMP_DP_PHY + DPPHY_AUX_CFG0 + 4 * I, Aux[I]);
    }
  }
  W (QMP_DP_PHY + DPPHY_AUX_INT_MASK, 0x1f);

  // DP controller: AUX clock (XO), reset, AUX on.
  MARK ("hw: dp aux clock\n");
  if (!Branch (DISPCC_BASE + DISPCC_MDSS_AHB_CBCR, "mdss_ahb")) {
    return FALSE;
  }
  W (DISPCC_BASE + DISPCC_DP_AUX_RCG_CFG, 0);  // XO, no divider
  W (DISPCC_BASE + DISPCC_DP_AUX_RCG_CMD, R (DISPCC_BASE + DISPCC_DP_AUX_RCG_CMD) | BIT0);
  for (UINT32 T = 0; T < 100 && (R (DISPCC_BASE + DISPCC_DP_AUX_RCG_CMD) & BIT0); T++) {
    gBS->Stall (10);
  }
  if (!Branch (DISPCC_BASE + DISPCC_DP_AUX_CBCR, "dp_aux")) {
    return FALSE;
  }
  MARK ("hw: dp controller\n");
  SAY ("dp: hw version %08x\n", R (DP_AHB + DP_HW_VERSION));
  W (DP_AHB + DP_SW_RESET, 1);
  gBS->Stall (1000);
  W (DP_AHB + DP_SW_RESET, 0);
  W (DP_AHB + DP_INTR_STATUS, DP_INTR_MASKS);
  V = R (DP_AUX + AUX_CTRL);
  W (DP_AUX + AUX_CTRL, V | BIT1);
  gBS->Stall (1000);
  W (DP_AUX + AUX_CTRL, V & ~BIT1);
  W (DP_AUX + AUX_TIMEOUT, 0xffff);
  W (DP_AUX + AUX_LIMITS, 0xffff);
  W (DP_AUX + AUX_CTRL, (V & ~BIT1) | BIT0);
  Snapshot ("after");

  // The sink's capabilities and the start of its EDID.
  MARK ("hw: aux read\n");
  if (!AuxXfer (FALSE, TRUE, 0x0000, Buf, 16)) {
    // As Linux's calibrate step: another AUX_CFG1 and try again.
    STATIC CONST UINT8 Cfg1[] = {0x20, 0x13, 0x23, 0x1d};
    BOOLEAN            Ok     = FALSE;

    for (UINT32 I = 0; I < 4 && !Ok; I++) {
      W (QMP_DP_PHY + DPPHY_AUX_CFG0 + 4, Cfg1[I]);
      Ok = AuxXfer (FALSE, TRUE, 0x0000, Buf, 16);
    }
    if (!Ok) {
      return FALSE;
    }
  }
  Hex ("dpcd 0000", Buf, 16);
  Buf[0] = 0;
  if (AuxXfer (TRUE, FALSE, 0x50, Buf, 1) && AuxXfer (TRUE, TRUE, 0x50, Buf, 16)) {
    Hex ("edid 00", Buf, 16);
  }
  return Video ();
}

// ---- Main link: PHY at HBR2 x2, clocks, training, test pattern ----------

#define DP_LINK          0x0ae90400u
#define DP_P0            0x0ae91000u
#define LINK_MAINLINK    0x00u
#define LINK_STATE       0x04u
#define LINK_CONFIG      0x08u
#define LINK_MVID        0x10u
#define LINK_NVID        0x18u
#define LINK_TOTAL       0x1cu
#define LINK_START       0x20u
#define LINK_SYNC        0x24u
#define LINK_ACTIVE      0x28u
#define LINK_MISC        0x2cu
#define LINK_VB          0x30u
#define LINK_VB2         0x34u
#define LINK_LANEMAP     0x38u
#define LINK_READY       0x40u
#define LINK_TU          0x4cu
#define P0_BIST          0x00u
#define P0_TE            0x10u
#define P0_INTF_CONFIG   0x14u
#define P0_HSYNC_CTL     0x18u
#define P0_VPERIOD_F0    0x1cu
#define P0_VPULSE_F0     0x24u
#define P0_DISP_VSTART   0x2cu
#define P0_DISP_VEND     0x34u
#define P0_DISP_HCTL     0x4cu
#define P0_TPG_MAIN      0x60u
#define P0_TPG_CONFIG    0x64u
#define P0_DSC_DTO       0x7cu

#define DISPCC_DP_LINK_CBCR      0x2040u
#define DISPCC_DP_LINK_INTF_CBCR 0x2044u
#define DISPCC_DP_PIXEL_CBCR     0x204cu
#define DISPCC_DP_LINK_RCG       0x2178u
#define DISPCC_DP_LINK_DIV       0x2190u
#define DISPCC_DP_PIXEL_RCG      0x21acu

typedef struct { UINT16 Off; UINT8 Val; } QMP_INIT;

// qmp_v4_dp_serdes_tbl + _hbr2, qmp_v4_dp_tx_tbl (phy-qcom-qmp-combo.c).
STATIC CONST QMP_INIT mSerdes[] = {
  {0x184, 0x05}, {0x094, 0x3b}, {0x04c, 0x02}, {0x048, 0x0c}, {0x050, 0x06},
  {0x154, 0x30}, {0x058, 0x0f}, {0x084, 0x36}, {0x07c, 0x16}, {0x074, 0x06},
  {0x17c, 0x02}, {0x0ec, 0x3f}, {0x0f0, 0x00}, {0x10c, 0x00}, {0x0cc, 0x00},
  {0x00c, 0x0a}, {0x168, 0x0a}, {0x108, 0x00}, {0x044, 0x17}, {0x174, 0x1f},
  // HBR2
  {0x158, 0x01}, {0x0bc, 0x8c}, {0x0d0, 0x00}, {0x0d4, 0x0a}, {0x0ac, 0x1f},
  {0x0b0, 0x1c}, {0x0a4, 0x08},
};
STATIC CONST QMP_INIT mTx[] = {
  {0x0e8, 0x40}, {0x020, 0x30}, {0x02c, 0x3b}, {0x008, 0x0f}, {0x01c, 0x03},
  {0x0b8, 0x0f}, {0x060, 0x00}, {0x0bc, 0x00}, {0x03c, 0x11}, {0x040, 0x11},
  {0x024, 0x04}, {0x05c, 0x0a}, {0x014, 0x2a}, {0x00c, 0x20},
};
// qmp_dp_v3_voltage_swing_hbr3_hbr2 / pre_emphasis_hbr3_hbr2
STATIC CONST UINT8 mSwing[4][4] = {
  {0x02, 0x12, 0x16, 0x1a}, {0x09, 0x19, 0x1f, 0xff}, {0x10, 0x1f, 0xff, 0xff}, {0x1f, 0xff, 0xff, 0xff}
};
STATIC CONST UINT8 mPre[4][4] = {
  {0x00, 0x0c, 0x15, 0x1a}, {0x02, 0x0e, 0x16, 0xff}, {0x02, 0x11, 0xff, 0xff}, {0x04, 0xff, 0xff, 0xff}
};

#define TX_DRV_LVL   0x014u
#define TX_EMP_POST1 0x00cu
#define TX_BIAS_EN   0x054u
#define TX_HIGHZ     0x058u
#define TX_POL_INV   0x05cu

STATIC BOOLEAN
Poll (
  IN UINT32       Addr,
  IN UINT32       Bit,
  IN CONST CHAR8 *What
)
{
  for (UINT32 T = 0; T < 200; T++) {
    if (R (Addr) & Bit) {
      return TRUE;
    }
    gBS->Stall (50);
  }
  SAY ("timeout %s (%08x)\n", What, R (Addr));
  return FALSE;
}

STATIC BOOLEAN
Swing (
  IN UINT32 V,
  IN UINT32 P
)
{
  UINT8 Drv = mSwing[V][P];
  UINT8 Emp = mPre[V][P];

  if (Drv == 0xff || Emp == 0xff) {
    return FALSE;
  }
  W (QMP_DP_TXA + TX_DRV_LVL, Drv | 0x20);
  W (QMP_DP_TXA + TX_EMP_POST1, Emp | 0x20);
  W (QMP_DP_TXB + TX_DRV_LVL, Drv | 0x20);
  W (QMP_DP_TXB + TX_EMP_POST1, Emp | 0x20);
  return TRUE;
}

// qmp_combo_dp_power_on at HBR2, 2 lanes, either orientation.
STATIC BOOLEAN
PhyPowerOn (VOID)
{
  for (UINTN I = 0; I < ARRAY_SIZE (mSerdes); I++) {
    W (QMP_DP_SERDES + mSerdes[I].Off, mSerdes[I].Val);
  }
  for (UINTN I = 0; I < ARRAY_SIZE (mTx); I++) {
    W (QMP_DP_TXA + mTx[I].Off, mTx[I].Val);
    W (QMP_DP_TXB + mTx[I].Off, mTx[I].Val);
  }
  W (QMP_DP_TXA + TX_DRV_LVL, 0x27);
  W (QMP_DP_TXB + TX_DRV_LVL, 0x27);
  W (QMP_DP_TXA + TX_EMP_POST1, 0x20);
  W (QMP_DP_TXB + TX_EMP_POST1, 0x20);
  Swing (0, 0);
  // qmp_v456_configure_dp_phy
  W (QMP_DP_PHY + 0x014, 0x0f);              // CFG_1
  // qmp_combo_configure_dp_mode: power down the unused lane pair.
  W (QMP_DP_PHY + DPPHY_PD_CTL, mReverse ? 0x6d : 0x75);
  W (QMP_DP_PHY + 0x01c, mReverse ? 0x4c : 0x5c);  // MODE
  W (QMP_DP_PHY + 0x024, 0x13);
  W (QMP_DP_PHY + 0x028, 0xa4);
  W (QMP_DP_PHY + 0x078, 0x05);
  W (QMP_DP_PHY + 0x09c, 0x05);
  W (QMP_DP_PHY + 0x070, 0x02);              // VCO_DIV for HBR2
  W (QMP_DP_PHY + DPPHY_CFG, 0x01);
  W (QMP_DP_PHY + DPPHY_CFG, 0x05);
  W (QMP_DP_PHY + DPPHY_CFG, 0x01);
  W (QMP_DP_PHY + DPPHY_CFG, 0x09);
  W (QMP_DP_SERDES + 0x09c, 0x20);           // RESETSM_CNTRL
  if (!Poll (QMP_DP_SERDES + 0x178, BIT0, "c_ready") ||
      !Poll (QMP_DP_SERDES + 0x140, BIT0, "cmn bit0") ||
      !Poll (QMP_DP_SERDES + 0x140, BIT1, "cmn bit1")) {
    return FALSE;
  }
  W (QMP_DP_PHY + DPPHY_CFG, 0x19);
  if (!Poll (QMP_DP_PHY + DPPHY_STATUS, BIT0, "phy status0") ||
      !Poll (QMP_DP_PHY + DPPHY_STATUS, BIT1, "phy status1")) {
    return FALSE;
  }
  // qmp_v4_configure_dp_phy, 2 lanes
  W (QMP_DP_TXA + TX_HIGHZ, 0x10);
  W (QMP_DP_TXA + TX_BIAS_EN, mReverse ? 0x3f : 0x15);
  W (QMP_DP_TXB + TX_HIGHZ, 0x10);
  W (QMP_DP_TXB + TX_BIAS_EN, mReverse ? 0x15 : 0x3f);
  W (QMP_DP_PHY + DPPHY_CFG, 0x18);
  gBS->Stall (2000);
  W (QMP_DP_PHY + DPPHY_CFG, 0x19);
  if (!Poll (QMP_DP_PHY + DPPHY_STATUS, BIT1, "phy status1 again")) {
    return FALSE;
  }
  W (QMP_DP_TXA + TX_POL_INV, 0x0a);
  W (QMP_DP_TXB + TX_POL_INV, 0x0a);
  W (QMP_DP_TXA + TX_DRV_LVL, 0x27);
  W (QMP_DP_TXB + TX_DRV_LVL, 0x27);
  W (QMP_DP_TXA + TX_EMP_POST1, 0x20);
  W (QMP_DP_TXB + TX_EMP_POST1, 0x20);
  return Swing (0, 0);
}

STATIC BOOLEAN
RcgUpdate (
  IN UINT32       Cmd,
  IN CONST CHAR8 *Name
)
{
  W (Cmd, R (Cmd) | BIT0);
  for (UINT32 T = 0; T < 500; T++) {
    if (!(R (Cmd) & BIT0)) {
      return TRUE;
    }
    gBS->Stall (10);
  }
  SAY ("rcg %s update stuck %08x\n", Name, R (Cmd));
  return FALSE;
}

// Link 540 MHz from the PHY, link_intf /4, pixel 148.5 MHz = 1350 * 11/100.
STATIC BOOLEAN
LinkClocks (VOID)
{
  W (DISPCC_BASE + DISPCC_DP_LINK_RCG + 4, 0x101);
  if (!RcgUpdate (DISPCC_BASE + DISPCC_DP_LINK_RCG, "link")) {
    return FALSE;
  }
  W (DISPCC_BASE + DISPCC_DP_LINK_DIV, 3);
  if (!Branch (DISPCC_BASE + DISPCC_DP_LINK_CBCR, "dp_link") ||
      !Branch (DISPCC_BASE + DISPCC_DP_LINK_INTF_CBCR, "dp_link_intf")) {
    return FALSE;
  }
  W (DISPCC_BASE + DISPCC_DP_PIXEL_RCG + 4, 0x2200);    // VCO div source, dual-edge MND
  W (DISPCC_BASE + DISPCC_DP_PIXEL_RCG + 8, 11);        // M
  W (DISPCC_BASE + DISPCC_DP_PIXEL_RCG + 12, (~(100u - 11u)) & 0xffff);  // N
  W (DISPCC_BASE + DISPCC_DP_PIXEL_RCG + 16, (~100u) & 0xffff);          // D
  if (!RcgUpdate (DISPCC_BASE + DISPCC_DP_PIXEL_RCG, "pixel")) {
    return FALSE;
  }
  return Branch (DISPCC_BASE + DISPCC_DP_PIXEL_CBCR, "dp_pixel");
}

STATIC BOOLEAN
DpcdW (
  IN UINT32 Addr,
  IN UINT8  Val
)
{
  return AuxXfer (FALSE, FALSE, Addr, &Val, 1);
}

STATIC BOOLEAN
SetPattern (IN UINT32 Bit)
{
  W (DP_LINK + LINK_STATE, Bit ? (1u << (Bit - 1)) : 0);
  return Bit == 0 || Poll (DP_LINK + LINK_READY, (1u << (Bit - 1)) << 3, "pattern ready");
}

// Linux msm_dp_ctrl_link_train_1 / _2 for 2 lanes.
STATIC BOOLEAN
Train (VOID)
{
  UINT8  St[6];
  UINT32 V = 0;
  UINT32 P = 0;
  UINT8  Lanes[2];

  W (DP_LINK + LINK_STATE, 0);
  if (!SetPattern (1) || !DpcdW (0x102, 0x21)) {
    return FALSE;
  }
  for (UINT32 Try = 0; Try < 10; Try++) {
    Swing (V, P);
    Lanes[0] = Lanes[1] = (UINT8)(V | (P << 3) | (V == 3 ? BIT2 : 0) | (P == 3 ? BIT5 : 0));
    AuxXfer (FALSE, FALSE, 0x103, Lanes, 2);
    gBS->Stall (500);
    if (!AuxXfer (FALSE, TRUE, 0x202, St, 6)) {
      return FALSE;
    }
    SAY ("cr try %u v%u p%u: %02x %02x %02x %02x %02x %02x\n", Try, V, P, St[0], St[1],
         St[2], St[3], St[4], St[5]);
    if ((St[0] & 0x11) == 0x11) {
      break;
    }
    {
      UINT32 Nv = MAX (St[4] & 3, (St[4] >> 4) & 3);
      UINT32 Np = MAX ((St[4] >> 2) & 3, (St[4] >> 6) & 3);

      if (Nv + Np > 3) {
        Np = 3 - Nv;
      }
      V = Nv;
      P = Np;
    }
    if (Try == 9) {
      SAY ("train: clock recovery failed\n");
      return FALSE;
    }
  }
  W (DP_LINK + LINK_STATE, 0);
  if (!SetPattern (2) || !DpcdW (0x102, 0x22)) {
    return FALSE;
  }
  for (UINT32 Try = 0; Try < 8; Try++) {
    gBS->Stall (500);
    if (!AuxXfer (FALSE, TRUE, 0x202, St, 6)) {
      return FALSE;
    }
    SAY ("eq try %u v%u p%u: %02x %02x %02x %02x %02x %02x\n", Try, V, P, St[0], St[1],
         St[2], St[3], St[4], St[5]);
    if ((St[0] & 0x77) == 0x77 && (St[2] & BIT0)) {
      W (DP_LINK + LINK_STATE, 0);
      DpcdW (0x102, 0);
      return TRUE;
    }
    {
      UINT32 Nv = MAX (St[4] & 3, (St[4] >> 4) & 3);
      UINT32 Np = MAX ((St[4] >> 2) & 3, (St[4] >> 6) & 3);

      if (Nv + Np > 3) {
        Np = 3 - Nv;
      }
      V = Nv;
      P = Np;
    }
    Swing (V, P);
    Lanes[0] = Lanes[1] = (UINT8)(V | (P << 3) | (V == 3 ? BIT2 : 0) | (P == 3 ? BIT5 : 0));
    AuxXfer (FALSE, FALSE, 0x103, Lanes, 2);
  }
  SAY ("train: channel equalization failed\n");
  W (DP_LINK + LINK_STATE, 0);
  DpcdW (0x102, 0);
  return FALSE;
}

// DPU (MDP) blocks, sm8250 catalog offsets from MDSS 0xae00000.
#define DPU_CTL(n)   (0x0ae02000u + 0x200u * (n))
#define DPU_VIG(n)   (0x0ae05000u + 0x2000u * (n))
#define DPU_DMA(n)   (0x0ae25000u + 0x2000u * (n))
#define DPU_LM(n)    (0x0ae45000u + 0x1000u * (n))
#define DPU_INTF(n)  (0x0ae6b000u + 0x800u * (n))
#define SCAN_W 1920u
#define SCAN_H 1080u

UINT32 *gDpScanout;  // 1920x1080 XRGB, what the monitor shows

// What XBL's panel pipeline holds, so ours stays off its blocks.
STATIC VOID
DpuSnapshot (VOID)
{
  for (UINT32 C = 0; C < 6; C++) {
    UINT32 B = DPU_CTL (C);
    SAY ("ctl%u: lm %x %x %x %x %x %x top %x intf %x\n", C, R (B), R (B + 4), R (B + 8),
         R (B + 0xc), R (B + 0x10), R (B + 0x24), R (B + 0x14), R (B + 0xf4));
  }
  for (UINT32 N = 0; N < 4; N++) {
    SAY ("vig%u %x@%x dma%u %x@%x\n", N, R (DPU_VIG (N)), R (DPU_VIG (N) + 0x14), N,
         R (DPU_DMA (N)), R (DPU_DMA (N) + 0x14));
  }
  SAY ("lm out %x %x %x %x %x %x\n", R (DPU_LM (0) + 4), R (DPU_LM (1) + 4),
       R (DPU_LM (2) + 4), R (DPU_LM (3) + 4), R (DPU_LM (4) + 4), R (DPU_LM (5) + 4));
  SAY ("intf1 te %x mux %x, intf0 te %x mux %x\n", R (DPU_INTF (1)),
       R (DPU_INTF (1) + 0x25c), R (DPU_INTF (0)), R (DPU_INTF (0) + 0x25c));
}

// Starts the monitor black; the menu's first frame fills it.
STATIC VOID
DrawScanout (VOID)
{
  ZeroMem (gDpScanout, SCAN_W * SCAN_H * 4);
  for (UINTN A = (UINTN)gDpScanout; A < (UINTN)gDpScanout + SCAN_W * SCAN_H * 4; A += 64) {
    __asm__ volatile ("dc cvac, %0" ::"r"(A) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// DMA1 -> LM2 -> PP2 -> INTF0 on CTL2, CEA 1080p60 at 148.5 MHz without
// widebus; the timing follows dpu_hw_intf_setup_timing_engine for a DP
// interface (front porches folded into the back porches).
STATIC BOOLEAN
Scanout (VOID)
{
  CONST UINT32 Ht = 2200, Hsw = 44, Hbp = 148 + 88, Vsw = 5, Vbp = 36 + 4;
  CONST UINT32 Hs = Hsw + Hbp, He = Hs + SCAN_W - 1, Vs = (Vsw + Vbp) * Ht + Hs;
  CONST UINT32 Sz = (SCAN_H << 16) | SCAN_W;
  EFI_PHYSICAL_ADDRESS Buf = 0xffffffffu;
  UINT32 Sspp = DPU_DMA (1), Lm = DPU_LM (2), Ctl = DPU_CTL (2), Intf = DPU_INTF (0);

  DpuSnapshot ();
  if (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData,
                          EFI_SIZE_TO_PAGES (SCAN_W * SCAN_H * 4), &Buf) != EFI_SUCCESS) {
    SAY ("dpu: no memory for the scanout\n");
    return FALSE;
  }
  gDpScanout = (UINT32 *)(UINTN)Buf;
  DrawScanout ();
  SAY ("dpu: scanout at %lx\n", (UINT64)Buf);

  // DP controller takes pixels from the DPU, not its own pattern generator.
  W (DP_P0 + P0_TE, 0);
  W (DP_P0 + P0_BIST, 0);
  W (DP_P0 + P0_TPG_MAIN, 0);
  W (DP_P0 + P0_INTF_CONFIG, 0);

  W (Sspp + 0x00, Sz);              // SRC_SIZE
  W (Sspp + 0x08, 0);               // SRC_XY
  W (Sspp + 0x0c, Sz);              // OUT_SIZE
  W (Sspp + 0x10, 0);               // OUT_XY
  W (Sspp + 0x14, (UINT32)Buf);     // SRC0_ADDR
  W (Sspp + 0x24, SCAN_W * 4);      // YSTRIDE0
  W (Sspp + 0x30, 0x000236ff);      // linear XRGB8888
  W (Sspp + 0x34, 0x03020001);      // unpack 0x00RRGGBB
  W (Sspp + 0x38, 0x80000000);      // pixel extension override
  W (Sspp + 0x44, 0);               // no UBWC
  W (Sspp + 0x48, 0x87);            // FETCH_CONFIG reset value
  W (Sspp + 0x60, 0xffff);          // danger LUT
  W (Sspp + 0x64, 0xff00);          // safe LUT
  W (Sspp + 0x74, 0x44556677);      // creq LUTs
  W (Sspp + 0x78, 0x00112233);
  W (Sspp + 0x100, 0);
  W (Sspp + 0x104, 0);
  W (Sspp + 0x108, Sz);
  W (Sspp + 0x110, 0);
  W (Sspp + 0x114, 0);
  W (Sspp + 0x118, Sz);
  W (Sspp + 0x120, 0);
  W (Sspp + 0x124, 0);
  W (Sspp + 0x128, Sz);

  W (Lm + 0x00, 2);                 // OP_MODE as Linux leaves it
  W (Lm + 0x04, Sz);                // OUT_SIZE
  W (Lm + 0x20, 0x100);             // stage 0: opaque
  W (Lm + 0x24, 0x00ff0000);

  for (UINT32 L = 0; L < 0x14; L += 4) {
    W (Ctl + L, 0);
    W (Ctl + 0x40 + L, 0);
    W (Ctl + 0x70 + L, 0);
  }
  W (Ctl + 0x08, BIT24 | (2u << 21));  // LM2: border + DMA1 at stage 0
  W (Ctl + 0x14, 0);                   // video mode
  W (Ctl + 0xe4, 0);                   // no merge_3d
  W (Ctl + 0xf4, BIT0);                // INTF0

  W (Intf + 0x00, 0);
  W (Intf + 0x08, (Ht << 16) | Hsw);
  W (Intf + 0x0c, 1125 * Ht);
  W (Intf + 0x14, Vsw * Ht);
  W (Intf + 0x1c, Vs);
  W (Intf + 0x24, 1125 * Ht - 1);
  W (Intf + 0x2c, Vs);
  W (Intf + 0x34, Vs + SCAN_H * Ht - 1);
  W (Intf + 0x3c, (He << 16) | Hs);
  W (Intf + 0x40, (He << 16) | Hs);
  W (Intf + 0x44, 0);
  W (Intf + 0x48, 0xff);            // underflow shows blue
  W (Intf + 0x4c, 0);
  W (Intf + 0x50, 0);
  W (Intf + 0xa8, 3);
  W (Intf + 0x04, R (Intf + 0x04) | BIT29 | BIT30);
  W (Intf + 0x90, 0x2100);          // 8 bpc RGB
  W (Intf + 0x60, BIT4);            // DATA_HCTL_EN, no widebus
  W (Intf + 0x64, (He << 16) | Hs);
  W (Intf + 0x25c, (R (Intf + 0x25c) & ~0xfu) | 2);  // fed by PP2

  W (Ctl + 0x110, BIT0);                           // INTF0 flush
  W (Ctl + 0x18, BIT31 | BIT17 | BIT12 | BIT8);    // intf, ctl, DMA1, LM2
  W (Intf + 0x00, 1);
  gBS->Stall (50 * 1000);
  SAY ("dpu: flush %x intf0 frames %x line %x status %x\n", R (Ctl + 0x18),
       R (Intf + 0xac), R (Intf + 0xb0), R (Intf + 0x26c));
  return TRUE;
}

STATIC BOOLEAN
Video (VOID)
{
  UINT8  Caps[4];
  UINT32 M;

  MARK ("hw: phy power on\n");
  if (!PhyPowerOn ()) {
    return FALSE;
  }
  MARK ("hw: link clocks\n");
  if (!LinkClocks ()) {
    return FALSE;
  }
  // Sink awake, 5.4 Gbps x2 with enhanced framing, no spread, 8b/10b.
  MARK ("hw: link setup\n");
  DpcdW (0x600, 1);
  Caps[0] = 0x14;
  Caps[1] = 0x82;
  AuxXfer (FALSE, FALSE, 0x100, Caps, 2);
  Caps[0] = 0x00;
  Caps[1] = 0x01;
  AuxXfer (FALSE, FALSE, 0x107, Caps, 2);
  // msm_dp_ctrl_mainlink_enable, configure_source_params
  M = R (DP_LINK + LINK_MAINLINK) & ~(BIT0 | BIT1);
  W (DP_LINK + LINK_MAINLINK, M);
  W (DP_LINK + LINK_MAINLINK, M | BIT1);
  W (DP_LINK + LINK_MAINLINK, M);
  W (DP_LINK + LINK_MAINLINK, M | BIT0 | BIT25);
  W (DP_LINK + LINK_LANEMAP, 0xe4);
  W (DP_LINK + LINK_CONFIG, 0x4157);
  MARK ("hw: training\n");
  // Equalization sometimes never locks at the same settings that lock on
  // another run (seen ~3 in 10 boots): train again from clock recovery.
  for (UINT32 Run = 0; !Train (); Run++) {
    if (Run == 3) {
      return FALSE;
    }
    SAY ("train: retry %u\n", Run + 1);
    gBS->Stall (20000);
  }
  MARK ("hw: video\n");
  W (DP_LINK + LINK_MISC, 0x21);
  W (DP_LINK + LINK_MVID, 0x4641);
  W (DP_LINK + LINK_NVID, 0xff78);
  W (DP_LINK + LINK_TOTAL, 0x04650898);
  W (DP_LINK + LINK_START, 0x002900c0);
  W (DP_LINK + LINK_SYNC, 0x0005002c);
  W (DP_LINK + LINK_ACTIVE, 0x04380780);
  W (DP_P0 + P0_DSC_DTO, 0);
  W (DP_LINK + LINK_VB, 0x0026000e);
  W (DP_LINK + LINK_TU, 0x1f);
  W (DP_LINK + LINK_VB2, 0x0041001b);
  if (!Scanout ()) {
    return FALSE;
  }
  W (DP_LINK + LINK_STATE, BIT7);  // SEND_VIDEO
  Poll (DP_LINK + LINK_READY, BIT0, "ready for video");
  SAY ("video: ready=%08x intr2=%08x mainlink=%08x\n", R (DP_LINK + LINK_READY),
       R (DP_AHB + 0x24), R (DP_LINK + LINK_MAINLINK));
  MARK ("hw: scanout on\n");
  return TRUE;
}

VOID
DpHwMirror (
  IN CONST UINT32 *Pixels,
  IN UINT32        Stride,
  IN UINT32        X,
  IN UINT32        Y,
  IN UINT32        W,
  IN UINT32        H
)
{
  // Panel pixel (px, py) shows at monitor (1919 - py, px), as DrawScanout.
  if (!gDpScanout || (UINT64)X + W > SCAN_H || (UINT64)Y + H > SCAN_W) {
    return;
  }
  for (UINT32 I = 0; I < W; I++) {
    UINT32 *Row = gDpScanout + (X + I) * SCAN_W + (SCAN_W - Y - H);

    for (UINT32 J = 0; J < H; J++) {
      Row[H - 1 - J] = Pixels[J * Stride + I];
    }
    for (UINTN A = (UINTN)Row & ~(UINTN)63; A < (UINTN)(Row + H); A += 64) {
      __asm__ volatile ("dc cvac, %0" ::"r"(A) : "memory");
    }
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}
