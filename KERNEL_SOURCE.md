# Kernel source, provenance, and credit

The kernel used for the obake `boot.img` and `recovery.img` is the Linux
3.4.91-based source tree at [`kernel/motorola/ghost/`](kernel/motorola/ghost/).
The complete modified kernel source is included in this release checkout.

The starting point was the XPerience project's
[`android_kernel_motorola_ghost` (`xpe-11.1`)](https://github.com/XPerience-AOSP-Lollipop/android_kernel_motorola_ghost/tree/xpe-11.1)
repository, `xpe-11.1` branch, at commit
[`5e94d50d6774309f56ea2c3250158fb3649a1a31`](https://github.com/XPerience-AOSP-Lollipop/android_kernel_motorola_ghost/commit/5e94d50d6774309f56ea2c3250158fb3649a1a31).
That identity is recorded in the original repo manifest and the local
`reconstruction/xpe711-initial-pinned.xml` audit record, retained outside
this standalone release. The XPerience tree incorporates work by Linux kernel,
Motorola, Qualcomm, and other contributors. Their copyright, authorship,
license, and attribution notices in the source files, along with the kernel's
[`COPYING`](kernel/motorola/ghost/COPYING),
[`CREDITS`](kernel/motorola/ghost/CREDITS), and
[`MAINTAINERS`](kernel/motorola/ghost/MAINTAINERS), are retained. This port
does not claim authorship of their work.

The historical checkout then added a local build-compatibility commit,
`c97e8c465ce94a1e478fa81c48d9f8624336fb5e` (`mm: fix
set_pageblock_order section annotation`), changing both definitions in
`mm/page_alloc.c` from `__init` to `__paginginit`. The section-mismatch audit
is retained locally at `reconstruction/KERNEL_SECTION_MISMATCH_AUDIT.md`,
outside this standalone release.
The obake port also has local changes in these kernel files:

- `drivers/media/video/msm/msm_vpe.c`
- `drivers/media/video/msm/msm_vpe1.c`
- `sound/soc/msm/msm-pcm-voice.c`
- `sound/soc/msm/qdsp6/q6voice.c`
- `sound/soc/msm/qdsp6/q6voice.h`

The original multi-project Git metadata was not carried into this flattened
release repository. Consequently, the two historical commit IDs identify
the starting tree and the recorded section-annotation fix; they do not by
themselves describe every difference in the final kernel. The source files
in this checkout are the authoritative corresponding source for binaries
built from this release. The modified kernel source is also available directly
at [the release repository's kernel tree](https://github.com/rsend/Xpe711-obake/tree/main/kernel/motorola/ghost)
once this version is published. Build configuration and scripts are included here;
see [`BUILD_ENVIRONMENT.md`](BUILD_ENVIRONMENT.md) and
[`setup_env.sh`](setup_env.sh). Use the release commit associated with a
distributed image to identify the exact source version for that image.

The Linux kernel is distributed under the GNU General Public License,
version 2, as stated in its `COPYING` file. Preserve all existing copyright
and license notices when redistributing it. This provenance note provides
credit and helps recipients locate the source; it does not replace the GPLv2
obligations for distributing a modified kernel binary, including providing
the complete corresponding source and build scripts.
