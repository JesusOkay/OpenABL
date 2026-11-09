# Menu over USB-C DisplayPort (SM8250)

This branch makes the OpenABL menu show on an external monitor through
USB-C, so a Retroid Pocket Flip2 (and probably a Retroid Pocket 5, same
SM8250 platform) with a broken internal screen can still pick Android or
Linux at power-on.

XBL brings up only the internal DSI panel, so the ABL does the whole
DisplayPort path itself when the menu opens:

1. **Type-C / USB-PD** (`Efi/DpProbe.c`, `Efi/DpPd.c`): PM8150B Type-C
   port as source over SPMI, VBUS (OTG) and VCONN on, PD 3.0 Source
   Capabilities, Discover Identity/SVIDs/Modes, Enter Mode and DP
   Configure (pin assignment C), HPD from DP Status/Attention.
   Many adapters ignore PD if the first Source_Capabilities comes late;
   the code cuts VBUS for 800 ms and retries.
2. **PHY and DP controller** (`Efi/DpHw.c`): SBU mux (TLMM 8/9), USB3/DP
   QMP combo PHY in DP mode, DP AUX (DPCD, EDID), HBR2 x2 link training.
3. **Scanout** (`Efi/DpHw.c`): DMA1 → LM2 → PP2 → INTF0 on CTL2, a
   1920x1080 XRGB buffer at 1080p60 (148.5 MHz). XBL's panel pipeline
   (CTL0/VIG0/LM0/INTF1) is left alone.
4. **Mirroring** (`Efi/Display.c`): every panel blit is copied, rotated to
   landscape, into the scanout buffer.

Register values follow Linux (`phy-qcom-qmp-combo.c`,
`drivers/gpu/drm/msm/dp`, `dpu_hw_*`) and what ROCKNIX programs on the
same hardware.

## Status

- Tested on a Retroid Pocket Flip2 with a USB-C → DP adapter (pin C,
  2 lanes) and a 1080p monitor: menu shows, keys work, Android and Linux
  boot from it.
- Retroid Pocket 5: **untested**.
- Fixed 1920x1080@60; the monitor must accept it.
- While DP is up there is no fastboot over USB (same port).
- Each menu open writes a bring-up report to misc (see `Efi/DpProbe.h`
  for how to read it). Send it along with any bug report.

## Test hook

Writing `OABLDPGO` at misc end − 16 KiB makes the next normal boot open the
menu (Linux can't reboot into the bootloader on this device). The ABL
clears the mark before running.

**Flashing an ABL can brick the device. Keep a copy of your abl_a/abl_b
first.**
