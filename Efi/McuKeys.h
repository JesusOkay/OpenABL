/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
  McuKeys - the Retroid Pocket gamepad MCU (SM8250: Flip2, RP5, Mini) as
  menu keys, so Select and Home work when the volume keys don't.

  The MCU sits on QUP16 (GENI UART, 115200 8N1) with power on TLMM 3 and
  reset/boot/enable on TLMM 109/110/111, as ROCKNIX's retroid-pocket-gamepad
  driver drives it. Its input frames are
    A5 D3 5A 3D, frame, command, length (Le16), data, XOR of frame..data
  and command 2 carries the buttons as a Le16 bitmap.
*/

#ifndef __EFI_MCU_KEYS_H__
#define __EFI_MCU_KEYS_H__

#include "Base/OablBase.h"

#define MCU_KEY_SELECT BIT10
#define MCU_KEY_HOME   BIT14

// Powers the MCU and opens the UART. Does nothing unless QUP16 runs the
// UART firmware. Steps go to the DP report.
VOID
McuKeysStart (VOID);

// Reads what arrived and returns the buttons held (MCU_KEY_*), as of the
// last good frame; 0 if the MCU isn't up.
UINT32
McuKeysHeld (VOID);

#endif
