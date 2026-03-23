#!/bin/sh
###
 # @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 # @Date: 2023-11-25 10:02:07
 # @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 # @LastEditTime: 2024-04-08 10:02:21
 # @FilePath: /net_camera/build.sh
 # @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
### 
clear;
arm-anykav500-linux-uclibcgnueabi-gcc net_camera.c GpioControl.c upgrade_app.c -o net_camera -std=gnu99
arm-anykav500-linux-uclibcgnueabi-gcc net_indoor.c  -o net_indoor -std=gnu99

cp net_camera ../etc/cbin/