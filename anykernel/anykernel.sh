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
# Modern AnyKernel3 uses UPPERCASE vars.
# setup_ak() (called at end of ak3-core.sh) auto-detects vendor_boot$SLOT and:
#   - Moves Image* -> boot-files/,  dtb -> vendor_boot-files/
#   - On dump_boot/write_boot it handles both partitions automatically.
# We must NOT manually call reset_ak+split_boot+flash_boot for vendor_boot;
# that conflicts with the auto-setup and causes dtb/Image to disappear.
BLOCK=boot;
IS_SLOT_DEVICE=auto;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

. tools/ak3-core.sh;

# After ak3-core.sh sourced, $AKHOME is set by setup_ak.
AKHOME="${AKHOME:-$PWD}";

ui_print " ";
ui_print "  ===========================================";
ui_print "  |       Ignis-Rei Kernel | munch          |";
ui_print "  |          Android 11 - 17 Ready          |";
ui_print "  ===========================================";
ui_print " ";

# If a dual-image zip is present, pick the right Image variant.
if [ -f "$AKHOME/kernels/aosp/Image" ] && [ -f "$AKHOME/kernels/stock/Image" ]; then
    CHOSEN_VARIANT="";

    # 1. Manual override flags
    if [ -f /tmp/aosp ] || [ -f /sdcard/aosp ] || [ -f /data/aosp ]; then
        CHOSEN_VARIANT="aosp";
        ui_print "  -> [Override] AOSP selected.";
    elif [ -f /tmp/stock ] || [ -f /sdcard/stock ] || [ -f /data/stock ] || \
         [ -f /tmp/miui ] || [ -f /sdcard/miui ] || [ -f /tmp/hyperos ] || [ -f /sdcard/hyperos ]; then
        CHOSEN_VARIANT="stock";
        ui_print "  -> [Override] Stock selected.";
    fi;

    # 2. Auto-detect via getprop when running in Android
    if [ -z "$CHOSEN_VARIANT" ] && [ "$BOOTMODE" = "true" ]; then
        OS_NAME=$(getprop ro.mi.os.version.name 2>/dev/null);
        MIUI_NAME=$(getprop ro.miui.ui.version.name 2>/dev/null);
        BUILD_INC=$(getprop ro.build.version.incremental 2>/dev/null);
        if [ -n "$OS_NAME" ] || [ -n "$MIUI_NAME" ] || echo "$BUILD_INC" | grep -q -E "^V[0-9]|^OS[0-9]"; then
            CHOSEN_VARIANT="stock";
            ui_print "  -> [Auto] Xiaomi HyperOS / MIUI detected.";
        else
            CHOSEN_VARIANT="aosp";
            ui_print "  -> [Auto] AOSP / Custom ROM detected.";
        fi;
    fi;

    # 3. Volume Button Selector (10-second timeout, defaults to AOSP)
    if [ -z "$CHOSEN_VARIANT" ]; then
        ui_print "  CHOOSE VARIANT (10s timeout):";
        ui_print "  [VOL+] AOSP   |   [VOL-] Stock/MIUI";
        START_TIME=$(date +%s);
        while [ -z "$CHOSEN_VARIANT" ]; do
            NOW=$(date +%s);
            if [ $((NOW - START_TIME)) -ge 10 ]; then
                ui_print "  [Timeout] Defaulting to AOSP...";
                CHOSEN_VARIANT="aosp";
                break;
            fi;
            if command -v getevent >/dev/null 2>&1; then
                KEY_EVENT=$(getevent -l -c 1 2>/dev/null);
                if echo "$KEY_EVENT" | grep -qi "KEY_VOLUMEUP.*DOWN"; then
                    CHOSEN_VARIANT="aosp";
                    ui_print "  -> AOSP selected.";
                    break;
                elif echo "$KEY_EVENT" | grep -qi "KEY_VOLUMEDOWN.*DOWN"; then
                    CHOSEN_VARIANT="stock";
                    ui_print "  -> Stock (MIUI/HyperOS) selected.";
                    break;
                fi;
            else
                CHOSEN_VARIANT="aosp";
                break;
            fi;
            sleep 0.1;
        done;
    fi;

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

# Verify Image is available (boot-files/ if ak3 auto-moved it during setup_ak)
[ -f "$AKHOME/Image" ] || [ -f "$AKHOME/boot-files/Image" ] || abort "  ! Kernel Image not found";

ui_print " ";
ui_print "  -> Flashing boot + vendor_boot + dtbo...";
# dump_boot: reads boot.img from device, splits it
# write_boot: repacks Image + ramdisk -> boot.img, flashes boot
#             then flashes dtbo (if dtbo.img present)
#             ak3 auto-setup already moved dtb -> vendor_boot-files/dtb,
#             so reset_ak + dump_boot + write_boot for vendor_boot happens automatically.
dump_boot;
write_boot;

ui_print " ";
ui_print "  ===========================================";
ui_print "  |       Ignis-Rei flashed successfully!   |";
ui_print "  ===========================================";
ui_print " ";
## end install
