###
 # @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 # @Date: 2024-04-09 08:59:00
 # @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 # @LastEditTime: 2024-08-06 16:41:54
 # @FilePath: /2CD-ME/make.sh
 # @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
### 

CC="$(pwd)/compilat_tool/arm-anykav500-linux-uclibcgnueabi/bin/arm-anykav500-linux-uclibcgnueabi-gcc"
CXX="$(pwd)/compilat_tool/arm-anykav500-linux-uclibcgnueabi/bin/arm-anykav500-linux-uclibcgnueabi-g++"

OPTION="AV100-82228-EPC"
ENABLE_ITS=0
ENABLE_ATS=0
ENABLE_CARD=1
ENABLE_KEYPAD=1
ENABLE_FINGERPRINT=0

#配置音频参数文件
add_audio_param_file()
{
    filename="src/AudioParam.c"

    # 使用sed检查文件中是否有指定的宏定义，并替换为头文件包含
    sed -i 's/#define _AK_AUDIO_CONFIG_H_/#include "ak_common_audio.h"/' $filename
}

#编译
compile_cmake()
{
    cd build

    rm -rf CMakeFiles
    rm -f cmake_install.cmake
    rm -f CMakeCache.txt
    cmake \
    -DCMAKE_C_COMPILER=${CC} \
    -DCMAKE_CXX_COMPILER=${CXX} \
    -DIPC_MODEL="$OPTION" \
    -DITS_ENABLE=${ENABLE_ITS} \
    -DATS_ENABLE=${ENABLE_ATS} \
    -DCARD_ENABLE=${ENABLE_CARD}  \
    -DKEYPAD_ENABLE=${ENABLE_KEYPAD} \
    -DFINGERPRINT_ENABLE=${ENABLE_FINGERPRINT} \
    .
    
    make clean
    make -j16
    cd -
}

#制作升级文件
make_upgrade_package()
{
    cd upgrade/
    ./make_image.sh $OPTION
    cd -
}


clear

# add_audio_param_file

compile_cmake

make_upgrade_package
