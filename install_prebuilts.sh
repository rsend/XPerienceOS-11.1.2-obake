#!/usr/bin/env bash
set -euo pipefail

# Fetch the exact prebuilt project revisions used by the working tree. These
# are build inputs installed after checkout, not source-release contents.
release_top=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)

projects=(
  'clang/host/linux-x86|https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86|45562a53b2a5eb7c6e8f400413d19a12c660d7b1'
  'gcc/linux-x86/arm/arm-linux-androideabi-4.9|https://github.com/AOSPA/android_prebuilts_gcc_linux-x86_arm_arm-linux-androideabi-4.9.git|4aeb413848eb4cb41bcc85e243c636b33e48e6d4'
  'gcc/linux-x86/host/x86_64-linux-glibc2.15-4.8|https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.15-4.8|73ca99196723f810dad42390d154654354f57c16'
  'misc|https://android.googlesource.com/platform/prebuilts/misc|6287eb468c5772f5da2c468396bdf0068a4fa2d5'
  'ndk|https://android.googlesource.com/platform/prebuilts/ndk|f7e665a1af37619e136cb2b998c076e5316fe937'
  'sdk|https://android.googlesource.com/platform/prebuilts/sdk|23f7de5ba433fc82269ba63c203fe0a3eaf4a680'
  'cmsdk|https://github.com/LineageOS/android_prebuilts_cmsdk.git|874451b0ed05c5bb197862ef0b3d149e176f641a'
  'devtools|https://android.googlesource.com/platform/prebuilts/devtools|d054448a1147fc5294089b6ac7aa3abe92202761'
  'tools|https://android.googlesource.com/platform/prebuilts/tools|b5022f03051bf5ad63112659132e3ec0fa6af4ce'
)

for entry in "${projects[@]}"; do
    IFS='|' read -r path url revision <<< "$entry"
    destination="$release_top/prebuilts/$path"
    filelist="$release_top/prebuilt-filelists/${path//\//_}.txt"
    if [[ ! -s "$filelist" ]]; then
        echo "Missing build-input inventory: $filelist" >&2
        exit 1
    fi
    if [[ -d "$destination" ]]; then
        if [[ -f "$destination/.xpe-installed-revision" ]] &&
           [[ "$(<"$destination/.xpe-installed-revision")" == "$revision" ]]; then
            continue
        fi
        echo "Refusing to replace existing $destination; expected an empty release checkout or a matching installed revision." >&2
        exit 1
    fi

    work_dir=$(mktemp -d "$release_top/.prebuilt-install.XXXXXX")
    if ! (
        set -euo pipefail
        mkdir -p "$work_dir/repo" "$work_dir/upstream" "$work_dir/files"
        git -C "$work_dir/repo" init -q
        timeout 3600 git -C "$work_dir/repo" -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=60 \
            fetch -q --depth=1 "$url" "$revision"
        fetched=$(git -C "$work_dir/repo" rev-parse FETCH_HEAD)
        [[ "$fetched" == "$revision" ]]
        git -C "$work_dir/repo" archive "$revision" | tar -xf - -C "$work_dir/upstream"
        rsync -a --files-from="$filelist" "$work_dir/upstream/" "$work_dir/files/"
        printf '%s\n' "$revision" > "$work_dir/files/.xpe-installed-revision"
        mkdir -p "$(dirname "$destination")"
        mv "$work_dir/files" "$destination"
    ); then
        rm -r -- "$work_dir"
        echo "Failed to install pinned prebuilt project: $path ($revision)" >&2
        exit 1
    fi
    rm -r -- "$work_dir"
    echo "Installed $path at $revision"
done
