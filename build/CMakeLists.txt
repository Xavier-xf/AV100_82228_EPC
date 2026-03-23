cmake_minimum_required(VERSION 3.5.4)

# 设置编译器，此处为ARM平台，如果你安装了编译链就用名称，没安装请用绝对路径
SET(PROJECT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../)
# SET(CMAKE_C_COMPILER "arm-anykav500-linux-uclibcgnueabi-gcc")
# SET(CMAKE_CXX_COMPILER "arm-anykav500-linux-uclibcgnueabi-g++")
SET(link_project ipc_camera)
project(${link_project})

add_definitions(-DIPC_MODEL="${IPC_MODEL}")
add_definitions(-D_GNU_SOURCE)#隐式声明函数‘strptime’

add_definitions(
    -DDOOR_CAMERA_MODEL=5 #门口机型号 1:253CF 2:251-4 3:225EPC 4:218KPC 5:228EPC 6:228C 7:230
    -DMAJOR_VER=0 #主版本
    -DMINOR_VER=0 #次版本
    -DPATCH_VER=2 #补丁版本
)

if(CARD_ENABLE)
add_definitions(-DCARD_ENABLE)
endif()

if(ATS_ENABLE)
add_definitions(-DATS_ENABLE)
endif()

if(ITS_ENABLE)
add_definitions(-DITS_ENABLE)
endif()

if(KEYPAD_ENABLE)
add_definitions(-DKEYPAD_ENABLE)
endif()

if(FINGERPRINT_ENABLE)
add_definitions(-DFINGERPRINT_ENABLE)
endif()

add_subdirectory(../cmake cmake)


SET(daemon1_project net_camera)   #守护进程 定义主项目名称变量
SET(daemon2_project net_indoor)
SET(DAEMON_PATH ${PROJECT_PATH}daemon)
project(${daemon1_project})       #声明项目名称
project(${daemon2_project})    
add_executable(${daemon1_project} ${DAEMON_PATH}/net_camera.c ${DAEMON_PATH}/GpioControl.c ${DAEMON_PATH}/upgrade_app.c)
add_executable(${daemon2_project} ${DAEMON_PATH}/net_indoor.c)
set_target_properties(${daemon1_project} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_PATH}/upgrade/daemon  # 设置可执行性文件运行时输出路径
)
set_target_properties(${daemon2_project} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_PATH}  # 设置可执行性文件运行时输出路径
)