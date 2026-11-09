/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
  DpPd - USB Power Delivery on the PM8150B PD PHY, polled, as source and
  DFP: power contract, then DisplayPort alternate mode entry. Register use
  follows Linux qcom_pmic_typec_pdphy.c; the DP steps follow
  drivers/usb/typec/altmodes/displayport.c.
*/

#include "Efi/DpPd.h"

#include "Efi/Efi.h"

// PD PHY registers, offsets from DP_PDPHY_BASE.
#define PD_MSG_CONFIG   0x40
#define PD_TX_SIZE      0x42
#define PD_TX_CONTROL   0x44
#define PD_EN_CONTROL   0x46
#define PD_RX_SIZE      0x48
#define PD_RX_STATUS    0x4A
#define PD_RX_ACK       0x4B
#define PD_FRAME_FILTER 0x4C
#define PD_TX_HDR       0x60
#define PD_TX_DATA      0x62
#define PD_RX_BUFFER    0x80
#define PD_BIST_MODE    0x4E
// Type-C VCONN control, as an offset from the PD PHY base.
#define TC_VCONN        (0x1546 - 0x1700)
#define TC_VCONN_VALUE  BIT1
#define TC_VCONN_ORIENT BIT2
// The peripheral's interrupt block (standard SPMI PMIC layout).
#define PD_INT_RT_STS      0x10
#define PD_INT_SET_TYPE    0x11
#define PD_INT_POL_HIGH    0x12
#define PD_INT_POL_LOW     0x13
#define PD_INT_LATCHED_CLR 0x14
#define PD_INT_EN_SET      0x15
#define PD_INT_EN_CLR      0x16
#define PD_INT_LATCHED_STS 0x18
#define PD_IRQ_MSG_TX      BIT2
#define PD_IRQ_MSG_RX      BIT3
#define PD_IRQ_TX_FAIL     BIT4
#define PD_IRQ_TX_DISCARD  BIT5
// MSG_CONFIG: PD 2.0, power role source, data role DFP.
#define PD_MSG_CONFIG_SRC_DFP 0x0D
#define PD_TX_SEND_MSG        BIT0
#define PD_TX_RETRY_PD20      (3u << 5)
#define PD_FILTER_SOP_HRESET  0x21

// PD message types.
#define PD_CTRL_GOODCRC     1
#define PD_CTRL_ACCEPT      3
#define PD_CTRL_PS_RDY      6
#define PD_DATA_SOURCE_CAP  1
#define PD_DATA_REQUEST     2
#define PD_DATA_VDM         15

// Structured VDM fields (Linux pd_vdo.h, typec_dp.h).
#define VDM_SVID_PD    0xFF00u
#define VDM_SVID_DP    0xFF01u
#define VDM_STRUCTURED BIT15
#define VDM_CMDT_REQ   0u
#define VDM_CMDT_ACK   1u
#define VDM_DISC_IDENT 1u
#define VDM_DISC_SVID  2u
#define VDM_DISC_MODES 3u
#define VDM_ENTER_MODE 4u
#define VDM_ATTENTION  6u
#define VDM_DP_STATUS  0x10u
#define VDM_DP_CONFIG  0x11u

#define DP_PIN_C BIT2
#define DP_PIN_D BIT3
#define DP_PIN_E BIT4

STATIC CONST DP_PD_IO *mIo;
STATIC UINT32          mBadRx;
STATIC UINT32          mTxId;
STATIC UINT32          mRev = 1;  // header spec revision: 1 = PD 2.0, 2 = PD 3.0

#define RD(Off)     mIo->Read (mIo->Ctx, DP_PDPHY_BASE + (Off))
#define WR(Off, V)  mIo->Write (mIo->Ctx, DP_PDPHY_BASE + (Off), (UINT8)(V))
#define SAY(...)    mIo->Say (mIo->Ctx, __VA_ARGS__)

STATIC UINT16
Header (
  IN UINT32 Type,
  IN UINT32 Count
)
{
  // Type, data role DFP (5), spec rev 2.0 (7:6 = 01), power role source
  // (8), message ID (11:9), data object count (14:12).
  return (UINT16)(Type | BIT5 | ((mRev & 3) << 6) | BIT8 | ((mTxId & 7) << 9) |
                  ((Count & 7) << 12));
}

// Sends one message and waits for its GoodCRC (the PHY sends and checks
// GoodCRC and retries itself). TRUE when it went through.
STATIC BOOLEAN
Send (
  IN UINT32        Type,
  IN UINT32        Count,
  IN CONST UINT32 *Objects OPTIONAL
)
{
  UINT16 Hdr = Header (Type, Count);

  WR (PD_INT_LATCHED_CLR, 0xFF);
  WR (PD_TX_CONTROL, 0);
  RD (PD_TX_CONTROL);
  WR (PD_TX_HDR, Hdr & 0xFF);
  WR (PD_TX_HDR + 1, Hdr >> 8);
  for (UINT32 I = 0; I < Count; I++) {
    for (UINT32 B = 0; B < 4; B++) {
      WR (PD_TX_DATA + I * 4 + B, Objects[I] >> (8 * B));
    }
  }
  WR (PD_TX_SIZE, 2 + Count * 4 - 1);
  WR (PD_TX_CONTROL, 0);
  RD (PD_TX_CONTROL);
  WR (PD_TX_CONTROL, PD_TX_SEND_MSG | (mRev == 2 ? (2u << 5) : PD_TX_RETRY_PD20));
  for (UINT32 T = 0; T < 50; T++) {
    UINT16 Sts = RD (PD_INT_LATCHED_STS);

    if (Sts != 0xFFFF && (Sts & (PD_IRQ_MSG_TX | PD_IRQ_TX_FAIL | PD_IRQ_TX_DISCARD))) {
      BOOLEAN Ok = (Sts & PD_IRQ_MSG_TX) != 0;

      SAY ("tx t%u n%u id%u %s sts=%02x rt=%02x t=%ums\n", Type, Count,
           mTxId & 7,
           Ok ? "ok" : ((Sts & PD_IRQ_TX_FAIL) ? "FAIL" : "DISCARD"),
           (UINT32)Sts, (UINT32)RD (PD_INT_RT_STS), T);
      if (Ok) {
        mTxId++;
      }
      return Ok;
    }
    gBS->Stall (1000);
  }
  SAY ("tx t%u n%u: no result\n", Type, Count);
  return FALSE;
}

// Waits up to Ms for a message. Objects gets up to 7 data objects.
STATIC BOOLEAN
Receive (
  IN UINT32  Ms,
  OUT UINT16 *Hdr,
  OUT UINT32 Objects[7]
)
{
  for (UINT32 T = 0; T < Ms; T++) {
    UINT16 Sts = RD (PD_INT_LATCHED_STS);

    if (Sts != 0xFFFF && (Sts & PD_IRQ_MSG_RX)) {
      UINT16 Size = RD (PD_RX_SIZE);
      UINT8  Buf[30];
      UINT32 Count;

      WR (PD_INT_LATCHED_CLR, PD_IRQ_MSG_RX);
      if (Size == 0xFFFF || Size < 1 || Size > 29) {
        if (mBadRx++ < 4) {
          SAY ("rx: bad size %u sts=%02x rt=%02x rxsts=%02x ack=%02x\n",
               (UINT32)Size, (UINT32)Sts, (UINT32)RD (PD_INT_RT_STS),
               (UINT32)RD (PD_RX_STATUS), (UINT32)RD (PD_RX_ACK));
        }
        WR (PD_RX_ACK, 0);
        gBS->Stall (1000);
        continue;
      }
      Size += 1;
      for (UINT32 B = 0; B < Size; B++) {
        Buf[B] = (UINT8)RD (PD_RX_BUFFER + B);
      }
      WR (PD_RX_ACK, 0);
      *Hdr  = (UINT16)(Buf[0] | (Buf[1] << 8));
      Count = (*Hdr >> 12) & 7;
      for (UINT32 I = 0; I < Count && I < 7; I++) {
        Objects[I] = Buf[2 + I * 4] | (Buf[3 + I * 4] << 8) |
                     (Buf[4 + I * 4] << 16) | ((UINT32)Buf[5 + I * 4] << 24);
      }
      SAY ("rx hdr=%04x t%u n%u", (UINT32)*Hdr, (UINT32)(*Hdr & 0x1F), Count);
      for (UINT32 I = 0; I < Count && I < 7; I++) {
        SAY (" %08x", Objects[I]);
      }
      SAY ("\n");
      return TRUE;
    }
    gBS->Stall (1000);
  }
  return FALSE;
}

// Sends a structured VDM request and waits for its ACK. On ACK, Objects
// holds the response (Objects[0] is its VDM header) and *Count its size.
STATIC BOOLEAN
Vdm (
  IN UINT32        Svid,
  IN UINT32        Cmd,
  IN UINT32        Pos,
  IN CONST UINT32 *Vdo OPTIONAL,
  OUT UINT32       Objects[7],
  OUT UINT32      *Count
)
{
  UINT32 Out[2];
  UINT16 Hdr;

  Out[0] = (Svid << 16) | VDM_STRUCTURED | (mRev == 2 ? BIT13 : 0) | ((Pos & 7) << 8) |
           (VDM_CMDT_REQ << 6) | Cmd;
  if (Vdo) {
    Out[1] = *Vdo;
  }
  if (!Send (PD_DATA_VDM, Vdo ? 2 : 1, Out)) {
    return FALSE;
  }
  // Skip anything else the partner says until the answer comes.
  for (UINT32 Tries = 0; Tries < 5; Tries++) {
    if (!Receive (100, &Hdr, Objects)) {
      SAY ("vdm %x/%u: no answer\n", Svid, Cmd);
      return FALSE;
    }
    *Count = (Hdr >> 12) & 7;
    if ((Hdr & 0x1F) == PD_DATA_VDM && *Count && (Objects[0] & 0x1F) == Cmd &&
        (Objects[0] >> 16) == Svid) {
      UINT32 Type = (Objects[0] >> 6) & 3;

      SAY ("vdm %x/%u: %s\n", Svid, Cmd,
           Type == VDM_CMDT_ACK ? "ACK" : (Type == 2 ? "NAK" : "BUSY"));
      return Type == VDM_CMDT_ACK;
    }
  }
  return FALSE;
}

BOOLEAN
DpPdRun (
  IN CONST DP_PD_IO *Io,
  OUT UINT32        *PinAssign,
  OUT BOOLEAN       *Hpd
)
{
  // ROCKNIX's source PDO: 5 V 3 A, dual-role, USB comms, data swap.
  STATIC CONST UINT32 SrcCap = BIT29 | BIT26 | BIT25 | (100u << 10) | 300u;
  UINT32              Obj[7];
  UINT32              Count = 0;
  UINT16              Hdr;
  BOOLEAN             Sent  = FALSE;
  UINT32              Mode;
  UINT32              Pins;
  UINT32              Conf;
  UINT32              Status = 1;  // connected as DFP_D

  mIo        = Io;
  mTxId      = 0;
  mBadRx     = 0;
  *PinAssign = 0;
  *Hpd       = FALSE;

  SAY ("pd: phy start\n");
  WR (PD_MSG_CONFIG, PD_MSG_CONFIG_SRC_DFP);
  WR (PD_EN_CONTROL, 0);
  WR (PD_EN_CONTROL, 1);
  WR (PD_FRAME_FILTER, PD_FILTER_SOP_HRESET);
  // Edge-triggered, rising, as Linux's devicetree sets them; level
  // interrupts would stay latched while the condition lasts.
  WR (PD_INT_SET_TYPE, 0xFF);
  WR (PD_INT_POL_HIGH, 0xFF);
  WR (PD_INT_POL_LOW, 0);
  WR (PD_INT_EN_SET, PD_IRQ_MSG_TX | PD_IRQ_MSG_RX | PD_IRQ_TX_FAIL | PD_IRQ_TX_DISCARD);
  WR (PD_INT_LATCHED_CLR, 0xFF);
  WR (PD_RX_ACK, 0);

  WR (PD_BIST_MODE, 0);
  // As Linux's TCPM (ROCKNIX log on this console): PD 3.0 headers, a
  // Source_Capabilities every 150 ms from right after VBUS; the adapter
  // answers from ~330 ms. If it gave up anyway, cycle VBUS and start over.
  mRev = 2;
  for (UINT32 Round = 0; Round < 2 && !Sent; Round++) {
    if (Round) {
      SAY ("pd: VBUS off/on\n");
      mIo->Write (mIo->Ctx, 0x1140, 0);
      gBS->Stall (800 * 1000);
      mIo->Write (mIo->Ctx, 0x1140, 1);
      gBS->Stall (16 * 1000);
    }
    for (UINT32 Try = 0; Try < 20 && !Sent; Try++) {
      Sent = Send (PD_DATA_SOURCE_CAP, 1, &SrcCap);
      if (!Sent) {
        Receive (150, &Hdr, Obj);
      }
    }
  }
  if (!Sent) {
    SAY ("pd: no sink answered Source_Capabilities\n");
    goto Off;
  }
  if (!Receive (500, &Hdr, Obj) || (Hdr & 0x1F) != PD_DATA_REQUEST ||
      ((Hdr >> 12) & 7) == 0) {
    SAY ("pd: no Request\n");
    goto Off;
  }
  Send (PD_CTRL_ACCEPT, 0, NULL);
  gBS->Stall (30 * 1000);
  Send (PD_CTRL_PS_RDY, 0, NULL);
  SAY ("pd: contract 5V\n");

  // Discovery. Some adapters answer only after Discover Identity.
  Vdm (VDM_SVID_PD, VDM_DISC_IDENT, 0, NULL, Obj, &Count);
  if (!Vdm (VDM_SVID_PD, VDM_DISC_SVID, 0, NULL, Obj, &Count)) {
    goto Off;
  }
  if (!Vdm (VDM_SVID_DP, VDM_DISC_MODES, 0, NULL, Obj, &Count) || Count < 2) {
    goto Off;
  }
  Mode = Obj[1];
  // The UFP_D pins: bits 23:16 for a receptacle, 15:8 for a plug (adapter).
  Pins = (Mode & BIT6) ? ((Mode >> 16) & 0xFF) : ((Mode >> 8) & 0xFF);
  SAY ("dp: mode vdo %08x pins %02x\n", Mode, Pins);
  if (!Vdm (VDM_SVID_DP, VDM_ENTER_MODE, 1, NULL, Obj, &Count)) {
    goto Off;
  }
  if (Vdm (VDM_SVID_DP, VDM_DP_STATUS, 1, &Status, Obj, &Count) && Count >= 2) {
    Status = Obj[1];
    SAY ("dp: partner status %08x\n", Status);
  }
  // As Linux: DP-only (C, E) unless the partner prefers USB too (D).
  if ((Status & BIT4) && (Pins & DP_PIN_D)) {
    Pins = DP_PIN_D;
  } else if (Pins & DP_PIN_C) {
    Pins = DP_PIN_C;
  } else if (Pins & DP_PIN_E) {
    Pins = DP_PIN_E;
  } else if (Pins & DP_PIN_D) {
    Pins = DP_PIN_D;
  } else {
    SAY ("dp: no usable pin assignment\n");
    goto Off;
  }
  // DP 1.3 signalling, UFP_U as UFP_D, the chosen pins.
  Conf = (1u << 2) | BIT1 | (Pins << 8);
  if (!Vdm (VDM_SVID_DP, VDM_DP_CONFIG, 1, &Conf, Obj, &Count)) {
    goto Off;
  }
  *PinAssign = Pins;
  *Hpd       = (Status & BIT7) != 0;
  // HPD often comes after configuration, in an Attention.
  for (UINT32 Wait = 0; Wait < 10 && !*Hpd; Wait++) {
    if (Receive (100, &Hdr, Obj) && (Hdr & 0x1F) == PD_DATA_VDM &&
        ((Hdr >> 12) & 7) >= 2 && (Obj[0] & 0x1F) == VDM_ATTENTION) {
      *Hpd = (Obj[1] & BIT7) != 0;
    }
  }
  SAY ("dp: configured pins %02x hpd %u\n", Pins, (UINT32)*Hpd);
  return TRUE;

Off:
  return FALSE;
}
