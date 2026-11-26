#!/usr/bin/env bash
#
# Ignis-Rei Universal Kernel Build Script for POCO F4 / Redmi K40S (munch)
# Universal Variants: KSU (KernelSU-Next) & No-KSU (Rootless)
#

set -e

KERNEL_NAME="Ignis-Rei"
DEVICE="munch"
BUILD_TARGET="${1:-all}" # all, ksu, noksu
DATE=$(date +%Y%m%d-%H%M)
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"
OUT_DIR="${ROOT_DIR}/out"

# Setup Toolchain
if [ -n "$CLANG_PATH" ] && [ -d "$CLANG_PATH" ]; then
    TC_DIR="$CLANG_PATH"
elif [ -d "$HOME/toolchains/neutron" ]; then
    TC_DIR="$HOME/toolchains/neutron"
elif [ -d "/home/arun/toolchains/neutron" ]; then
    TC_DIR="/home/arun/toolchains/neutron"
elif [ -d "${ROOT_DIR}/clang" ]; then
    TC_DIR="${ROOT_DIR}/clang"
else
    echo "ERROR: Clang toolchain not found!"
    echo "Please set CLANG_PATH or install Neutron Clang in ~/toolchains/neutron"
    exit 1
fi

export PATH="${TC_DIR}/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export ARCH=arm64
export SUBARCH=arm64
export CC=clang
export LD=ld.lld
export AR=llvm-ar
export NM=llvm-nm
export OBJCOPY=llvm-objcopy
export OBJDUMP=llvm-objdump
export STRIP=llvm-strip
export HOSTCC=gcc
export HOSTCXX=g++
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-

CLANG_VER=$("${TC_DIR}/bin/clang" --version | head -n 1)

echo "================================================="
echo "  Kernel Name : ${KERNEL_NAME} (Universal)"
echo "  Device      : ${DEVICE} (POCO F4 / Redmi K40S)"
echo "  Target      : ${BUILD_TARGET}"
echo "  Compiler    : ${CLANG_VER}"
echo "  Date        : ${DATE}"
echo "================================================="

# Clean stale root artifacts
rm -f include/config/auto.conf .config

MAKE_FLAGS=(
    ARCH=arm64
    CC=clang
    LD=ld.lld
    AR=llvm-ar
    NM=llvm-nm
    OBJCOPY=llvm-objcopy
    OBJDUMP=llvm-objdump
    STRIP=llvm-strip
    HOSTCC=gcc
    HOSTCXX=g++
    CLANG_TRIPLE=aarch64-linux-gnu-
    CROSS_COMPILE=aarch64-linux-gnu-
    CROSS_COMPILE_ARM32=arm-linux-gnueabi-
)

build_kernel() {
    local variant="$1" # ksu or noksu
    local out_sub="${OUT_DIR}/${variant}"
    mkdir -p "${out_sub}"

    echo ">>> Generating defconfig for [${variant}]..."
    if [ "$variant" = "noksu" ]; then
        ./scripts/kconfig/merge_config.sh -m -O "${out_sub}" \
            arch/arm64/configs/vendor/kona-perf_defconfig \
            arch/arm64/configs/vendor/xiaomi/sm8250-common.config \
            arch/arm64/configs/vendor/xiaomi/munch.config \
            arch/arm64/configs/vendor/xiaomi/noksu.config
    else
        ./scripts/kconfig/merge_config.sh -m -O "${out_sub}" \
            arch/arm64/configs/vendor/kona-perf_defconfig \
            arch/arm64/configs/vendor/xiaomi/sm8250-common.config \
            arch/arm64/configs/vendor/xiaomi/munch.config \
            arch/arm64/configs/vendor/xiaomi/ksu.config
    fi
    make O="${out_sub}" "${MAKE_FLAGS[@]}" olddefconfig

    echo ">>> Compiling Kernel [${variant}]..."
    make -j$(nproc --all) O="${out_sub}" "${MAKE_FLAGS[@]}" Image

    if [ ! -f "${out_sub}/arch/arm64/boot/Image" ]; then
        echo "ERROR: Kernel compilation failed for ${variant}!"
        exit 1
    fi

    echo ">>> Kernel [${variant}] compiled successfully!"
}

package_variant() {
    local variant="$1" # ksu or noksu
    local variant_label="KSU"
    [ "$variant" = "noksu" ] && variant_label="NoKSU"

    echo ">>> Packaging flashable AnyKernel3 zip for [${variant_label}]..."
    local ak3_dir="${OUT_DIR}/AK3_${variant}"
    rm -rf "${ak3_dir}"
    mkdir -p "${ak3_dir}"

    if [ -d "${ROOT_DIR}/anykernel" ]; then
        cp -r "${ROOT_DIR}/anykernel/"* "${ak3_dir}/"
    fi

    # Stage kernel image as MUNCH-Image
    cp -f "${OUT_DIR}/${variant}/arch/arm64/boot/Image" "${ak3_dir}/MUNCH-Image"

    # Ensure anykernel.sh is present and executable
    if [ -f "${ROOT_DIR}/anykernel/anykernel.sh" ]; then
        cp -f "${ROOT_DIR}/anykernel/anykernel.sh" "${ak3_dir}/anykernel.sh"
        chmod +x "${ak3_dir}/anykernel.sh"
    fi

    # Clean loose files from staging (keep only META-INF, tools, anykernel.sh, MUNCH-Image)
    rm -rf "${ak3_dir}/kernels" "${ak3_dir}/dtb" "${ak3_dir}/dtbo.img" "${ak3_dir}/Image" "${ak3_dir}/.git" "${ak3_dir}/README.md" "${ak3_dir}/LICENSE" "${ak3_dir}/modules" "${ak3_dir}/patch" "${ak3_dir}/ramdisk"

    cd "${ak3_dir}"
    local zip_name="${KERNEL_NAME}-${DEVICE}-Universal-${variant_label}-${DATE}.zip"
    zip -r9 "${OUT_DIR}/${zip_name}" *

    echo "================================================="
    echo "  ${variant_label} Build Completed Successfully!"
    echo "  Zip Location: ${OUT_DIR}/${zip_name}"
    echo "================================================="

    # Copy to Windows Desktop and Downloads if running under WSL
    if [ -d "/mnt/c/Users/ARUN/Desktop" ]; then
        cp -f "${OUT_DIR}/${zip_name}" "/mnt/c/Users/ARUN/Desktop/${zip_name}"
        cp -f "${OUT_DIR}/${zip_name}" "/mnt/c/Users/ARUN/Desktop/${KERNEL_NAME}-${DEVICE}-${variant_label}.zip"
        echo "  Copied to Desktop: C:\Users\ARUN\Desktop\${zip_name}"
        echo "  Copied to Desktop: C:\Users\ARUN\Desktop\${KERNEL_NAME}-${DEVICE}-${variant_label}.zip"
    fi
    if [ -d "/mnt/c/Users/ARUN/Downloads" ]; then
        cp -f "${OUT_DIR}/${zip_name}" "/mnt/c/Users/ARUN/Downloads/${zip_name}"
        cp -f "${OUT_DIR}/${zip_name}" "/mnt/c/Users/ARUN/Downloads/${KERNEL_NAME}-${DEVICE}-${variant_label}.zip"
    fi
    cd "${ROOT_DIR}"
}

# Execute builds
case "$BUILD_TARGET" in
    ksu)
        build_kernel "ksu"
        package_variant "ksu"
        ;;
    noksu)
        build_kernel "noksu"
        package_variant "noksu"
        ;;
    all|both|*)
        build_kernel "ksu"
        package_variant "ksu"
        build_kernel "noksu"
        package_variant "noksu"
        ;;
esac
