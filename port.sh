#!/bin/bash

export ARCH=arm
export CROSS_COMPILE=$(pwd)/arm-eabi-4.8/bin/arm-eabi-
mkdir output

make -C $(pwd) O=output apq8084_sec_defconfig VARIANT_DEFCONFIG=apq8084_sec_trlte_vzw_defconfig SELINUX_DEFCONFIG=selinux_defconfig
make -j64 -C $(pwd) O=output

cp output/arch/arm/boot/zImage $(pwd)/AIK-port/split_img/boot.img-zImage
./tools/dtbTool -o ./AIK-port/split_img/boot.img-dtb -s 4096 -p ./output/scripts/dtc/ ./output/arch/arm/boot/dts/
./AIK-port/repackimg.sh
mv ./AIK-port/image-new.img ./boot-port.img
rm ./AIK-port/split_img/boot.img-dtb
rm ./AIK-port/split_img/boot.img-zImage
rm -rf output/
make clean && make mrproper