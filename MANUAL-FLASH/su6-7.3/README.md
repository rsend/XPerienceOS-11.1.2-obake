# Droid Mini SU6-7.3 radio pair

These are the matching stock Motorola Droid Mini (`obake`) Android 4.4.4
SU6-7.3 radio files for manual flashing to the `modem` and `fsg` partitions.
The expected baseband is `MSM8960PRO_BP_23255.132.81.01R`.

| File | Partition | SHA-256 |
| --- | --- | --- |
| `NON-HLOS.bin` | `modem` | `4bbe89f2f64620a23ebc34ce7264e5e528940f4d36c5cf7e3e5293ce3e6ce3b5` |
| `fsg.mbn` | `fsg` | `4718f2e3421ff63fc97badd13b78dd1ccaf36080f570b8b14d1df7f1feeb1d77` |

Both files came from the local stock archive
`CFC_obake_verizon-user-4.4.4-SU6-7.3-release-keys.xml.zip`. Its flashing XML
also identifies the same pair by MD5 (`612409C5E10E92881EED39A2FACE9AEB`
for `NON-HLOS.bin`, `9B7D707C4B83859515DF00E314A41191` for `fsg.mbn`).

These files are not inputs to `boot.img` or `system.img` and are not installed
by the Android product makefiles. This directory contains no flashing script:
flashing radio partitions is a separate manual operation. Do not substitute
the SU6-7.7 FSG or a backed-up live partition for this stock SU6-7.3 file.
