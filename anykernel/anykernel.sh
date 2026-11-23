# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers
#
# Ignis-Rei Kernel Installer for POCO F4 / Redmi K40S (munch)

properties() { '
kernel.string=Ignis-Rei Kernel | munch
do.modules=0
do.systemless=1
'; }

# Install begins here
devicename=munch
case "$devicename" in
  munch|alioth|pipa)
    is_slot_device=1;
  ;;
  apollo|lmi)
    is_slot_device=0;
  ;;
esac
case "$devicename" in
  munch)
    devicename2=munchin
  ;;
  alioth)
    devicename2=aliothin
  ;;
  apollo)
    devicename2=apollon
  ;;
esac

# Find block device path dynamically with fallback to standard bootdevice
block=/dev/block/bootdevice/by-name/boot
for testpath in /dev/block/bootdevice/by-name /dev/block/by-name /dev/block/mapper; do
  if [ -e "$testpath/boot_a" ] || [ -e "$testpath/boot" ]; then
    block="$testpath/boot"
    break
  fi
done

ramdisk_compression=auto
patch_vbmeta_flag=auto

. tools/ak3-core.sh

ui_print " "
ui_print "  ==========================================="
ui_print "  |       Ignis-Rei Kernel | munch          |"
ui_print "  |          Android 11 - 17 Ready          |"
ui_print "  ==========================================="
ui_print " "

# Rename staged Image
mv *-Image $home/Image 2>/dev/null || true

# Verify Image exists
[ -f "$home/Image" ] || abort "  ! Kernel Image not found in $home";

ui_print "--> Flashing kernel to boot partition..."
split_boot
flash_boot

# OPlus / ColorOS / OxygenOS stub nodes permissions
chmod 644 /proc/oplusVersion/* 2>/dev/null
chmod 644 /proc/oplus_charger/* 2>/dev/null
chmod 666 /proc/theiaPwkReport 2>/dev/null
chmod 666 /proc/bootprof 2>/dev/null
chmod 666 /proc/storage/buf_log 2>/dev/null
chmod 666 /proc/oplus_storage/io_metrics/forever/* 2>/dev/null
chmod 666 /proc/oplus_scheduler/sched_assist/* 2>/dev/null
chmod 666 /proc/oplus_scheduler/sched_assist/audio/* 2>/dev/null
chmod 666 /proc/oplus_frame_boost/* 2>/dev/null
chmod 666 /proc/oplus_binder/* 2>/dev/null
chmod 666 /proc/sys/fbg/* 2>/dev/null
chmod 666 /proc/oplus_afs_config/afs_config 2>/dev/null
chmod 666 /proc/oplus_mem/memory_monitor 2>/dev/null
chmod 666 /proc/devinfo/lcd* 2>/dev/null
chmod 666 /sys/class/oplus_chg/*/* 2>/dev/null

ui_print " "
ui_print "  ==========================================="
ui_print "  |       Ignis-Rei flashed successfully!   |"
ui_print "  ==========================================="
ui_print " "
