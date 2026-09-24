# SU6-7.3 voice calibration loader

`libacdbloader.so` is extracted unchanged from `/lib/libacdbloader.so` in the
Droid Mini SU6-7.3 system image. SHA256 and analysis are recorded in
`FIXES/AUDIO/VOLTE_VOLUME_CONTROL.md`.

The older loader remains in vendor/motorola/msm8960dt-common/proprietary/lib.
The obake/device.mk copy mapping selects this loader for obake only. It restores the
stock LTE calibration rows (DSP networks 0x11324/0x11325/0x11326) missing from
the older loader; no carrier identifiers or amplifier gain overrides are used.
