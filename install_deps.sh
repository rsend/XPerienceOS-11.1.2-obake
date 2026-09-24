#!/usr/bin/env bash
set -euo pipefail

# Install the Ubuntu 20.04 host-package baseline, fetch pinned historical
# build inputs, then build the Bison 2.7 host tool from this checkout.
if [[ ! -r /etc/os-release ]]; then
    echo "Cannot identify the host OS (/etc/os-release is missing)." >&2
    exit 1
fi
source /etc/os-release
if [[ "${ID:-}" != ubuntu || "${VERSION_ID:-}" != 20.04 ]]; then
    echo "This script supports Ubuntu 20.04 only; found ${PRETTY_NAME:-unknown}." >&2
    exit 1
fi
if [[ "$(dpkg --print-architecture)" != amd64 ]]; then
    echo "This build requires an amd64 Ubuntu host." >&2
    exit 1
fi

if (( EUID == 0 )); then
    echo "Run this script as your regular user; it invokes sudo only for Ubuntu packages." >&2
    exit 1
fi
if ! command -v sudo >/dev/null; then
    echo "Install sudo before using this script." >&2
    exit 1
fi
apt=(sudo apt-get)

"${apt[@]}" update
for release_pkg in openjdk-8-jdk python2 python-is-python2; do
    if ! apt-cache show "$release_pkg" >/dev/null 2>&1; then
        echo "Ubuntu package '$release_pkg' is unavailable; enable the focal universe repository and retry." >&2
        exit 1
    fi
done
"${apt[@]}" install -y \
    bc build-essential curl flex git gperf imagemagick \
    lib32stdc++6 lib32z1-dev libc6-dev-i386 libncurses-dev libncurses5 \
    libssl-dev libxml2-utils lzop m4 make ninja-build openjdk-8-jdk pngcrush \
    python-is-python2 python2 rsync schedtool unzip xsltproc zip zlib1g-dev

release_top=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
bash "$release_top/install_prebuilts.sh"
(
    cd "$release_top"
    # Legacy envsetup and make helpers are not compatible with Bash errexit
    # or nounset; keep their effects confined to this subshell.
    set +e +u
    source ./setup_env.sh rom || exit 1
    make -j4 bison || exit 1
    bison_version=$("$OUT_DIR/host/linux-x86/bin/bison" --version | head -n 1) || exit 1
    if [[ "$bison_version" != 'bison (GNU Bison) 2.7' ]]; then
        echo "Expected the in-tree Bison 2.7 build; found '$bison_version'." >&2
        exit 1
    fi
)

echo "Host packages and in-tree Bison 2.7 are ready. Source ./setup_env.sh rom or recovery next."
