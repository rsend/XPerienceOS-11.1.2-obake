XPerienceOS 11.1.2 for Droid Mini (obake)

This is a heavily AI-assisted port of Android 7 for the Droid Mini (XT1080) that is more or less fully working.

For reference, the last known working build I'm aware of was LineageOS 13 (Android 6): https://xdaforums.com/t/rom-unofficial-lineage-os-13-for-obake-22-2-17.3319958/

There was an effort to make this exact version work, but as far as I can tell the maintainer disappeared and it didn't quite work right: https://xdaforums.com/t/oms7-nougat-7-1-2-obake-xperience-11-1-2_r29-nightly.3681850/

AI tools are now useful enough that such efforts are more easily doable, especially for those of us who might have a niche interest but not the specific skills. I'm aware of the different opinions about using AI tools for such things over really studying the depths of how everything works; but it's my belief that if the tools allows us to accomplish projects that would otherwise remain undone that's a good enough tradeoff for me.
I welcome discussions and input on this point, however.

So why? Why not!

I recently dug up my old phone and decided it might fun to try and port "newer" Android versions - just for the challenge. 

According to some analysis, this hardware is technically only blocked at Android 15 (!!!) because 64-bits becomes mandatory at that point. So my goal is to see how far I can take it, and this port is a stepping stone to later versions. 

What is it and works?

- XperienceOS 11.2.1 based on Android 7
- SELinux enforcing
- Recovery using TWRP 3.1.1
- Play Services SHOULD work, according to Google the hard minimum IS Android 7
- F-Droid / Aurora Store work great as well
- SuperSU using Chainfire's SuperSU 2.8.2-SR5 ZIP; included here

- Charging LED fixed
- Sound routing (Earpiece / Speaker / Headphones)
- VoLTE (!)
- Bluetooth
- Wifi

- NO Camera. I fixed it in Android 8 instead.

Note: The device used to NOT have VoLTE enabled since 2G and 3G were acceptable telephone fallbacks at the time. Since we no longer really have 3G, VoLTE is mandatory and necessary for all future ports. 
Getting this to work took a lot of time and effort. It's also tied to a specific version of the modem binaries (SU6-7.3) which are NOT the latest; they are included here. See instructions at the end.

How do you get it?

This directory is a standalone copy of the XPerienceOS source checkout that has been heavily modified to build for OBAKE. There was ALMOST a version of this for the GHOST platform which is very similar, so I started with that and tweaked it until most things worked.

It produces three images: system.img, boot.img and recovery.img

The kernel source comes from the XPerience Motorola Ghost kernel project,
with obake-specific modifications. See [KERNEL_SOURCE.md](KERNEL_SOURCE.md)
for its exact upstream revision, contributor credit, local-change record,
GPLv2 license, and corresponding-source location.

It builds on Ubuntu 20.04 and the project is optimized for simplicity. All code and tools are included; only two setup scripts are needed. Build sequence:

- Install a fresh Ubuntu 20.04 VM

  - 100GB disk (smaller may be ok, didn't try)
  - 16GB of RAM (8GB may work)
  - At least 4 CPUs

- Checkout / clone this repository

 On your fresh Ubuntu installation, run as regular user:

`bash ./install_deps.sh`

This installs Ubuntu packages, downloads exact pinned historical
prebuilt build inputs, and builds the in-tree Bison 2.7 host tool. It invokes
sudo only for Ubuntu packages. It does not fetch ROM or device source projects. See [BUILD_ENVIRONMENT.md](BUILD_ENVIRONMENT.md) for the toolchain inventory.

`m` may be used instead of `make` after sourcing the setup script. The two
output directories are deliberately separate: ROM images are `user` and
enforcing; TWRP 3.1.1 recovery is `userdebug` and contains no SuperSU.
The setup script derives all paths from its own location, so the tree may be
cloned anywhere. 

If you are building the system image and boot image, set up the environment and build: 

```sh
source ./setup_env.sh rom
./jack.sh 6
make -j4 systemimage bootimage
```

NOTE2: "6" in the jack.sh line is heap size in GB. It must be smaller than your RAM with room to spare; I tried 6 GB heap with 8GB of RAM and it works, but 3GB heap is too little and throws an error.

If you are building `recovery.img`, run: 

```
source ./setup_env.sh recovery
make -jN recoveryimage
```

Output directories are separate as above:
- ROM and boot image will be in
- Recovery image will be in

All three of these can be flashed via fastboot the usual way:

- fastboot flash boot boot.img
- fastboot flash system system.img

YOU MUST ALSO FLASH THE SPECIFIC RADIO BINARIES from the MANUAL_FLASH directory:

- fastboot flash modem NON-HLOS.bin
- fastboot flash fsg fsg.mbn
- fastboot erase modemst1
- fastboot erase modemst2

If you want root, flash the included SuperSU-v2.82-SR5.zip through TWRP. This should also install the SuperSU app in userspace.

