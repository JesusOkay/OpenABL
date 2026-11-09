/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
  DpHw - SM8250 USB-C DisplayPort hardware: SBU mux, QMP PHY, DP AUX.
*/

#ifndef __EFI_DP_HW_H__
#define __EFI_DP_HW_H__

#include "Base/OablBase.h"

typedef struct {
  // Logs a line into the report.
  VOID (*Say) (
    IN CONST CHAR8 *Format,
    ...
  );
  // Logs a line and saves the report, so a hang after it shows where.
  VOID (*Mark) (
    IN CONST CHAR8 *Format,
    ...
  );
} DP_HW_IO;

// After DP alt mode is entered (pin assignment C, HPD high): brings the PHY
// and the DP controller's AUX up and reads the sink's DPCD and EDID.
BOOLEAN
DpHwRun (
  IN CONST DP_HW_IO *Io,
  IN BOOLEAN         Reverse
);

// The monitor's 1920x1080 picture; NULL until DpHwRun brought DP up.
extern UINT32 *gDpScanout;

// Copies a panel blit (portrait, as Display draws it) into the monitor's
// landscape scanout; does nothing until DpHwRun brought the monitor up.
VOID
DpHwMirror (
  IN CONST UINT32 *Pixels,
  IN UINT32        Stride,
  IN UINT32        X,
  IN UINT32        Y,
  IN UINT32        W,
  IN UINT32        H
);

#endif
