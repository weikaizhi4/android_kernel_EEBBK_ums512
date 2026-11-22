#!/usr/bin/env bash
set -euo pipefail

# Targets:
#   kernel  build Image and modules only
#   recovery build Image, patch socko, and generate one recovery upgrade zip
#   socko/all aliases for recovery (kept for backwards compatibility)
# Profiles are selected with P21H170_PROFILE and P21H170_DEFCONFIG.  The
# default profile builds ums512-p21h170_defconfig.  p21h170 ships a single
# profile: the Droidspaces defconfig only exists for p22h190.
TARGET="${1:-all}"
if [[ "$TARGET" == --target=* ]]; then
    TARGET="${TARGET#--target=}"
fi
case "$TARGET" in
    kernel|recovery|socko|all)
        ;;
    -h|--help|help)
        printf 'usage: %s [kernel|recovery|socko|all]\n' "$0"
        printf '\n'
        printf '  kernel  build Image/modules and publish kernel-output\n'
        printf '  recovery build and package one recovery upgrade zip\n'
        printf '  socko/all aliases for recovery (default)\n'
        printf '\n'
        printf '  Environment overrides:\n'
        printf '    P21H170_DEFCONFIG=ums512-p21h170_defconfig \\\n'
        printf '    P21H170_BUILD_DIR=out/ums512-p21h170 \\\n'
        printf '    %s all\n' "$0"
        exit 0
        ;;
    *)
        printf 'error: unknown target: %s\n' "$TARGET" >&2
        printf 'usage: %s [kernel|recovery|socko|all]\n' "$0" >&2
        exit 2
        ;;
esac

SRC_DIR="${P21H170_SOURCE_DIR:-$(cd "$(dirname "$0")/.." && pwd -P)}"
PROFILE="${P21H170_PROFILE:-p21h170}"
DEFCONFIG="${P21H170_DEFCONFIG:-${DEFCONFIG:-ums512-p21h170_defconfig}}"
if [[ "$PROFILE" == "p21h170" ]]; then
    PROFILE_SUFFIX=""
else
    PROFILE_SUFFIX="-$PROFILE"
fi
OUT_DIR="${P21H170_BUILD_DIR:-$SRC_DIR/out/ums512-p21h170${PROFILE_SUFFIX}}"
DEST_DIR="${P21H170_OUTPUT_DIR:-$SRC_DIR/out/release${PROFILE_SUFFIX}}"
# Toolchain: match the local LineageOS ROM kernel build, i.e. the clang 17
# prebuilt (r487747c) plus the AOSP/Lineage aarch64 GCC 4.9 binutils, with
# CROSS_COMPILE=aarch64-linux-android- and CLANG_TRIPLE=aarch64-linux-gnu-.
TC_DIR="${ANDROID_CLANG_DIR:-$SRC_DIR/out/toolchains/clang-r487747c/bin}"
GCC_DIR="${ANDROID_GCC_DIR:-$SRC_DIR/out/toolchains/aarch64-linux-android-4.9}"
CCPREFIX="${CROSS_COMPILE:-aarch64-linux-android-}"
MODULE_METADATA_PATCHER="${MODULE_METADATA_PATCHER:-$SRC_DIR/tools/patch-module-metadata.pl}"
SOCKO_TEMPLATE="${SOCKO_TEMPLATE:-$SRC_DIR/prebuilts/socko.factory.img}"
ANYKERNEL_DIR="${ANYKERNEL_DIR:-$SRC_DIR/out/AnyKernel3}"
RECOVERY_SCRIPT_TEMPLATE="${RECOVERY_SCRIPT_TEMPLATE:-$SRC_DIR/scripts/p21h170-recovery-anykernel.sh.in}"
FUSERMOUNT="${FUSERMOUNT:-$(command -v fusermount || command -v fusermount3 || true)}"

if [[ ! -f "$SRC_DIR/arch/arm64/configs/$DEFCONFIG" ]]; then
    printf 'error: defconfig not found: %s\n' "$SRC_DIR/arch/arm64/configs/$DEFCONFIG" >&2
    exit 1
fi

if [[ ! -x "$TC_DIR/clang" || ! -x "$TC_DIR/ld.lld" ]]; then
    printf 'error: Android clang toolchain not found in %s\n' "$TC_DIR" >&2
    exit 1
fi

if [[ -d "$GCC_DIR/bin" ]]; then
    export PATH="$GCC_DIR/bin:$PATH"
fi

if ! command -v "${CCPREFIX}ld" >/dev/null 2>&1; then
    printf 'error: %s binutils are not in PATH (set ANDROID_GCC_DIR)\n' "$CCPREFIX" >&2
    exit 1
fi

if [[ "$TARGET" != "kernel" ]]; then
    for command_name in "${CCPREFIX}objcopy" modinfo modprobe fuse2fs e2fsck zip; do
        if ! command -v "$command_name" >/dev/null 2>&1; then
            printf 'error: required command not found: %s\n' "$command_name" >&2
            exit 1
        fi
    done

    if [[ -z "$FUSERMOUNT" ]]; then
        printf 'error: fusermount or fusermount3 is required\n' >&2
        exit 1
    fi

    if [[ ! -f "$SOCKO_TEMPLATE" ]]; then
        printf 'error: socko template not found: %s\n' "$SOCKO_TEMPLATE" >&2
        exit 1
    fi

    if [[ ! -f "$MODULE_METADATA_PATCHER" ]]; then
        printf 'error: module metadata patcher not found: %s\n' "$MODULE_METADATA_PATCHER" >&2
        exit 1
    fi

    if [[ ! -f "$RECOVERY_SCRIPT_TEMPLATE" ]]; then
        printf 'error: recovery installer template not found: %s\n' "$RECOVERY_SCRIPT_TEMPLATE" >&2
        exit 1
    fi
fi

export PATH="$TC_DIR:$PATH"

# Keep the release string stable so the kernel and factory modules use the
# exact same vermagic across reproducible builds.
# Cosmetic only: the effective version comes from CONFIG_LOCALVERSION in the
# defconfig; read it so logs match the build.
KERNEL_LOCALVERSION="${KERNEL_LOCALVERSION:-$(sed -n 's/^CONFIG_LOCALVERSION="\(.*\)"/\1/p' \
    "$SRC_DIR/arch/arm64/configs/$DEFCONFIG" | head -1)}"
KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-twodays}"
KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-P21H170-build}"
BUILD_TIME="${KBUILD_BUILD_TIMESTAMP:-$(date -u '+%Y-%m-%d %H:%M:%S UTC')}"
GIT_HASH="${GIT_HASH:-$(git -C "$SRC_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
KERNEL_AUTHOR="$KBUILD_BUILD_USER"

MAKE_ARGS=(
    O="$OUT_DIR"
    ARCH=arm64
    CC=clang
    LD=ld.lld
    # clang 17 emits DWARF-5 .file directives that the AOSP/Lineage GCC 4.9
    # assembler (binutils 2.27) cannot parse; without this the kernel Makefile
    # adds -no-integrated-as and every object fails to assemble. The local
    # Lineage kernel build passes LLVM_IAS=1 too.
    LLVM_IAS=1
    CROSS_COMPILE="${CCPREFIX}"
    CLANG_TRIPLE=aarch64-linux-gnu-
    KBUILD_BUILD_USER="$KBUILD_BUILD_USER"
    KBUILD_BUILD_HOST="$KBUILD_BUILD_HOST"
    KBUILD_BUILD_TIMESTAMP="$BUILD_TIME"
)

JOBS="${JOBS:-$(nproc)}"
# The module metadata patcher needs an objcopy that can dump/update sections;
# prefer the one shipped with the clang toolchain, else the Android binutils.
OBJCOPY="${OBJCOPY:-$(command -v llvm-objcopy || echo "${CCPREFIX}objcopy")}"

SOCKO_MODULES=(
    drivers/gpu/arm/midgard/mali_gondul.ko
    drivers/input/misc/vl53L0/stmvl53l0.ko
    drivers/npu/vdsp/sprd_vdsp.ko
    drivers/wcn/bluetooth/driver/sprdbt_tty.ko
    drivers/wcn/fm/driver/sprd_fm.ko
    drivers/wcn/wlan/sprdwl_ng.ko
    drivers/camera/core/sprd_camera.ko
    drivers/camera/cpp/sprd_cpp.ko
    drivers/camera/fd/sprd_fd.ko
    drivers/camera/flash/flash_drv/sprd_flash_drv.ko
    drivers/camera/flash/ocp8137/flash_ic_ocp8137.ko
    drivers/camera/mmdvfs/mmdvfs.ko
    drivers/camera/sensor/sprd_sensor.ko
    drivers/camera/flash/sc2703/flash_ic_sc2703.ko
    drivers/input/misc/tcs3430/tcs3430.ko
)

printf 'profile: %s\n' "$PROFILE"
printf 'defconfig: %s\n' "$DEFCONFIG"
printf 'target: %s\n' "$TARGET"
printf '%s\n' "[1/4] generating $DEFCONFIG"
make -C "$SRC_DIR" "${MAKE_ARGS[@]}" "$DEFCONFIG"

printf '%s\n' "[2/4] building Image and modules with -j$JOBS ($KERNEL_LOCALVERSION)"
make -C "$SRC_DIR" "${MAKE_ARGS[@]}" -j"$JOBS"

IMAGE="$OUT_DIR/arch/arm64/boot/Image"
if [[ ! -f "$IMAGE" ]]; then
    printf 'error: build completed without %s\n' "$IMAGE" >&2
    exit 1
fi
KERNEL_RELEASE="$(make -C "$SRC_DIR" "${MAKE_ARGS[@]}" -s kernelrelease)"
if [[ ! -f "$OUT_DIR/Module.symvers" ]]; then
    printf 'error: build completed without %s\n' "$OUT_DIR/Module.symvers" >&2
    exit 1
fi
KERNEL_VERMAGIC=""
if [[ "$TARGET" != "kernel" ]]; then
    REFERENCE_MODULE="$(find "$OUT_DIR" -type f -path '*/kernel/*.ko' -print -quit 2>/dev/null || true)"
    if [[ -z "$REFERENCE_MODULE" ]]; then
        printf 'error: no built kernel module available to determine vermagic\n' >&2
        exit 1
    fi
    KERNEL_VERMAGIC="$(modinfo -F vermagic "$REFERENCE_MODULE")"
    if [[ -z "$KERNEL_VERMAGIC" ]]; then
        printf 'error: cannot read vermagic from %s\n' "$REFERENCE_MODULE" >&2
        exit 1
    fi
fi

STAGE="$(mktemp -d /tmp/p21h170-output.XXXXXX)"
SOCKO_MOUNT="$STAGE/socko-mount"
SOCKO_MOUNTED=0
cleanup() {
    if [[ "$SOCKO_MOUNTED" == "1" ]]; then
        "$FUSERMOUNT" -u "$SOCKO_MOUNT" 2>/dev/null || true
    fi
    rm -rf "$STAGE"
}
trap cleanup EXIT
mkdir -p "$STAGE/modules"
mkdir -p "$STAGE/socko-modules"

cp -f "$IMAGE" "$STAGE/Image"
cp -f "$OUT_DIR/System.map" "$STAGE/System.map"
cp -f "$OUT_DIR/.config" "$STAGE/.config"

while IFS= read -r -d '' module; do
    relative="${module#"$OUT_DIR/"}"
    mkdir -p "$STAGE/modules/$(dirname "$relative")"
    cp -f "$module" "$STAGE/modules/$relative"
done < <(find "$OUT_DIR" -type f -name '*.ko' -print0)

if [[ "$TARGET" == "kernel" ]]; then
    {
        printf 'source=%s\n' "$SRC_DIR"
        printf 'output=%s\n' "$OUT_DIR"
        printf 'profile=%s\n' "$PROFILE"
        printf 'config=%s\n' "$DEFCONFIG"
        printf 'target=%s\n' "$TARGET"
        printf 'clang=%s\n' "$TC_DIR/clang"
        printf 'kernel_release=%s\n' "$KERNEL_RELEASE"
        printf 'git_hash=%s\n' "$GIT_HASH"
        printf 'built_at=%s\n' "$(date -Is)"
        sha256sum "$STAGE/Image"
    } > "$STAGE/build-info.txt"

    mkdir -p "$DEST_DIR"
    rm -rf "$DEST_DIR/modules.new"
    mv "$STAGE/modules" "$DEST_DIR/modules.new"
    cp -f "$STAGE/Image" "$DEST_DIR/Image"
    cp -f "$STAGE/System.map" "$DEST_DIR/System.map"
    cp -f "$STAGE/.config" "$DEST_DIR/.config"
    cp -f "$STAGE/build-info.txt" "$DEST_DIR/build-info.txt"
    rm -rf "$DEST_DIR/modules"
    mv "$DEST_DIR/modules.new" "$DEST_DIR/modules"

    printf '%s\n' '[3/4] kernel artifacts'
    stat -c '%n %s bytes' "$DEST_DIR/Image"
    printf 'modules: '
    find "$DEST_DIR/modules" -type f -name '*.ko' | wc -l
    sha256sum "$DEST_DIR/Image"
    printf 'output: %s\n' "$DEST_DIR"
    exit 0
fi

for relative in "${SOCKO_MODULES[@]}"; do
    module="$OUT_DIR/$relative"
    if [[ ! -f "$module" ]]; then
        printf 'error: required socko module not found: %s\n' "$module" >&2
        exit 1
    fi
    cp -f "$module" "$STAGE/socko-modules/$(basename "$relative")"
done

printf '%s\n' '[3/4] rebuilding socko image with compatible modules'
cp -f "$SOCKO_TEMPLATE" "$STAGE/socko.img"
mkdir -p "$SOCKO_MOUNT"
fuse2fs -o fakeroot "$STAGE/socko.img" "$SOCKO_MOUNT"
SOCKO_MOUNTED=1

declare -A BUILT_SOCKO_MODULES=()
for relative in "${SOCKO_MODULES[@]}"; do
    BUILT_SOCKO_MODULES["$(basename "$relative")"]="$OUT_DIR/$relative"
done

FACTORY_MODULES=()
while IFS= read -r -d '' module; do
    module_name="$(basename "$module")"
    if [[ -n "${BUILT_SOCKO_MODULES[$module_name]+present}" ]]; then
        continue
    fi
    cp -f "$module" "$STAGE/socko-modules/$module_name"
    FACTORY_MODULES+=("$STAGE/socko-modules/$module_name")
done < <(find "$SOCKO_MOUNT" -maxdepth 1 -type f -name '*.ko' -print0)

if ((${#FACTORY_MODULES[@]})); then
    perl "$MODULE_METADATA_PATCHER" \
        "$OUT_DIR/Module.symvers" "$KERNEL_VERMAGIC" "$OBJCOPY" \
        "${FACTORY_MODULES[@]}"
fi

for relative in "${SOCKO_MODULES[@]}"; do
    module_name="$(basename "$relative")"
    target="$SOCKO_MOUNT/$module_name"
    if [[ ! -f "$target" ]]; then
        printf 'error: socko template is missing module: %s\n' "$module_name" >&2
        exit 1
    fi
    cp -f "${BUILT_SOCKO_MODULES[$module_name]}" "$target"
done

for module in "${FACTORY_MODULES[@]}"; do
    cp -f "$module" "$SOCKO_MOUNT/$(basename "$module")"
done

"$FUSERMOUNT" -u "$SOCKO_MOUNT"
SOCKO_MOUNTED=0
e2fsck -fn "$STAGE/socko.img" >/dev/null
printf '%s\n' '[4/5] building vendor_dlkm image'
VDLKM_ROOT="$STAGE/vendor_dlkm"
VDLKM_MODDIR="$VDLKM_ROOT/lib/modules"
mkdir -p "$VDLKM_MODDIR" "$VDLKM_ROOT/etc"

# vendor_dlkm is the second module location: an ext4 partition mounted at
# /vendor_dlkm with the .ko files flat in lib/modules (see
# BOARD_VENDOR_DLKMIMAGE_FILE_SYSTEM_TYPE in the device tree).  It only exists
# on the LineageOS-style layout, so the installer flashes it when the partition
# is there and skips it otherwise.
#
# Seed from the AOSP build tree when it is reachable: modules that only ship as
# vendor prebuilts (fingerprint, touch) and etc/build.prop have to survive.
VENDOR_DLKM_BASE="${VENDOR_DLKM_BASE:-$SRC_DIR/../../../out/target/product/$PROFILE/vendor_dlkm}"
if [[ -d "$VENDOR_DLKM_BASE/lib/modules" ]]; then
    cp -a "$VENDOR_DLKM_BASE/lib/modules/." "$VDLKM_MODDIR/"
    if [[ -f "$VENDOR_DLKM_BASE/etc/build.prop" ]]; then
        cp -f "$VENDOR_DLKM_BASE/etc/build.prop" "$VDLKM_ROOT/etc/build.prop"
    fi
    printf 'vendor_dlkm: seeded from %s\n' "$VENDOR_DLKM_BASE"
else
    printf 'vendor_dlkm: no base tree at %s; image carries this kernel modules only\n' \
        "$VENDOR_DLKM_BASE"
fi

VENDOR_DLKM_MODULES=0
while IFS= read -r -d '' module; do
    cp -f "$module" "$VDLKM_MODDIR/$(basename "$module")"
    VENDOR_DLKM_MODULES=$((VENDOR_DLKM_MODULES + 1))
done < <(find "$OUT_DIR" -type f -name '*.ko' -print0)

# Rebuild the module index for the merged set.  depmod wants a versioned
# directory and emits bare names, while vendor_dlkm keeps the files flat and
# indexes them by absolute path.
DEPMOD_ROOT="$STAGE/vendor_dlkm-depmod"
rm -rf "$DEPMOD_ROOT"
mkdir -p "$DEPMOD_ROOT/lib/modules/$KERNEL_RELEASE"
cp -f "$VDLKM_MODDIR"/*.ko "$DEPMOD_ROOT/lib/modules/$KERNEL_RELEASE/"
depmod -b "$DEPMOD_ROOT" -a "$KERNEL_RELEASE" >/dev/null 2>&1 || true
for index in modules.dep modules.alias modules.softdep; do
    [[ -f "$DEPMOD_ROOT/lib/modules/$KERNEL_RELEASE/$index" ]] || continue
    sed -E 's@(^|: )([^ :]+\.ko)@\1/vendor_dlkm/lib/modules/\2@g' \
        "$DEPMOD_ROOT/lib/modules/$KERNEL_RELEASE/$index" > "$VDLKM_MODDIR/$index"
done
if [[ ! -f "$VDLKM_MODDIR/modules.load" ]]; then
    printf '%s\n' mali_gondul kheaders \
        trace_irqsoff_bytedancy trace_noschedule_bytedancy \
        trace_runqlat_bytedancy incrementalfs > "$VDLKM_MODDIR/modules.load"
fi

chmod -R u=rwX,go=rX "$VDLKM_ROOT"
VENDOR_DLKM_IMG="$STAGE/vendor_dlkm.img"
rm -f "$VENDOR_DLKM_IMG"
mke2fs -q -F -t ext4 -L vendor_dlkm -b 4096 -I 256 \
    -O ^has_journal,^resize_inode -m 0 -d "$VDLKM_ROOT" \
    "$VENDOR_DLKM_IMG" "${VENDOR_DLKM_SIZE:-64M}"
e2fsck -fn "$VENDOR_DLKM_IMG" >/dev/null
printf 'vendor_dlkm: %s module(s), image %s bytes\n' \
    "$VENDOR_DLKM_MODULES" "$(stat -c %s "$VENDOR_DLKM_IMG")"

printf '%s\n' '[5/5] packaging recovery upgrade zip'
if [[ ! -f "$ANYKERNEL_DIR/anykernel.sh" ]]; then
    printf 'error: AnyKernel3 template not found: %s\n' "$ANYKERNEL_DIR" >&2
    exit 1
fi
AK_STAGE="$STAGE/AnyKernel3"
cp -a "$ANYKERNEL_DIR/." "$AK_STAGE/"
rm -rf "$AK_STAGE/.git" "$AK_STAGE/.github"
cp -f "$STAGE/Image" "$AK_STAGE/Image"
mkdir -p "$DEST_DIR"
cp -f "$STAGE/socko.img" "$AK_STAGE/socko.img"
cp -f "$STAGE/vendor_dlkm.img" "$AK_STAGE/vendor_dlkm.img"
cp -f "$RECOVERY_SCRIPT_TEMPLATE" "$AK_STAGE/anykernel.sh"

export AK_KERNEL_RELEASE="$KERNEL_RELEASE"
export AK_KERNEL_AUTHOR="$KERNEL_AUTHOR"
export AK_BUILD_TIME="$BUILD_TIME"
perl -0pi -e \
    's/\@KERNEL_RELEASE\@/$ENV{AK_KERNEL_RELEASE}/g; \
     s/\@KERNEL_AUTHOR\@/$ENV{AK_KERNEL_AUTHOR}/g; \
     s/\@BUILD_TIME\@/$ENV{AK_BUILD_TIME}/g' \
    "$AK_STAGE/anykernel.sh"
chmod 0755 "$AK_STAGE/anykernel.sh"

if [[ ! -f "$AK_STAGE/tools/ak3-core.sh" ]]; then
    printf 'error: AnyKernel3 boot unpack tool is missing: %s/tools/ak3-core.sh\n' "$AK_STAGE" >&2
    exit 1
fi

cat > "$AK_STAGE/build-info.txt" <<EOF
device=EEBBK P21H170
profile=$PROFILE
defconfig=$DEFCONFIG
kernel_release=$KERNEL_RELEASE
kernel_author=$KERNEL_AUTHOR
build_time=$BUILD_TIME
git_hash=$GIT_HASH
socko_target=/dev/block/by-name/socko
vendor_dlkm_target=/dev/block/mapper/vendor_dlkm
EOF

RECOVERY_OUTPUT="$DEST_DIR/P21H170-recovery${PROFILE_SUFFIX}-$KERNEL_RELEASE.zip"
rm -f "$DEST_DIR"/*.zip "$DEST_DIR"/*.img "$DEST_DIR"/Image \
      "$DEST_DIR"/System.map "$DEST_DIR"/.config "$DEST_DIR"/build-info.txt \
      "$DEST_DIR"/SHA256SUMS
rm -rf "$DEST_DIR/modules" "$DEST_DIR/socko-modules"
(
    cd "$AK_STAGE"
    zip -q -r -9 "$RECOVERY_OUTPUT" .
)

{
    printf 'source=%s\n' "$SRC_DIR"
    printf 'output=%s\n' "$OUT_DIR"
    printf 'profile=%s\n' "$PROFILE"
    printf 'config=%s\n' "$DEFCONFIG"
    printf 'target=%s\n' "$TARGET"
    printf 'clang=%s\n' "$TC_DIR/clang"
    printf 'kernel_release=%s\n' "$KERNEL_RELEASE"
    printf 'kernel_author=%s\n' "$KERNEL_AUTHOR"
    printf 'build_time=%s\n' "$BUILD_TIME"
    printf 'git_hash=%s\n' "$GIT_HASH"
    printf 'socko_modules=%s\n' "${#SOCKO_MODULES[@]}"
    printf 'socko_factory_modules=%s\n' "${#FACTORY_MODULES[@]}"
    printf 'socko_template=%s\n' "$SOCKO_TEMPLATE"
    printf 'socko_target=/dev/block/by-name/socko\n'
    printf 'vendor_dlkm_modules=%s\n' "$VENDOR_DLKM_MODULES"
    printf 'vendor_dlkm_target=%s\n' '/dev/block/mapper/vendor_dlkm'
    printf 'recovery_output=%s\n' "$RECOVERY_OUTPUT"
    sha256sum "$STAGE/Image"
    sha256sum "$STAGE/socko.img"
    sha256sum "$RECOVERY_OUTPUT"
} > "$STAGE/build-info.txt"

printf '%s\n' '[5/5] recovery artifact'
stat -c '%n %s bytes' "$RECOVERY_OUTPUT"
sha256sum "$RECOVERY_OUTPUT"
printf 'output: %s\n' "$DEST_DIR"
printf 'recovery: %s\n' "$RECOVERY_OUTPUT"
