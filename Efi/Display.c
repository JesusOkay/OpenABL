/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026-present Open Handheld Collective
 *   https://github.com/OpenHandheldCollective
 */

/**
  Display - draws frames through the Graphics Output Protocol.
*/

#include "Efi/Display.h"

#include "Efi/DpHw.h"
#include "Efi/Handover.h"

// GET_PROTOCOL leaves the console driver bound to the GOP. We only call Blt
// and read the mode.
STATIC EFI_STATUS
OpenGop (
  IN EFI_HANDLE                      Handle,
  OUT EFI_GRAPHICS_OUTPUT_PROTOCOL **Gop
)
{
  VOID      *Interface = NULL;
  EFI_STATUS Status    = gBS->OpenProtocol (
    Handle, &gEfiGraphicsOutputProtocolGuid, &Interface, gImageHandle, NULL,
    EFI_OPEN_PROTOCOL_GET_PROTOCOL
  );

  *Gop = Interface;
  return Status;
}

STATIC DISPLAY
Opened (IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop)
{
  return (DISPLAY){Gop, Gop->Mode->Info->HorizontalResolution,
                   Gop->Mode->Info->VerticalResolution};
}

// Every other graphics output XBL registers (an external display, if XBL
// brings one up) gets the same frames as the first, so the menu shows on it.
#define DISPLAY_MIRRORS 4
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mMirror[DISPLAY_MIRRORS];
STATIC UINTN                         mMirrorCount;
UINTN                                gDisplayGopCount;

EFI_STATUS
DisplayOpen (OUT DISPLAY *Out)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_HANDLE                   *Handles = NULL;
  EFI_HANDLE                    Handle;
  UINTN                         Count = 0;
  EFI_STATUS                    Status;

  Status = gBS->LocateHandleBuffer (
    ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles
  );
  if (Status != EFI_SUCCESS) {
    return Status;
  }
  Handle           = Count ? Handles[0] : NULL;
  gDisplayGopCount = Count;
  mMirrorCount     = 0;
  for (UINTN Index = 1; Index < Count && mMirrorCount < DISPLAY_MIRRORS; Index++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Extra;

    if (OpenGop (Handles[Index], &Extra) == EFI_SUCCESS && Extra->Mode &&
        Extra->Mode->Info) {
      mMirror[mMirrorCount++] = Extra;
    }
  }
  if (Handles) {
    gBS->FreePool (Handles);
  }
  if (!Handle) {
    return EFI_NOT_FOUND;
  }
  Status = OpenGop (Handle, &Gop);
  if (Status != EFI_SUCCESS) {
    return Status;
  }
  *Out = Opened (Gop);
  return EFI_SUCCESS;
}

EFI_STATUS
DisplayOpenPanel (OUT DISPLAY *Out)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *First        = NULL;
  EFI_HANDLE                   *Handles      = NULL;
  UINT64                        RegionBase   = 0;
  UINT64                        RegionLength = 0;
  BOOLEAN                       Region;
  UINTN                         Count = 0;
  EFI_STATUS                    Status;

  Status = gBS->LocateHandleBuffer (
    ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles
  );
  if (Status != EFI_SUCCESS) {
    return Status;
  }
  Region = HandoverDisplayRegion (&RegionBase, &RegionLength);
  for (UINTN Index = 0; Index < Count; Index++) {
    UINT64 Base;

    if (OpenGop (Handles[Index], &Gop) != EFI_SUCCESS) {
      continue;
    }
    Base = Gop->Mode->FrameBufferBase;
    if (Gop->Mode->Info->PixelFormat != PixelBltOnly && Region &&
        RegionBase <= Base && Base < RegionBase + RegionLength) {
      gBS->FreePool (Handles);
      *Out = Opened (Gop);
      return EFI_SUCCESS;
    }
    if (!First) {
      First = Gop;
    }
  }
  if (Handles) {
    gBS->FreePool (Handles);
  }
  if (!First) {
    return EFI_NOT_FOUND;
  }
  *Out = Opened (First);
  return EFI_SUCCESS;
}

VOID
DisplayBlit (
  IN DISPLAY      *Display,
  IN CONST UINT32 *Pixels,
  IN UINT32        Stride,
  IN UINT32        X,
  IN UINT32        Y,
  IN UINT32        W,
  IN UINT32        H
)
{
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info = Display->Gop->Mode->Info;

  if ((UINT64)X + W > Info->HorizontalResolution ||
      (UINT64)Y + H > Info->VerticalResolution) {
    return;
  }
  // A BltPixel is {blue, green, red, reserved}, which is a little-endian
  // 0x00RRGGBB.
  Display->Gop->Blt (
    Display->Gop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Pixels, EfiBltBufferToVideo,
    0, 0, X, Y, W, H, (UINTN)Stride * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
  );
  DpHwMirror (Pixels, Stride, X, Y, W, H);
  for (UINTN Index = 0; Index < mMirrorCount; Index++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = mMirror[Index];
    UINT32                        MW  = Gop->Mode->Info->HorizontalResolution;
    UINT32                        MH  = Gop->Mode->Info->VerticalResolution;

    // Another size than the panel: draw the part that fits.
    if (X >= MW || Y >= MH) {
      continue;
    }
    Gop->Blt (
      Gop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Pixels, EfiBltBufferToVideo,
      0, 0, X, Y, MIN (W, MW - X), MIN (H, MH - Y),
      (UINTN)Stride * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
    );
  }
}
