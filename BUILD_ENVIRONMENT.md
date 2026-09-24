# Droid Mini image build environment

This is the build setup for an Ubuntu 20.04 amd64 checkout of this release
tree. It targets only `system.img`, `boot.img`, and TWRP 3.1.1 `recovery.img`.
All three image targets built locally from the staged tree with locally
installed pinned prebuilts before the final test-material scrub. The upload
candidate needs an end-to-end build from a fresh checkout using
`install_deps.sh`; do not claim that path is verified yet.

## Host setup

Run `bash ./install_deps.sh` once. The script checks the Ubuntu version and
architecture, installs its declared Ubuntu packages (including OpenJDK 8,
Python 2, Flex, Ninja, and ordinary C/C++/archive utilities), downloads the
exact pinned prebuilt build inputs, and builds Bison 2.7 from `external/bison`
into the ROM host output directory. Run the installer as a regular user; it
uses `sudo` only for Ubuntu packages. It does not fetch ROM or device source
projects. The package baseline and complete install have not yet been
validated on a fresh Ubuntu installation.

For an enforcing, unrooted ROM image build:

```sh
source ./setup_env.sh rom
./jack.sh 3
make -j6 systemimage bootimage
```

For a separate TWRP 3.1.1 recovery build:

```sh
source ./setup_env.sh recovery
make -j6 recoveryimage
```

`m` may replace `make` after sourcing `setup_env.sh`. Outputs are under
`out-release/target/product/obake/` and
`out-twrp-userdebug/target/product/obake/`, respectively. The script derives
all checkout paths from its own location. The ROM uses `user`; recovery uses
`userdebug`, because the legacy recovery policy is not valid for a `user`
build. Neither image bundles SuperSU. Optional manual-flash files are in
`MANUAL-FLASH/` and are not build inputs.
The setup script also sets `LIBCORE_SKIP_TESTS=true`, because this image-only
release excludes libcore's optional test suites.

## Toolchain policy and current inventory

Ubuntu packages provide general host utilities, Flex, and Ninja. Ubuntu 20.04's
Ninja 1.10 parsed the recorded ROM graph with the same 120,631 targets as the
original bundled Ninja; the staged build uses `ninja` from `PATH`. The copied
Ninja prebuilt has been removed. The pinned Flex 2.5.39 binary is downloaded
by `install_prebuilts.sh` rather than packaged in the repository: Ubuntu's
Flex 2.6.4 generates a C++ scanner incompatible with MCLinker's checked-in
`FlexLexer.h`. Build rules run the historical Flex with `LC_ALL=C` to avoid
its locale assertion on Ubuntu 20.04.

Ubuntu 20.04's Bison 3.5 rejected this tree's AIDL grammar. The bundled
Bison 2.7 binary has been removed: `install_deps.sh` builds the checked-in
Bison 2.7 host module, and Yacc rules depend on that host output so a clean
image build can rebuild it. A separately tested build of the host module
generated AIDL parser files byte-for-byte identical to the former prebuilt.

Go is **not required** for these three image targets: the selected builds have
`USE_SOONG` and `USE_SOONG_FOR_KATI` unset, and the recorded image graphs have
no Go inputs. The copied Go toolchain has therefore been removed and the
installer does not install Go. Enabling Soong is outside this release's image
build workflow; Ubuntu 20.04's Go 1.13 also failed to link this historical
Blueprint bootstrap unchanged (`not package main`).

The following `prebuilts/` content is image-build input, not a substitute for
similarly named Ubuntu packages. It is no longer included in the source
release. `install_prebuilts.sh`, called by `install_deps.sh`, downloads the
exact upstream Git commits recorded in the original working tree and copies
only the files listed in `prebuilt-filelists/` into the checkout:

| Directory | Why retained |
| --- | --- |
| `clang/host/linux-x86/clang-2690385` | Selected target Clang compiler. |
| `gcc/linux-x86/arm/arm-linux-androideabi-4.9` | Selected ARM cross-compiler and binutils. |
| `gcc/linux-x86/host/x86_64-linux-glibc2.15-4.8` | Selected host compiler/sysroot. |
| `misc/` | Flex 2.5.39, referenced `aprotoc`, relocation packer, and image-input Java libraries. |
| `ndk/current/` | ARM platform headers/libraries for API 8, 9, 14, 17, and 19, selected C++ STL sources, and Android support headers required by LatinIME. |
| `sdk/` | Historical Android API JARs/stubs, support libraries, Jack/Jill compiler binaries, and render-script inputs used by this Android 7 build. Kati also parses unselected modules and requires older API directories to validate their `LOCAL_SDK_VERSION` declarations. This is a build-input subset, not the standalone Android SDK. |
| `cmsdk/`, `devtools/`, `tools/` | Referenced CM API, manifest-merger and Java library inputs. |

Ubuntu 20.04 offers newer Clang, ARM cross-compiler, and Android SDK packages,
but they do not reproduce these exact historical build inputs. Pinned
downloads preserve the working-tree versions while leaving them out of the
upload set. The installation requires access to Android Gitiles and the
AOSPA and LineageOS GitHub repositories. Download failures are fatal and
leave existing installed projects untouched. Unused Clang/GCC variants,
Ninja, Go, emulator/IDE/test bundles, and unused NDK
platform/architecture copies are omitted from the installed subset. The
source-tree original was not modified.

The original XPerience product list also installed the debugger `gdbserver`
and the diagnostic/profiling tools `micro_bench`, `oprofiled`, `powertop`, and
`strace` into the system image. This staged release omits those packages and
the `gdbserver` prebuilt. The staged ROM images built successfully after these
omissions; device validation is ongoing.

The local staged builds completed `systemimage`, `bootimage`, and
`recoveryimage` after restoring production headers omitted during pruning and
correcting TWRP's `file_contexts` output path. The local prebuilt subset was
copied from the original checkout; the complete pinned-download installer has
not yet been exercised on a fresh host. Build outputs and installed prebuilts
were removed from the upload tree afterward. Unreferenced test and sample
directories were subsequently moved out of the upload tree; the fresh-VM
build is the validation gate for this final candidate.
