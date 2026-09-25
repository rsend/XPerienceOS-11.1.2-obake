XPerienceOS 11.1.2 for Droid Mini (obake)

This is a heavily AI-assisted port of Android 7 for the Droid Mini (XT1080) that is more or less fully working.

For reference, the last known working build I'm aware of was LineageOS 13 (Android 6): https://xdaforums.com/t/rom-unofficial-lineage-os-13-for-obake-22-2-17.3319958/

AI tools are now useful enough that such efforts are more easily doable, especially for those of us who might have a niche interest but not the specific skills. I'm aware of the different opinions about using AI tools for such things over really studying the depths of how everything works; but it's my belief that if the tools allows us to accomplish projects that would otherwise remain undone that's a good enough tradeoff for me.
I welcome discussions and input on this point, however.

So why? Why not!

I recently dug up my old phone and decided it might fun to try and port "newer" Android versions - just for the challenge. 

According to some analysis, this hardware is technically only blocked at Android 15 (!!!) because 64-bits becomes mandatory at that point. So my goal is to see how far I can take it, and this port is a stepping stone to later versions. 

So, what works?

- XperienceOS 11.2.1 based on Android 7
- SELinux enforcing
- Rootable with TWRP 3.1.1 recovery, see instructions below
- Play Services SHOULD work, according to Google the hard minimum IS Android 7
- F-Droid / Aurora Store work great as well

- Charging LED fixed
- Sound routing (Earpiece / Speaker / Headphones)
- VoLTE (!)
- Bluetooth
- Wifi

Note: The device used to NOT have VoLTE enabled since 2G and 3G were acceptable telephone fallbacks at the time. Since we no longer really have 3G, VoLTE is mandatory and necessary for all future ports. 
Getting this to work took a lot of time and effort.

How do you get it?

This directory is a standalone copy of the XPerienceOS source checkout that has been heavily modified to build for OBAKE. There was ALMOST a version of this for the GHOST platform which is very similar, so I started with that and tweaked it until most things worked.

It builds on Ubuntu 20.04 and the project is optimized for simplicity. All code and tools are included; only two setup scripts are needed. Build sequence:

- Install a fresh Ubuntu 20.04 VM
  - 100GB disk (smaller may be ok, didn't try)
  - 16GB of RAM (8GB may work)



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
