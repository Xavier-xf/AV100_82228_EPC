#!/bin/sh
###
 # @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 # @Date: 2024-04-10 09:53:42
 # @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 # @LastEditTime: 2024-04-10 10:53:26
 # @FilePath: /2CD-ME/upgrade/app/daemon.sh
 # @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
### 
# 守护进程
# while test "1" = "1"
# do
#     sleep 1
#     value=$(ps aux | grep TABA.BIN | grep -v grep | wc -l)
#     if [ $value -ne 1 ]
#     then
#         killall TABA.BIN
#         /app/app/TABA.BIN leo &
#     fi
# done
#!/bin/bash

IPC_CAMERA_PATH=$1"ipc_camera"

while true; do
    # 检查文件是否存在
    if [ -e "$IPC_CAMERA_PATH" ]; then
        # 检查 IPC_CAMERA 是否在运行
        if pgrep ipc_camera > /dev/null; then
            sleep 1
        else
            echo "IPC_CAMERA is not running, restarting..."
            "$IPC_CAMERA_PATH" &
            sleep 5  # 等待 5 秒再次检查
        fi
    else
        echo "IPC_CAMERA path not found: $IPC_CAMERA_PATH"
        exit 1
    fi
done

