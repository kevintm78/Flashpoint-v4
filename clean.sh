#!/bin/bash


rm -rf AnyKernel3/dtb
rm -rf AnyKernel3/zImage
rm -rf AnyKernel3/modules/system/lib/modules/*.ko


read -t 10 -p "Deep Clean  Ccache, 10sec timeout (y/n)?";
if [ "$REPLY" == "y" ]; then
ccache -c
rm -rf output/
rm -rf AnyKernel3/*.zip
make clean
make mrproper
fi
