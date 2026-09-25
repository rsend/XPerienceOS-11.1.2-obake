XPerienceOS 11.1.2 for Droid Mini (obake)

This is a heavily AI-assisted port of Android 7 for the Droid Mini (XT1080). 

Why? Why not!

I recently dug up my old phone and decided it might fun to try and port "newer" Android versions - just for the challenge. AI tools are now useful enough that such efforts are more easily doable, especially for those of us who might have a niche interest but not the specific skills.  

For reference, the last known working build I'm aware of was LineageOS 13 (Android 6): https://xdaforums.com/t/rom-unofficial-lineage-os-13-for-obake-22-2-17.3319958/



This directory is a standalone copy of the XPerienceOS source checkout.
All three image targets built successfully from this staged tree using
locally installed pinned prebuilts, before the final test-material scrub. The
upload candidate still needs a build from a fresh Ubuntu installation using
`install_deps.sh`. Build outputs, Repo
metadata, investigation artifacts, and unrelated stock firmware archives
belong outside it. The optional radio pair and SuperSU ZIP live under
`MANUAL-FLASH/` and are not build inputs.

On a fresh Ubuntu 20.04 amd64 host, run `bash ./install_deps.sh` as your
regular user. It installs Ubuntu packages, downloads exact pinned historical
prebuilt build inputs, and builds the in-tree Bison 2.7 host tool. It invokes
sudo only for Ubuntu packages. It does not fetch ROM or device source projects.
Then use a Bash shell in this directory. See
[BUILD_ENVIRONMENT.md](BUILD_ENVIRONMENT.md) for the toolchain inventory and
current validation status.

```sh
source ./setup_env.sh rom
./jack.sh 6
make -j6 systemimage bootimage

source ./setup_env.sh recovery
make -j6 recoveryimage
```

`m` may be used instead of `make` after sourcing the setup script. The two
output directories are deliberately separate: ROM images are `user` and
enforcing; TWRP 3.1.1 recovery is `userdebug` and contains no SuperSU.
The setup script derives all paths from its own location, so the tree may be
cloned anywhere. It does not fetch source or install host packages.

The Ubuntu 20.04 setup guide is included, but its fresh-host installation
still needs an end-to-end test. This release tree's `lunch` has been changed to
reject missing products locally; it no longer invokes upstream roomservice.
