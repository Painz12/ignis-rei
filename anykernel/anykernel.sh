### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers
## Ignis-Rei Kernel for Xiaomi POCO F4 / Redmi K40S (munch)

### AnyKernel setup
properties() { '
kernel.string=Ignis-Rei Kernel | munch
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=munch
device.name2=munchin
device.name3=22021211RC
device.name4=22021211RI
device.name5=22021211RG
device.name6=POCO F4
device.name7=Redmi K40S
supported.versions=11-17
supported.patchlevels=
'; } # end properties

### AnyKernel install
BLOCK=/dev/block/bootdevice/by-name/boot;
block=/dev/block/bootdevice/by-name/boot;
IS_SLOT_DEVICE=auto;
is_slot_device=auto;
RAMDISK_COMPRESSION=auto;
ramdisk_compression=auto;
PATCH_VBMETA_FLAG=auto;
patch_vbmeta_flag=auto;

. tools/ak3-core.sh;

AKHOME="${AKHOME:-${home:-$PWD}}";
home="$AKHOME";

ui_print " ";
ui_print "  ===========================================";
ui_print "  |       Ignis-Rei Kernel | munch          |";
ui_print "  |          Android 11 - 17 Ready          |";
ui_print "  ===========================================";
ui_print " ";

# Check if dual images are available
if [ -f "$AKHOME/kernels/aosp/Image" ] && [ -f "$AKHOME/kernels/stock/Image" ]; then
    CHOSEN_VARIANT="";

    # 1. Check for manual override flags
    if [ -f /tmp/aosp ] || [ -f /sdcard/aosp ] || [ -f /data/aosp ]; then
        CHOSEN_VARIANT="aosp";
        ui_print "  -> [Override Flag] AOSP selected.";
    elif [ -f /tmp/stock ] || [ -f /sdcard/stock ] || [ -f /data/stock ] || \
         [ -f /tmp/miui ] || [ -f /sdcard/miui ] || [ -f /tmp/hyperos ] || [ -f /sdcard/hyperos ]; then
        CHOSEN_VARIANT="stock";
        ui_print "  -> [Override Flag] Stock selected.";
    fi;

    # 2. If booted in Android (e.g. KernelSU / FK Manager), auto-detect via getprop
    if [ -z "$CHOSEN_VARIANT" ] && [ "$BOOTMODE" = "true" ]; then
        if command -v getprop >/dev/null 2>&1; then
            OS_NAME=$(getprop ro.mi.os.version.name 2>/dev/null);
            MIUI_NAME=$(getprop ro.miui.ui.version.name 2>/dev/null);
            BUILD_INC=$(getprop ro.build.version.incremental 2>/dev/null);
            if [ -n "$OS_NAME" ] || [ -n "$MIUI_NAME" ] || echo "$BUILD_INC" | grep -q -E "^V[0-9]|^OS[0-9]"; then
                CHOSEN_VARIANT="stock";
                ui_print "  -> [Live Detection] Xiaomi HyperOS / MIUI detected.";
            else
                CHOSEN_VARIANT="aosp";
                ui_print "  -> [Live Detection] AOSP / Custom ROM detected.";
            fi;
        fi;
    fi;

    # 3. Interactive Volume Button Selector (Recovery Mode)
    if [ -z "$CHOSEN_VARIANT" ]; then
        ui_print "  -----------------------------------------";
        ui_print "  CHOOSE KERNEL VARIANT (10s timeout):";
        ui_print "  -> Press [VOLUME UP]   : AOSP (A11-A17 Custom ROMs)";
        ui_print "  -> Press [VOLUME DOWN] : Stock (MIUI / HyperOS)";
        ui_print "  -----------------------------------------";

        START_TIME=$(date +%s);
        TIMEOUT=10;

        # Key listener loop
        while [ -z "$CHOSEN_VARIANT" ]; do
            NOW=$(date +%s);
            ELAPSED=$((NOW - START_TIME));
            if [ $ELAPSED -ge $TIMEOUT ]; then
                ui_print "  [Timeout] No key pressed. Defaulting to AOSP...";
                CHOSEN_VARIANT="aosp";
                break;
            fi;

            # Check key event via getevent
            if command -v getevent >/dev/null 2>&1; then
                KEY_EVENT=$(getevent -l -c 1 2>/dev/null);
                if echo "$KEY_EVENT" | grep -qi "KEY_VOLUMEUP.*DOWN"; then
                    CHOSEN_VARIANT="aosp";
                    ui_print "  -> [VOL+] Pressed: Selected AOSP Kernel.";
                    break;
                elif echo "$KEY_EVENT" | grep -qi "KEY_VOLUMEDOWN.*DOWN"; then
                    CHOSEN_VARIANT="stock";
                    ui_print "  -> [VOL-] Pressed: Selected Stock Kernel (MIUI/HyperOS).";
                    break;
                fi;
            else
                # Fallback if getevent is unavailable
                CHOSEN_VARIANT="aosp";
                break;
            fi;
            sleep 0.1;
        done;
    fi;

    # Apply selected kernel Image (DO NOT touch DTB or DTBO to preserve ROM panel/touch calibration)
    if [ "$CHOSEN_VARIANT" = "stock" ]; then
        ui_print "  -> Flashing Stock Kernel (MIUI / HyperOS)...";
        cp -f "$AKHOME/kernels/stock/Image" "$AKHOME/Image";
    else
        ui_print "  -> Flashing AOSP Kernel (Custom ROMs)...";
        cp -f "$AKHOME/kernels/aosp/Image" "$AKHOME/Image";
    fi;
elif [ -f "$AKHOME/kernels/stock/Image" ]; then
    ui_print "  -> Target: Stock MIUI / HyperOS";
    cp -f "$AKHOME/kernels/stock/Image" "$AKHOME/Image";
elif [ -f "$AKHOME/kernels/aosp/Image" ]; then
    ui_print "  -> Target: AOSP / Custom ROM";
    cp -f "$AKHOME/kernels/aosp/Image" "$AKHOME/Image";
fi;

# DO NOT touch DTB or DTBO (preserve panel, touchscreen, and display calibration from ROM)
rm -f "$AKHOME/dtb" "$AKHOME/dtbo.img" "$AKHOME/dtbo" 2>/dev/null;

# Verify that target Image exists before proceeding
[ -f "$AKHOME/Image" ] || abort "  ! Kernel Image not found in $AKHOME";

ui_print " ";
ui_print "  -> Splitting and flashing kernel to boot partition...";
split_boot;
flash_boot;

ui_print " ";
ui_print "  ===========================================";
ui_print "  |       Ignis-Rei flashed successfully!   |";
ui_print "  ===========================================";
ui_print " ";
## end install
