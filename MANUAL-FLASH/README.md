# Optional manual-flash files

This directory is separate from the Android and TWRP builds. None of these
files is consumed by `systemimage`, `bootimage`, or `recoveryimage`, and no
build or setup script flashes a partition or installs root software.

- `su6-7.3/NON-HLOS.bin` and `su6-7.3/fsg.mbn` are the matching stock Droid
  Mini radio pair. See [su6-7.3/README.md](su6-7.3/README.md) for provenance,
  checksums, and partition names.
- `supersu/SR5-SuperSU-v2.82-SR5-20171001224502.zip` is the optional ZIP
  tested with TWRP 3.1.1 on the enforcing build. See
  [supersu/README.md](supersu/README.md) before installing it.

The ROM is built non-rooted. Flashing the SuperSU ZIP modifies the installed
device; it is not required to build or run the ROM. Keep any
manual-flash files out of the source dependency graph and the default build.
