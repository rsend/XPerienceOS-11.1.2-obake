# Optional SuperSU ZIP

`SR5-SuperSU-v2.82-SR5-20171001224502.zip` is the exact ZIP tested on the
Droid Mini with TWRP 3.1.1 and the enforcing, non-rooted XPerience build.
Installing it is optional and must be done manually in recovery; no build
target or setup script installs it.

The test established that `su` produced an approval prompt and the installed
OS remained SELinux-enforcing. The SuperSU app's launcher entry initially
remained disabled from an earlier installation attempt and was restored with
`pm enable` as root; this should not be represented as a fully clean-install
test of the app UI. TWRP 2.8.4 did not install this ZIP successfully in the
same testing sequence.

SHA-256: `b53beee2e2ca7b74f5a91f957525a660afd180c6e2ceca9017697d0686a5d49f`.
The ZIP was copied from the device's internal-storage root. All 97 archive
members pass a payload CRC check. Info-ZIP's `unzip -t` nevertheless reports
malformed WinNT extra-field metadata on some members; do not treat its nonzero
exit status as evidence of a corrupt payload.

Chainfire's [2015 redistribution statement](https://chainfire.eu/articles/917/SuperSU_transferred)
permits redistribution of non-Pro SuperSU ZIPs in **unmodified form**, until
further notice. His [SR5 release announcement](https://chainfire.eu/articles/995/SuperSU_v2.82-SR5_and_suhide_v1.09_released)
lists this exact ZIP filename. This package contains `common/Superuser.apk`
and no Pro APK. Preserve the ZIP byte-for-byte when publishing it; do not
repack or modify its contents. The local SHA-256 above is the integrity check
for the file copied from the device, not an independently published upstream
checksum.

Chainfire's [How-To SU](https://su.chainfire.eu/) also describes embedding a
SuperSU ZIP in a ROM installer, but this project does **not** do that: users
choose whether to flash this separate ZIP with TWRP.
