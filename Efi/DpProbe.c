/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
  DpProbe - USB-C DisplayPort bring-up for the menu. See DpProbe.h.
*/

#include "Efi/DpProbe.h"

#include "Base/Buffer.h"
#include "Base/Memory.h"
#include "Efi/Display.h"
#include "Efi/DpPd.h"
#include "Efi/DpHw.h"
#include "Efi/Efi.h"
#include "Efi/Uninstall.h"

#include "Base/Print.h"

// gQcomSPMIProtocolGuid, as in DisplayHandover.c.
STATIC EFI_GUID mSpmiGuid = {0xfa5f306b,
                             0xf47d,
                             0x4ac4,
                             {0xa4, 0x7d, 0x88, 0x2f, 0x82, 0x04, 0xec, 0x30}};

// EFI_QCOM_SPMI_PROTOCOL up to WriteLong. SM8250's XBL is the XF build
// (revision 0x10001): ReadLong (This, SlaveId, Priority, Address, Data,
// Len, BytesRead), returning 0 on success.
typedef struct {
  UINT64 Revision;
  UINTN  ReadLong;
  UINTN  WriteLong;
} DP_SPMI;
typedef UINT32 (EFIAPI *DP_SPMI_READ_XF) (
  IN DP_SPMI *This,
  IN UINT32   SlaveId,
  IN UINT32   Priority,
  IN UINT32   Address,
  OUT UINT8  *Data,
  IN UINT32   Len,
  OUT UINT32 *Read
);
#define DP_SPMI_REVISION_BUS_ID 0x10003u

// PM8150B: SPMI slave 2, Type-C block at 0x1500, PD PHY at 0x1700
// (Linux pm8150b.dtsi).
#define PM8150B_SID 2u
#define TYPEC_BASE  0x1500u
#define PDPHY_BASE  0x1700u

#define REPORT_SIZE 8192u
#define REPORT_MAGIC "OABLDP1\n"

STATIC CHAR8 mReport[REPORT_SIZE];
STATIC UINTN mUsed;

STATIC VOID
WriteReport (IN BOOLEAN Finish);

STATIC VOID
VAdd (
  IN CONST CHAR8 *Format,
  IN va_list      Args
)
{
  INT32 Len;

  if (mUsed + 1 >= REPORT_SIZE) {
    return;
  }
  Len = VSPrint (mReport + mUsed, REPORT_SIZE - mUsed, Format, Args);
  // VSPrint returns the untruncated length.
  if (Len > 0) {
    mUsed = MIN (mUsed + (UINTN)Len, REPORT_SIZE - 1);
  }
}

STATIC VOID
Add (IN CONST CHAR8 *Format, ...) __attribute__ ((format (printf, 1, 2)));

STATIC VOID
Add (IN CONST CHAR8 *Format, ...)
{
  va_list Args;

  va_start (Args, Format);
  VAdd (Format, Args);
  va_end (Args);
}

// Reads Len registers from Base on the PM8150B one at a time, so one bad
// register doesn't lose the rest. Unread bytes are shown as "--".
STATIC VOID
Dump (
  IN DP_SPMI     *Spmi,
  IN CONST CHAR8 *Name,
  IN UINT32       Base,
  IN UINT32       Len
)
{
  Add ("%s 0x%04x:", Name, (UINT32)Base);
  for (UINT32 Off = 0; Off < Len; Off++) {
    UINT8  Value = 0;
    UINT32 Read  = 0;
    UINT32 Rc    = ((DP_SPMI_READ_XF)Spmi->ReadLong) (
      Spmi, PM8150B_SID, 0, Base + Off, &Value, 1, &Read
    );

    if (Off % 16 == 0) {
      Add ("\n %02x:", (UINT32)Off);
    }
    if (Rc == 0 && Read == 1) {
      Add (" %02x", (UINT32)Value);
    } else {
      Add (" --");
    }
  }
  Add ("\n");
}

typedef UINT32 (EFIAPI *DP_SPMI_WRITE_XF) (
  IN DP_SPMI *This,
  IN UINT32   SlaveId,
  IN UINT32   Priority,
  IN UINT32   Address,
  IN UINT8   *Data,
  IN UINT32   Len
);

// One PM8150B register; 0xFFFF if the read failed.
STATIC UINT16
Rd (
  IN DP_SPMI *Spmi,
  IN UINT32   Address
)
{
  UINT8  Value = 0;
  UINT32 Read  = 0;

  if (((DP_SPMI_READ_XF)Spmi->ReadLong) (
        Spmi, PM8150B_SID, 0, Address, &Value, 1, &Read
      ) != 0 ||
      Read != 1) {
    return 0xFFFF;
  }
  return Value;
}

STATIC VOID
Wr (
  IN DP_SPMI *Spmi,
  IN UINT32   Address,
  IN UINT8    Value
)
{
  UINT32 Rc = ((DP_SPMI_WRITE_XF)Spmi->WriteLong) (
    Spmi, PM8150B_SID, 0, Address, &Value, 1
  );

  Add ("wr %04x=%02x rc=%u\n", Address, (UINT32)Value, Rc);
}

// Type-C registers (Linux qcom_pmic_typec_port.c), offsets from TYPEC_BASE.
#define TC_SNK_STATUS  0x06
#define TC_SRC_STATUS  0x08
#define TC_SM_STATUS   0x0A
#define TC_MISC_STATUS 0x0B
#define TC_MODE_CFG    0x44
#define TC_MISC_CC_ATTACHED  BIT0
#define TC_MISC_ORIENTATION  BIT1
#define TC_MISC_VBUS_DETECT  BIT5
#define TC_MISC_SNK_SRC_MODE BIT6
#define TC_SRC_RD_OPEN       BIT3
#define TC_SRC_RD_RA_VCONN   BIT2
#define TC_VCONN_CONTROL     0x46
#define TC_VCONN_ORIENTATION BIT2
#define TC_VCONN_VALUE       BIT1
#define TC_VCONN_SRC         BIT0
#define TC_MODE_DISABLE      0x01
#define TC_MODE_TRY_SNK      0x10
// VBUS boost (Linux qcom_usb_vbus-regulator.c), at 0x1100.
#define OTG_BASE    0x1100u
#define OTG_CMD     0x40
#define OTG_CFG     0x53
#define OTG_EN_SRC  BIT1

// Logs the Type-C status registers when they change, for up to Ms.
STATIC UINT16
Watch (
  IN DP_SPMI *Spmi,
  IN UINT32   Ms
)
{
  UINT32 Last = 0xFFFFFFFF;
  UINT16 Misc = 0;

  for (UINT32 T = 0; T <= Ms; T += 100) {
    UINT16 Snk = Rd (Spmi, TYPEC_BASE + TC_SNK_STATUS);
    UINT16 Src = Rd (Spmi, TYPEC_BASE + TC_SRC_STATUS);
    UINT16 Sm  = Rd (Spmi, TYPEC_BASE + TC_SM_STATUS);
    UINT32 Now;

    Misc = Rd (Spmi, TYPEC_BASE + TC_MISC_STATUS);
    Now  = ((UINT32)Snk << 24) ^ ((UINT32)Src << 16) ^ ((UINT32)Sm << 8) ^ Misc;
    if (Now != Last) {
      Add ("t=%u snk=%02x src=%02x sm=%02x misc=%02x\n", T, (UINT32)Snk,
           (UINT32)Src, (UINT32)Sm, (UINT32)Misc);
      Last = Now;
    }
    gBS->Stall (100 * 1000);
  }
  return Misc;
}

// Puts the port in dual-role (try-sink) as Linux does, waits for an
// attach, and if the far end is a sink with no VBUS, turns the 5 V boost on.
STATIC BOOLEAN
Attach (IN DP_SPMI *Spmi)
{
  UINT16 Misc;
  UINT16 Src;

  Add ("otg before: cmd=%02x cfg=%02x\n",
       (UINT32)Rd (Spmi, OTG_BASE + OTG_CMD), (UINT32)Rd (Spmi, OTG_BASE + OTG_CFG));
  Wr (Spmi, TYPEC_BASE + TC_MODE_CFG, TC_MODE_DISABLE);
  gBS->Stall (50 * 1000);
  Wr (Spmi, TYPEC_BASE + TC_MODE_CFG, TC_MODE_TRY_SNK);
  Misc = Watch (Spmi, 3000);
  if (Misc == 0xFFFF || !(Misc & TC_MISC_CC_ATTACHED)) {
    Add ("attach: nothing attached\n");
    return FALSE;
  }
  Add ("attach: cc%u, %s\n", (Misc & TC_MISC_ORIENTATION) ? 2u : 1u,
       (Misc & TC_MISC_SNK_SRC_MODE) ? "we are source" : "we are sink");
  Src = Rd (Spmi, TYPEC_BASE + TC_SRC_STATUS);
  Add ("attach: src=%02x\n", (UINT32)Src);
  // A sink (Rd/open), or a sink behind a powered adapter (Rd/Ra), and no
  // one else driving VBUS: we must source it.
  if ((Misc & TC_MISC_SNK_SRC_MODE) && !(Misc & TC_MISC_VBUS_DETECT) &&
      Src != 0xFFFF && (Src & (TC_SRC_RD_OPEN | TC_SRC_RD_RA_VCONN))) {
    UINT16 Cfg = Rd (Spmi, OTG_BASE + OTG_CFG);

    if (Cfg != 0xFFFF) {
      Wr (Spmi, OTG_BASE + OTG_CFG, (UINT8)(Cfg & ~OTG_EN_SRC));
    }
    Wr (Spmi, OTG_BASE + OTG_CMD, 1);
    // The adapter's Ra is on the CC we aren't using: VCONN goes there.
    if (Src & TC_SRC_RD_RA_VCONN) {
      UINT16 Vc = Rd (Spmi, TYPEC_BASE + TC_VCONN_CONTROL);

      if (Vc != 0xFFFF) {
        Vc &= ~(TC_VCONN_ORIENTATION | TC_VCONN_VALUE);
        Vc |= TC_VCONN_VALUE | TC_VCONN_SRC |
              ((Misc & TC_MISC_ORIENTATION) ? 0 : TC_VCONN_ORIENTATION);
        Wr (Spmi, TYPEC_BASE + TC_VCONN_CONTROL, (UINT8)Vc);
      }
    }
    // No waiting here: a sink gives up on PD if Source_Capabilities don't
    // start within ~0.5 s of VBUS (Linux sends the first one after 16 ms).
    return TRUE;
  }
  return FALSE;
}

// XBL's EFI_QCOM_PMIC_VREG_PROTOCOL (edk2-msm EFIPmicVreg.h): it goes
// through RPMh, which owns the PM8150 LDOs.
STATIC EFI_GUID mVregGuid = {0x22d38d3d, 0xe8b6, 0x4f8f,
                             {0x9c, 0x26, 0xbc, 0xeb, 0x07, 0xd6, 0xcb, 0x68}};
typedef struct {
  UINT32  PullDown;
  UINT32  SwMode;
  UINT32  PinCtrled;
  UINT32  SwEnable;
  BOOLEAN VregOk;
} DP_VREG_STATUS;
typedef struct {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *Control) (IN UINT32 Pmic, IN UINT32 Vreg, IN BOOLEAN Enable);
  EFI_STATUS (EFIAPI *SetLevel) (IN UINT32 Pmic, IN UINT32 Vreg, IN UINT32 Mv);
  EFI_STATUS (EFIAPI *GetLevel) (IN UINT32 Pmic, IN UINT32 Vreg, OUT UINT32 *Mv);
  EFI_STATUS (EFIAPI *SetPwrMode) (IN UINT32 Pmic, IN UINT32 Vreg, IN UINT32 Mode);
  UINTN MultiphaseCtrl;
  EFI_STATUS (EFIAPI *GetStatus) (IN UINT32 Pmic, IN UINT32 Vreg, OUT DP_VREG_STATUS *Sts);
} DP_VREG;
#define VREG_PMIC_A 0u
#define VREG_LDO_2  1u  // EFI_PM_LDO_2
#define VREG_MODE_NPM 4u  // EFI_PM_PWR_SW_MODE_NPM

// Turns L2A (the PD PHY's 3.1 V) on through XBL if it is off.
STATIC VOID
PdSupply (VOID)
{
  DP_VREG       *Vreg = NULL;
  DP_VREG_STATUS Sts;
  UINT32         Mv = 0;
  EFI_STATUS     Rc;

  if (gBS->LocateProtocol (&mVregGuid, NULL, (VOID **)&Vreg) != EFI_SUCCESS || !Vreg) {
    Add ("l2a: no vreg protocol\n");
    return;
  }
  ZeroMem (&Sts, sizeof (Sts));
  Add ("l2a: vreg rev 0x%llx\n", Vreg->Revision);
  WriteReport (FALSE);
  Rc = Vreg->GetStatus (VREG_PMIC_A, VREG_LDO_2, &Sts);
  Vreg->GetLevel (VREG_PMIC_A, VREG_LDO_2, &Mv);
  Add ("l2a: status rc=0x%llx sw=%u ok=%u pin=%u mode=%u level=%umV\n", (UINT64)Rc,
       Sts.SwEnable, (UINT32)Sts.VregOk, Sts.PinCtrled, Sts.SwMode, Mv);
  WriteReport (FALSE);
  // shortcut: SetPwrMode hangs XBL here (mode reads 3, likely NPM already); not called.
  if (Rc == EFI_SUCCESS && (Sts.SwEnable == 1 || Sts.VregOk)) {
    return;
  }
  Rc = Vreg->SetLevel (VREG_PMIC_A, VREG_LDO_2, 3100);
  Add ("l2a: set 3100mV rc=0x%llx\n", (UINT64)Rc);
  WriteReport (FALSE);
  Rc = Vreg->Control (VREG_PMIC_A, VREG_LDO_2, TRUE);
  gBS->Stall (5 * 1000);
  ZeroMem (&Sts, sizeof (Sts));
  Vreg->GetStatus (VREG_PMIC_A, VREG_LDO_2, &Sts);
  Add ("l2a: enable rc=0x%llx -> sw=%u ok=%u\n", (UINT64)Rc, Sts.SwEnable,
       (UINT32)Sts.VregOk);
  WriteReport (FALSE);
}

// DpHw's log: a line, or a line plus a checkpoint of the report.
STATIC VOID
HwSay (IN CONST CHAR8 *Format, ...)
{
  va_list Args;

  va_start (Args, Format);
  VAdd (Format, Args);
  va_end (Args);
}

VOID
DpProbeMark (IN CONST CHAR8 *Format, ...)
{
  va_list Args;

  va_start (Args, Format);
  VAdd (Format, Args);
  va_end (Args);
  WriteReport (FALSE);
}

// The QMP PHY's supplies (Flip2 DT: vdda-phy L9A 1.2 V, vdda-pll L18A
// 0.92 V), on through XBL if they are off. Levels are in uV here.
STATIC VOID
PhySupply (VOID)
{
  STATIC CONST struct { UINT32 Id; UINT32 Uv; CONST CHAR8 *Name; } Ldo[] = {
    {8, 1200000, "l9a"}, {17, 920000, "l18a"}
  };
  DP_VREG       *Vreg = NULL;
  DP_VREG_STATUS Sts;
  UINT32         Uv;
  EFI_STATUS     Rc;

  if (gBS->LocateProtocol (&mVregGuid, NULL, (VOID **)&Vreg) != EFI_SUCCESS || !Vreg) {
    return;
  }
  for (UINTN I = 0; I < ARRAY_SIZE (Ldo); I++) {
    ZeroMem (&Sts, sizeof (Sts));
    Uv = 0;
    Rc = Vreg->GetStatus (VREG_PMIC_A, Ldo[I].Id, &Sts);
    Vreg->GetLevel (VREG_PMIC_A, Ldo[I].Id, &Uv);
    Add ("%s: rc=0x%llx sw=%u ok=%u level=%uuV\n", Ldo[I].Name, (UINT64)Rc,
         Sts.SwEnable, (UINT32)Sts.VregOk, Uv);
    WriteReport (FALSE);
    if (Rc == EFI_SUCCESS && (Sts.SwEnable == 1 || Sts.VregOk)) {
      continue;
    }
    Rc = Vreg->SetLevel (VREG_PMIC_A, Ldo[I].Id, Ldo[I].Uv);
    Add ("%s: set rc=0x%llx\n", Ldo[I].Name, (UINT64)Rc);
    WriteReport (FALSE);
    Rc = Vreg->Control (VREG_PMIC_A, Ldo[I].Id, TRUE);
    gBS->Stall (2000);
    ZeroMem (&Sts, sizeof (Sts));
    Vreg->GetStatus (VREG_PMIC_A, Ldo[I].Id, &Sts);
    Add ("%s: on rc=0x%llx -> sw=%u ok=%u\n", Ldo[I].Name, (UINT64)Rc, Sts.SwEnable,
         (UINT32)Sts.VregOk);
    WriteReport (FALSE);
  }
}

// DP_PD_IO for DpPd: quiet register access, logging into the report.
STATIC UINT16
IoRead (
  IN VOID   *Ctx,
  IN UINT32  Address
)
{
  return Rd ((DP_SPMI *)Ctx, Address);
}

STATIC VOID
IoWrite (
  IN VOID   *Ctx,
  IN UINT32  Address,
  IN UINT8   Value
)
{
  ((DP_SPMI_WRITE_XF)((DP_SPMI *)Ctx)->WriteLong) (
    (DP_SPMI *)Ctx, PM8150B_SID, 0, Address, &Value, 1
  );
}

STATIC VOID
IoSay (
  IN VOID        *Ctx,
  IN CONST CHAR8 *Format,
  ...
)
{
  va_list Args;

  va_start (Args, Format);
  VAdd (Format, Args);
  va_end (Args);
}

// Test runs: Android's script writes RUN_MAGIC in the 4 KiB before the
// report and restarts into the bootloader. Then the probe clears it and
// restarts again, so the device returns to its normal boot by itself.
#define RUN_MAGIC "OABLDPGO"

STATIC VOID
RunOnce (
  IN EFI_BLOCK_IO_PROTOCOL *BlockIo,
  IN UINT64                 Lba
)
{
  OABL_BUFFER Raw = {0};
  BOOLEAN     Go;

  if (!UninstallRead (BlockIo, Lba, 4096, &Raw) || Raw.Len != 4096) {
    BufferFree (&Raw);
    return;
  }
  Go = CompareMem (Raw.Data, RUN_MAGIC, 8) == 0;
  if (Go) {
    ZeroMem (Raw.Data, 4096);
    // Without clearing it, every restart would come back here.
    if (UninstallWrite (BlockIo, Lba, Raw.Data, 4096) != EFI_SUCCESS ||
        BlockIo->FlushBlocks (BlockIo) != EFI_SUCCESS) {
      Go = FALSE;
    }
  }
  BufferFree (&Raw);
  // The menu stays up on the monitor afterwards; no restart.
  (VOID)Go;
}

// TRUE if a test run is waiting (RUN_MAGIC set), so a normal boot opens the
// menu instead: Linux can't ask for the bootloader on this device.
BOOLEAN
DpProbeWanted (VOID)
{
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINT64                 First;
  UINT64                 Last;
  UINTN                  BlockSize;
  OABL_BUFFER            Raw = {0};
  BOOLEAN                Go  = FALSE;

  if (!UninstallFindMisc (&BlockIo, &First, &Last, LOG_QUIET)) {
    return FALSE;
  }
  BlockSize = BlockIo->Media->BlockSize;
  if (BlockSize == 0 || REPORT_SIZE % BlockSize != 0 ||
      Last - First + 1 < (REPORT_SIZE + 2 * 4096) / BlockSize) {
    return FALSE;
  }
  if (UninstallRead (BlockIo, Last + 1 - (REPORT_SIZE + 2 * 4096) / BlockSize,
                     4096, &Raw) && Raw.Len == 4096) {
    Go = CompareMem (Raw.Data, RUN_MAGIC, 8) == 0;
    // Cleared now, so a hang in the probe can't loop every boot into it.
    if (Go) {
      ZeroMem (Raw.Data, 4096);
      Go = UninstallWrite (BlockIo, Last + 1 - (REPORT_SIZE + 2 * 4096) / BlockSize,
                           Raw.Data, 4096) == EFI_SUCCESS &&
           BlockIo->FlushBlocks (BlockIo) == EFI_SUCCESS;

    }
  }
  BufferFree (&Raw);
  return Go;
}

STATIC VOID
Save (VOID)
{
  WriteReport (TRUE);
}

// Writes the report to the 8 KiB before the settings store at misc's end.
// Finish also handles a test run's restart.
STATIC VOID
WriteReport (IN BOOLEAN Finish)
{
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINT64                 First;
  UINT64                 Last;
  UINTN                  BlockSize;

  if (!UninstallFindMisc (&BlockIo, &First, &Last, LOG_QUIET)) {
    return;
  }
  BlockSize = BlockIo->Media->BlockSize;
  if (BlockSize == 0 || REPORT_SIZE % BlockSize != 0 ||
      Last - First + 1 < (REPORT_SIZE + 4096) / BlockSize) {
    return;
  }
  if (UninstallWrite (
        BlockIo, Last + 1 - (REPORT_SIZE + 4096) / BlockSize, (UINT8 *)mReport,
        REPORT_SIZE
      ) == EFI_SUCCESS) {
    BlockIo->FlushBlocks (BlockIo);
  }
  if (Finish) {
    RunOnce (BlockIo, Last + 1 - (REPORT_SIZE + 2 * 4096) / BlockSize);
  }
}

// Logs every graphics output's mode, and fills each one after the first
// (the panel) with colour bars, so a second output on the monitor shows.
STATIC VOID
ProbeGops (VOID)
{
  STATIC CONST UINT32 Bars[] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                                0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};
  EFI_HANDLE *Handles = NULL;
  UINTN       Count   = 0;

  if (gBS->LocateHandleBuffer (
        ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles
      ) != EFI_SUCCESS) {
    Add ("gop: none\n");
    return;
  }
  for (UINTN Index = 0; Index < Count; Index++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL         *Gop = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINT32                                W, H;

    if (gBS->HandleProtocol (
          Handles[Index], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop
        ) != EFI_SUCCESS ||
        !Gop || !Gop->Mode || !Gop->Mode->Info) {
      Add ("gop%u: unusable\n", (UINT32)Index);
      continue;
    }
    Info = Gop->Mode->Info;
    W    = Info->HorizontalResolution;
    H    = Info->VerticalResolution;
    Add (
      "gop%u: %ux%u fmt=%u stride=%u mode=%u/%u fb=0x%llx size=0x%llx\n",
      (UINT32)Index, W, H, (UINT32)Info->PixelFormat,
      Info->PixelsPerScanLine, Gop->Mode->Mode, Gop->Mode->MaxMode,
      (UINT64)Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize
    );
    for (UINT32 M = 0; M < Gop->Mode->MaxMode && M < 8; M++) {
      EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *MI   = NULL;
      UINTN                                 Size = 0;

      if (Gop->QueryMode (Gop, M, &Size, &MI) == EFI_SUCCESS && MI) {
        Add ("  mode%u: %ux%u\n", M, MI->HorizontalResolution,
             MI->VerticalResolution);
        gBS->FreePool (MI);
      }
    }
    if (Index == 0 || W == 0 || H == 0) {
      continue;
    }
    for (UINT32 B = 0; B < 8; B++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL Px;
      EFI_STATUS                    Rc;

      Px.Blue     = (UINT8)Bars[B];
      Px.Green    = (UINT8)(Bars[B] >> 8);
      Px.Red      = (UINT8)(Bars[B] >> 16);
      Px.Reserved = 0;
      Rc          = Gop->Blt (
        Gop, &Px, EfiBltVideoFill, 0, 0, B * W / 8, 0, W / 8, H, 0
      );
      if (B == 0) {
        Add ("gop%u: fill -> 0x%llx\n", (UINT32)Index, (UINT64)Rc);
      }
    }
  }
  gBS->FreePool (Handles);
}

VOID
DpProbeRun (VOID)
{
  DP_SPMI   *Spmi = NULL;
  EFI_STATUS Status;

  ZeroMem (mReport, REPORT_SIZE);
  mUsed = 0;
  Add ("%s", REPORT_MAGIC);
  Add ("gop_count=%u\n", (UINT32)gDisplayGopCount);
  ProbeGops ();
  Status = gBS->LocateProtocol (&mSpmiGuid, NULL, (VOID **)&Spmi);
  if (EFI_ERROR (Status) || !Spmi) {
    Add ("spmi: not found (0x%llx)\n", (UINT64)Status);
    Save ();
    return;
  }
  Add ("spmi: revision 0x%llx\n", Spmi->Revision);
  // Only the XF calling convention is wired up; MXF would fault.
  if (Spmi->Revision >= DP_SPMI_REVISION_BUS_ID) {
    Add ("spmi: MXF build, not probed\n");
    Save ();
    return;
  }
  PdSupply ();
  if (Attach (Spmi)) {
    DP_PD_IO Io  = {Spmi, IoRead, IoWrite, IoSay};
    UINT32   Pins = 0;
    BOOLEAN  Hpd  = FALSE;

    BOOLEAN  Dp;

    Dp = DpPdRun (&Io, &Pins, &Hpd);
    Add ("pd: result %u\n", (UINT32)Dp);
    if (Dp && Pins) {
      STATIC CONST DP_HW_IO HwIo = {HwSay, DpProbeMark};

      PhySupply ();
      Add ("hw: result %u\n",
           (UINT32)DpHwRun (&HwIo, (Rd (Spmi, TYPEC_BASE + TC_MISC_STATUS) & TC_MISC_ORIENTATION) != 0));
    }
  }
  Dump (Spmi, "typec-after", TYPEC_BASE, 0x10);
  Add ("end\n");
  Save ();
}
