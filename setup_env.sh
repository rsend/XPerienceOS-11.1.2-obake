#!/usr/bin/env bash

# Source this file from Bash before using make or m. Paths are derived from
# this checkout; no path from the machine that prepared the release is needed.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "Source this file: source ./setup_env.sh rom|recovery" >&2
    exit 2
fi

if [[ $# -ne 1 || ( "$1" != rom && "$1" != recovery ) ]]; then
    echo "Usage: source ./setup_env.sh rom|recovery" >&2
    return 2
fi

release_mode=$1
release_top=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P) || return 1
cd "$release_top" || return 1
export TOP="$release_top"

unset LC_ALL
unset LC_TIME LC_CTYPE

release_javac=$(dpkg -L openjdk-8-jdk-headless 2>/dev/null | sed -n '/\/bin\/javac$/p' | head -n 1)
if [[ -z "$release_javac" || ! -x "$release_javac" ]]; then
    echo "OpenJDK 8 is missing; run bash ./install_deps.sh first." >&2
    return 1
fi
export JAVA_HOME="${release_javac%/bin/javac}"
if [[ ! -x /usr/bin/python ]] || ! /usr/bin/python -c 'import sys; assert sys.version_info[0] == 2' 2>/dev/null; then
    echo "Python 2 is required at /usr/bin/python by checked-in GCC wrappers." >&2
    return 1
fi
export PATH="$JAVA_HOME/bin:$release_top/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin:$PATH"
for release_tool in ninja flex m4; do
    if ! command -v "$release_tool" >/dev/null; then
        echo "$release_tool is missing; run bash ./install_deps.sh first." >&2
        return 1
    fi
done
if [[ ! -x "$release_top/prebuilts/misc/linux-x86/flex/flex-2.5.39" ]]; then
    echo "Pinned Flex 2.5.39 is missing; run bash ./install_deps.sh first." >&2
    return 1
fi
# envsetup defines m and other Android build helpers. Product selection is
# local and explicit, avoiding lunch's upstream roomservice lookup.
source "$release_top/build/envsetup.sh" || return 1
export XPE_BUILD=obake TARGET_PRODUCT=xpe_obake TARGET_BUILD_TYPE=release
# The release contains image inputs, not libcore's optional test suites.
export LIBCORE_SKIP_TESTS=true

if [[ "$release_mode" == rom ]]; then
    export OUT_DIR="$release_top/out-release"
    export TARGET_BUILD_VARIANT=user WITH_TWRP=false
else
    export OUT_DIR="$release_top/out-twrp-userdebug"
    export TARGET_BUILD_VARIANT=userdebug WITH_TWRP=true
fi

echo "Configured $TARGET_PRODUCT ($TARGET_BUILD_VARIANT), OUT_DIR=$OUT_DIR"
unset release_mode release_top release_javac release_tool
