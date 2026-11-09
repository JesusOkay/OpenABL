/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
  DpPd - USB PD source/DFP and DisplayPort alternate mode entry on the
  PM8150B PD PHY. Polled; the caller has attached the port and turned VBUS
  (and VCONN if needed) on.
*/

#ifndef __EFI_DP_PD_H__
#define __EFI_DP_PD_H__

#include "Base/OablBase.h"

#define DP_PDPHY_BASE 0x1700u

// Register access and logging, provided by the caller (DpProbe).
typedef struct {
  VOID *Ctx;
  // A PM8150B register, or 0xFFFF if the read failed.
  UINT16 (*Read) (
    IN VOID   *Ctx,
    IN UINT32  Address
  );
  VOID (*Write) (
    IN VOID   *Ctx,
    IN UINT32  Address,
    IN UINT8   Value
  );
  VOID (*Say) (
    IN VOID        *Ctx,
    IN CONST CHAR8 *Format,
    ...
  );
} DP_PD_IO;

// Makes a 5 V contract and enters DisplayPort mode. On success
// *PinAssign is the configured pin assignment bit (C = BIT2, D = BIT3,
// E = BIT4) and *Hpd whether the monitor's hot-plug is high.
BOOLEAN
DpPdRun (
  IN CONST DP_PD_IO *Io,
  OUT UINT32        *PinAssign,
  OUT BOOLEAN       *Hpd
);

#endif
