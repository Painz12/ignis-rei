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

# Rename staged files (MUNCH-Image -> Image, MUNCH-dtb -> dtb, MUNCH-dtbo.img -> dtbo.img)
mv *-Image $home/Image 2>/dev/null || true
mv *-dtb $home/dtb 2>/dev/null || true
mv *-dtbo.img $home/dtbo.img 2>/dev/null || true

# Verify Image exists
[ -f "$home/Image" ] || abort "  ! Kernel Image not found in $home";

ui_print "--> Flashing kernel to boot partition and dtbo overlay..."
dump_boot
write_boot

if [[ $is_slot_device == 1 ]]; then
  ui_print "--> Flashing hardware DTB to vendor_boot partition..."
  block=/dev/block/bootdevice/by-name/vendor_boot
  for testpath in /dev/block/bootdevice/by-name /dev/block/by-name /dev/block/mapper; do
    if [ -e "$testpath/vendor_boot_a" ] || [ -e "$testpath/vendor_boot" ]; then
      block="$testpath/vendor_boot"
      break
    fi
  done
  ramdisk_compression=auto
  patch_vbmeta_flag=auto
  reset_ak
  dump_boot
  write_boot
else
  ui_print "--> Installing to boot partition... "
fi

ui_print " "
ui_print "  ==========================================="
ui_print "  |       Ignis-Rei flashed successfully!   |"
ui_print "  ==========================================="
ui_print " "
