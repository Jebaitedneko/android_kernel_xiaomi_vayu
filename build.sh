#!/bin/bash
set -x

tg_up() {
    curl "https://api.telegram.org/bot${TG_BOT_TOKEN}/sendMediaGroup" \
    -F chat_id="-$TG_CHAT_ID" \
    -F media='[{"type":"document","media":"attach://f1"},{"type":"document","media":"attach://f2"}]' \
    -F f1="@$1" -F f2="@$2" &> /dev/null
}

tg_msg() {
    curl "https://api.telegram.org/bot${TG_BOT_TOKEN}/sendMessage" \
    -F chat_id="-$TG_CHAT_ID" -F text="$1" -F parse_mode="Markdown" &> /dev/null
}

kmake() {
	MAKEOPTS="-j$(nproc) O=out ARCH=arm64 CROSS_COMPILE=$CROSS/$PRE_64- CROSS_COMPILE_ARM32=$CROSSCOMPAT/$PRE_32-"
	env PATH="$CROSS:$CROSSCOMPAT:$PATH" make $MAKEOPTS CC="$CCACHE${CROSS}/$PRE_64-gcc" "$@"
}

kzip() {
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
	" > out/ak3/anykernel.sh && sed -i "s/\t//g;1d" out/ak3/anykernel.sh
	[ -f out/arch/arm64/boot/Image ] && cp out/arch/arm64/boot/Image out/ak3
	[ -f out/arch/arm64/boot/dtb.img ] && cp out/arch/arm64/boot/dtb.img out/ak3/dtb
	[ -f out/arch/arm64/boot/dtbo.img ] && cp out/arch/arm64/boot/dtbo.img out/ak3
	mkdir -p out/ak3/vendor_ramdisk out/ak3/vendor_patch
	ZIP_PREFIX_KVER=$(grep Linux out/.config | cut -f 3 -d " ")
	ZIP_POSTFIX_DATE=$(date +%d-%h-%Y-%R:%S | sed "s/:/./g")
	ZIP_PREFIX_STR="$BLDHST-$DEVICE"
	ZIP_FMT=${ZIP_PREFIX_STR}_"${ZIP_PREFIX_KVER}"_"${ZIP_POSTFIX_DATE}"
	( cd out/ak3 && zip -r9 ../"${ZIP_FMT}".zip . -x '*.git*' )
}

BLDHST="mochi" && DEVICE="vayu"
DOCKER_64=/usr/gcc64 && DOCKER_32=/usr/gcc32
LOCAL_64=~/.local/gcc64 && LOCAL_32=~/.local/gcc32
PRE_64="aarch64-elf" && PRE_32="arm-eabi"
export KBUILD_BUILD_USER="$BLDHST"
export KBUILD_BUILD_HOST="$BLDHST"

[[ $(which ccache) ]] && CCACHE="$(which ccache) "
[ -d $DOCKER_64 ] && CROSS=$DOCKER_64/bin || CROSS=$LOCAL_64/bin
[ -d $DOCKER_32 ] && CROSSCOMPAT=$DOCKER_32/bin || CROSSCOMPAT=$LOCAL_32/bin

kmake vayu_defconfig

case "$@" in
	'') ;;
	*'llvm'*) MAKEOPTS="$MAKEOPTS LD=ld.lld AR=llvm-ar NM=llvm-nm STRIP=llvm-strip OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump READELF=llvm-readelf" ;;
	*'regen'*) cp out/.config arch/arm64/configs/vayu_defconfig && exit ;;
	*'zip'*) kzip && exit ;;
	*) kmake "$@" && exit ;;
esac

echo "CONFIG_FORTIFY_SOURCE=n" >> out/.config
START=$(date +"%s")
tg_msg "Build started"

kmake | tee out/build.log

DIFF=$(($(date +"%s") - START))
[ ! -f out/arch/arm64/boot/Image ] && tg_msg "Build failed" && exit || MSG="Build success"
MSG="$MSG. Time: \`$((DIFF / 60))\`m\`$((DIFF % 60))\`s" && tg_msg "$MSG" && kzip

[ -f out/arch/arm64/boot/Image ] && ( cd out && tg_up "${ZIP_FMT}.zip" "build.log" && tg_msg "\`$(curl -s -F f[]=@${ZIP_FMT}.zip oshi.at)\`" )
