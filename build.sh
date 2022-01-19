#!/bin/bash
set -ex
BLDHST="mochi" && DEVICE="vayu"
PRE_64="aarch64-elf" && PRE_32="arm-eabi"
[[ $(which ccache) ]] && CCACHE="$(which ccache) "
[ -d /usr/gcc64 ] && CROSS=/usr/gcc64/bin/$PRE_64- || CROSS=~/.local/gcc64/bin/$PRE_64-
[ -d /usr/gcc32 ] && CROSSCOMPAT=/usr/gcc32/bin/$PRE_32- || CROSSCOMPAT=~/.local/gcc32/bin/$PRE_32-
export KBUILD_BUILD_USER="$BLDHST"
export KBUILD_BUILD_HOST="$BLDHST"
CROSS_TRIM=$(echo $CROSSCOMPAT | sed "s/$(basename $CROSS)//g;s/\/$//g")
CROSSCOMPAT_TRIM=$(echo $CROSS | sed "s/$(basename $CROSS)//g;s/\/$//g")
MAKEOPTS="-j$(nproc) O=out ARCH=arm64 CROSS_COMPILE=$CROSS CROSS_COMPILE_ARM32=$CROSSCOMPAT"
env PATH="$CROSS_TRIM:$CROSSCOMPAT_TRIM:$PATH" make $MAKEOPTS CC="$CCACHE${CROSS}gcc" vayu_defconfig
echo "CONFIG_FORTIFY_SOURCE=n" >> out/.config
env PATH="$CROSS_TRIM:$CROSSCOMPAT_TRIM:$PATH" make $MAKEOPTS CC="$CCACHE${CROSS}gcc"
[ ! -d out/ak3 ] && git clone --depth=1 https://github.com/osm0sis/AnyKernel3 out/ak3
echo -e "
# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers
properties() { '
kernel.string=$BLDHST
device.name1=$DEVICE
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
'; }
block=/dev/block/bootdevice/by-name/boot;
is_slot_device=0;
ramdisk_compression=auto;
patch_vbmeta_flag=auto;
. tools/ak3-core.sh;
set_perm_recursive 0 0 755 644 \$ramdisk/*;
set_perm_recursive 0 0 750 750 \$ramdisk/init* \$ramdisk/sbin;
dump_boot;
if [ -d \$ramdisk/overlay ]; then
	rm -rf \$ramdisk/overlay;
fi;
write_boot;
" > out/ak3/anykernel.sh
cp out/arch/arm64/boot/{Image,dtb.img,dtbo.img} out/ak3
ZIP_PREFIX_KVER=$(grep Linux out/.config | cut -f 3 -d " ")
ZIP_POSTFIX_DATE=$(date +%d-%h-%Y-%R:%S | sed "s/:/./g")
ZIP_PREFIX_STR="$BLDHST-$DEVICE"
ZIP_FMT=${ZIP_PREFIX_STR}_"${ZIP_PREFIX_KVER}"_"${ZIP_POSTFIX_DATE}"
cd out/ak3 && zip -r9 "${ZIP_FMT}".zip . -x '*.git*' '*modules*' '*patch*' '*ramdisk*' 'LICENSE' 'README.md'
curl -i -F f[]=@"${ZIP_FMT}.zip" https://oshi.at -s && cd ../..
