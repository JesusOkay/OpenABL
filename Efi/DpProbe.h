/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
  DpProbe - USB-C DisplayPort output for the menu (SM8250).

  When the menu opens: PM8150B Type-C as source, USB-PD contract and DP
  alt mode with the adapter, then DpHw brings the monitor up. Every step
  goes to a text report in misc, in the 8 KiB before the settings store,
  readable from Linux or Android after the next boot:

    M=/dev/block/by-name/misc; SZ=$(blockdev --getsize64 $M)
    dd if=$M bs=4096 skip=$(( (SZ - 12288) / 4096 )) count=2 | tr -d '\000'
*/

#ifndef __EFI_DP_PROBE_H__
#define __EFI_DP_PROBE_H__

#include "Base/OablBase.h"

// Brings DP up if an adapter is plugged in, and writes the report.
// Never fails the caller.
VOID
DpProbeRun (VOID);

// TRUE if a test run is waiting; the boot then opens the menu.
BOOLEAN
DpProbeWanted (VOID);

// Appends a line to the report and saves it (menu freeze diagnosis).
VOID
DpProbeMark (IN CONST CHAR8 *Format, ...);

#endif
