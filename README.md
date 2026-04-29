# AV100-82228-EPC 智能门口机固件

基于 Anyka AK3918 ARM 平台的可视对讲门口机固件，运行于 Linux + uclibc。
支持双向音视频对讲、AI 移动侦测、红外夜视、门禁（IC 卡 / 键盘 / 指纹）和 OTA 升级。

---

## 目录结构

```
app/
├── app/         应用层：业务状态机（对讲、门铃、OTA）
├── service/     服务层：网络、定时器、音频、AI 视觉、语音播报
├── hal/         硬件抽象层：GPIO、ADC、音频、视频、看门狗、原始套接字
└── common/      公共组件：事件总线、日志
daemon/          独立守护进程（net_camera / net_indoor）
lib/             预编译第三方库（Anyka SDK、G711、libmad）
include/         头文件（含 Anyka SDK）
upgrade/         OTA 打包脚本与升级流程
compilat_tool/   ARM 交叉编译工具链
```

---

## 编译

```bash
./make.sh
```

脚本做三件事：
1. 导出交叉编译器路径：`compilat_tool/arm-anykav500-linux-uclibcgnueabi/bin/arm-anykav500-linux-uclibcgnueabi-gcc` (GCC 4.9.4)
2. 进入 `build/` 跑 `cmake` + `make -j16`
3. 调用 `upgrade/make_image.sh` 打升级包

### 构建产物

| 产物 | 说明 |
|------|------|
| `ipc_camera` | 主应用二进制 |
| `net_indoor` | 室内机通信守护进程 |
| `AV100-82228-EPC-MMDDHHM` | OTA 升级包（squashfs + tar.gz） |

### 功能开关（`make.sh` 顶部）

| 宏 | 默认 | 说明 |
|----|------|------|
| `ENABLE_CARD` | 1 | IC 卡开锁 |
| `ENABLE_KEYPAD` | 1 | 键盘开锁 |
| `ENABLE_FINGERPRINT` | 0 | 指纹（未启用） |
| `ENABLE_ITS` / `ENABLE_ATS` | 0 | 调试服务器（生产关闭） |

---

## 架构

严格分层，只能向下调用：

```
app    →   业务逻辑、状态机
service →  跨模块平台服务
hal    →   硬件寄存器/驱动访问
common →  纯工具（事件总线等）
```

### 初始化顺序（`app/main.c`）

HAL → Service → App：

1. `DrvPlatformInit()`（Anyka SDK，失败即退出）
2. 系统 tick / 看门狗 / GPIO / ADC / 音频输入 / 视频输入
3. `SvcTimerInit` → `SvcAudioInit` → `SvcVoiceInit` → `SvcNetworkInit`（监听 6666）
4. `SvcSvpInit`（AI 移动侦测）→ `SvcIntercomInit`
5. `AppIntercomInit` → `AppUpgradeInit` → `AppDoorbellInit`
6. 主循环每 5 秒喂一次硬件看门狗

### 事件总线（`app/common/event_bus.h`）

同步发布-订阅，24 种事件类型。订阅全部在初始化阶段完成，发布在发布者线程同步调用订阅回调，运行期无锁无队列。

关键事件：`EVT_CALL_KEY_PRESSED`、`EVT_NET_CALL_START`、`EVT_NET_UNLOCK_CMD`、`EVT_INFRARED_NIGHT_MODE`、`EVT_SVP_MOTION_DETECTED`、`EVT_SYSTEM_RESET_CMD`。

### 网络协议（`app/service/svc_network.c`）

- TCP 6666：命令通道
- 原始套接字 EtherType `0xFFFF`：对讲流 + OTA
- 命令字节：`0x56`门铃事件 / `0x57`呼叫 / `0x59`流状态 / `0x54`开锁 / `0x62`升级 / `0x63`复位
- 设备 ID 运行时从 GPIO 拨码开关读取

### 核心模块

| 模块 | 文件 | 说明 |
|------|------|------|
| 对讲状态机 | `app/app/app_intercom.c` | IDLE → CALLING → TALKING / MONITORING |
| 门铃 ADC | `app/app/app_doorbell.c` | ADC 电压 → 按键事件 |
| OTA 升级 | `app/app/app_upgrade.c` | IDLE→RECEIVING→VERIFYING→FLASHING |
| 网络服务 | `app/service/svc_network.c` | 协议解析核心 |
| 定时器服务 | `app/service/svc_timer.c` | 25 路具名软定时器 |
| 音频服务 | `app/service/svc_audio.c` | 双队列（语音 / 对讲），POSIX 消息队列 |
| 语音播报 | `app/service/svc_voice.c` | PCM 优先、MP3 回退（libmad） |
| SVP AI | `app/service/svc_svp.c` | AK 硬件 CNN 人形检测 |
| GPIO HAL | `app/hal/drv_gpio.c` | 锁、灯、红外 LED、IRCUT 电机 |

### OTA 升级

- 来源：网络下发（`/tmp/av100.update`）或 SD 卡（`/mnt/sd/cbin.update`）
- 格式：tar.gz，包含 `image/image.tar.gz` + `image/image.sha1`
- 升级运行在独立线程，脚本在 `upgrade/`

### 守护进程

`daemon/net_camera.c`、`daemon/net_indoor.c` 是独立进程，与主程序 `ipc_camera` 分开运行。

---

## 测试

无单元测试框架，需烧录到真机验证。

---

## 版本与兼容

本仓库为新版重构实现。功能行为与产品形态需与旧版保持一致，但代码架构不强制对齐。


## 修改日志：

## Update Time: 2026-04-29

更新时间：2026-04-29

修改文件：
`app/service/svc_network.c`

修改位置：
`net_recv_thread()`，起始位置约在 `svc_network.c:361`

修改内容：

1. 接收缓冲区调整
- 将原 `1024 + 73` 接收缓冲区改为 `NET_RECV_BUF_LEN`
- `NET_RECV_BUF_LEN` 定义为 `1514 - 60 = 1454`
- 用于覆盖 raw 帧剥离 60 字节前缀后的最大单帧 payload

2. 超时恢复策略重构
- 移除原先依赖 `eth_reset` 的单次触发复位逻辑
- 改为“连续超时计数”机制
- 当前参数为：
  - `NET_RECV_TIMEOUT_MS = 5000`
  - `NET_RECV_RESET_TIMEOUTS = 2`

3. 增加恢复冷却时间
- 新增 `NET_RECV_RESET_COOLDOWN_MS = 30000`
- 防止链路异常时频繁执行 `eth0 down/up`

4. 恢复动作完整化
- 新增 `recv_sock_reset(int *fd_io)`
- 恢复流程包含：
  - 关闭旧接收 socket
  - 清空 `s_net.recv_fd`
  - 执行 `eth0 down/up`
  - 重新创建 raw socket
  - 重新 bind 接口
  - 更新新的 `recv_fd`

5. 增加接收错误恢复
- 除 `recv_len == 0` 的超时场景外，新增 `recv_len < 0` 错误处理分支
- 底层接收返回错误时同样尝试重建 socket

6. 接收 fd 更新统一封装
- 新增 `set_recv_fd(int fd)`
- 统一在加锁下更新 `s_net.recv_fd`

7. 使用单调时钟节流恢复
- 新增 `monotonic_ms_now()`
- 使用 `CLOCK_MONOTONIC` 计算恢复冷却时间
- 避免系统时间跳变影响恢复判断

8. 恢复失败后的退避
- 新增 `NET_RECV_RETRY_US = 500000`
- 恢复失败时线程先短暂休眠，避免异常状态下高频重试

修改后的行为：

- 正常收到数据时，清零连续超时计数
- 连续 2 次 5 秒超时后，若距离上次恢复已超过 30 秒，则执行一次网口恢复和接收 socket 重建
- 若 `NetRawPacketReceive()` 返回错误值，也会按冷却时间限制执行 socket 重建
- 线程退出前统一将 `s_net.recv_fd` 置为 `-1`

说明：

- 本次修改只涉及 `svc_network.c` 的接收线程恢复逻辑
- 未修改发送线程的 socket 恢复逻辑
- 当前在 Windows 拷贝环境下完成修改，未使用 Linux 交叉编译环境执行 `./make.sh` 验证
   `svp_process_thread` 过去是一个 150 行的大函数，现拆为：
   - `process_one_frame`：取帧→推理→决策→上报的单帧主干
   - `fuse_svp_md`：SVP × MD IoU 融合，填 obj_boxes
   - `md_box_to_pixel`：MD 网格→像素的坐标转换（含 +1 保护）
   - `emit_one_box`：单框写入 box_info 与 event
   - `dispatch_detection`：分发到 HAL 画框 + 事件总线 + 活动定时器

---

## Update Time: 2026-04-28
更新时间：2026-04-28
修改文件：
app/hal/drv_net_raw.c、app/hal/drv_net_raw.h、app/service/svc_intercom_stream.c
明确网络原始收发模块所用 60 字节区域为项目私有协议兼容前缀，并非标准以太网 MAC 头部；其中仅前 14 字节为标准以太网线头，14~59 字节为兼容性保留与补零字段。
统一修正底层裸网接口及对讲音频流业务代码内的关联注释，规范表述口径，删除易产生误导的「60 字节 MAC 头」错误描述。
修复裸网发送链路帧长越界问题，限制单帧有效载荷最大长度为 1454 字节；叠加 60 字节协议前缀后，整帧无 FCS 总长度严格控制在 1514 字节以内，解决原有 1500 字节载荷加前缀导致的缓冲区溢出风险。



## 修改时间：2026 年 4 月 14 日 19:00
修改文件：app/service/svc_net_manage.c

一、修复问题列表

P0 — 协议正确性 / 进程存活

1. TCP 流未做分帧，粘包/拆包直接错位
   原实现把一次 `recv` 的返回值当作"整包长度"交给 `check_packet`，长包 1288B 几乎必然跨 MTU 分片 → 第一次 `recv` 只收到 ~1460 以内 → 长度不匹配丢包，剩余字节被当作新包头继续解析 → 彻底错位。

2. `check_packet` 长度守卫不足
   `len <= 0` 后直接访问 `buf[2]/buf[len-1]`，`len=1/2` 为越界未定义行为。

3. 短包 sum 字段从不校验
   发送侧认真填 sum，接收侧直接跳过 → 通信抖动或对端 bug 可触发 `NET_MGR_DEL_CARD + arg1=DECK_SIZE_MAX`，静默格式化整张卡库。

4. `tcp_send` 单次 send，`n>0` 就视为成功；未屏蔽 SIGPIPE
   部分写会截断长包；对端关闭后首次 send 可能触发 SIGPIPE 杀掉 `ipc_camera` 进程。

P1 — 运行时可靠性

5. `send_long` 数据超限静默截断（`LONG_PACK_LEN - 7 = 1281` 之上直接 memcpy 截断）。
6. 会话无空闲超时、无 TCP keepalive，对端假死时内层 `recv` 循环永远 -1（超时），新 `connect` 挂在 backlog 里永不被 accept。
7. `s_client_fd` 与 `s_connected` 分两把锁 + 一个裸读，关联状态可能短暂不一致；`SvcNetManageSendShort` 无锁读 fd 存在读到已关闭 fd 的窗口。

P2 — 功能毛刺

8. `manage_light_cb` 内 `static int light_on` 跨会话残留，第二次进入添加卡模式闪烁相位不定。
9. 会话结束只停了 `TMR_ADD_CARD`，未显式停 `TMR_MANAGE_LIGHT`，依赖链隐晦。

P3 — 可读性 / 可维护性

10. `handle_packet` 末尾 `(void)arg2` 与上文 `SET_CARD_PERM` 使用 `arg2` 冲突。
11. `NET_MGR_DEL_CARD`/`NET_MGR_SET_CARD_PERM` 对越界 arg 静默丢弃，无日志。
12. `dest/src=1`、`LONG_PACK_LEN - 7` 等 magic number 散布，无集中定义。
13. `perror` 与 `LOG_E` 混用，日志走两条路径。
14. 每次 recv 前 `memset(buf,0,1288)` 纯浪费。
15. 静态变量 `s_server_fd/s_client_fd/s_connected/两把锁` 散落文件头，未结构化。
16. server socket 多余的 `O_NONBLOCK`（已配合 select 使用）。

二、代码修改详情

1. 状态全部结构化
   新增三个结构体集中管理运行期状态：
   ```c
   typedef struct {
       pthread_mutex_t lock, send_lock;   /* 状态锁 + 发送串行化锁 */
       int server_fd, client_fd, connected;
   } SvcNetMgrCtx;

   typedef struct {                        /* 接收组帧器 */
       unsigned char buf[LONG_PACK_LEN];
       int have, need;                     /* need=0 表示未锁定起始 */
   } RxFramer;

   typedef struct { int running, on; } LightBlinkCtx;
   ```
   原有 6 个散落静态变量统一归入 `s_ctx`/`s_blink`，并通过 `ctx_get_client_fd()` / `ctx_set_session()` 封装原子读写。

2. TCP 分帧器（RxFramer）
   实现 `framer_append` / `framer_try_parse` / `framer_drop_front` 三个原语，按"首字节锁定 → 攒满期望长度 → check_packet → 消费"的状态机工作。`check_packet` 失败或首字节非 0xAA/0xBB 时 resync（丢 1 字节重扫）。`session_run` 里 `tcp_read_once → framer_append → while try_parse` 循环消费，彻底解决粘包/拆包。

3. 协议字段、常量集中定义
   `PKT_OFF_START/DEST/SRC/CMD/ARG1/ARG2/SUM`、`LONG_OFF_DATA`、`LONG_PACK_DATA_MAX=1281`、`DEVICE_ID_SELF/INDOOR/BROADCAST=0x01/0x01/0xFF` 全部集中到文件顶部常量区，彻底消除 magic number。

4. 短包 sum 校验
   新增 `short_checksum(pkt)` helper；`SvcNetManageSendShort` 用它生成 sum，`check_packet` 用它校验；不匹配时 `LOG_W` 并视为无效包 resync。

5. send 路径重写
   - `tcp_send_all(fd, buf, len, total_timeout_ms)`：循环 `select(write)` + `send(MSG_DONTWAIT|MSG_NOSIGNAL)`，支持部分写续传与总超时。
   - `SvcNetManageInit` 开头 `signal(SIGPIPE, SIG_IGN)`（双保险）。
   - `net_send_bytes` 统一走 `send_lock` 串行化，失败返回 -1 向上传递，`SvcNetManageSendShort` / `send_long` 不再吞错。

6. `send_long` 数据边界守卫
   `data_len < 0` 归零；`data_len > LONG_PACK_DATA_MAX` 拒绝发送并 `LOG_E`，不再静默截断。

7. 会话 liveness：空闲超时 + TCP keepalive 双保险
   - `tcp_enable_keepalive(fd)`：`SO_KEEPALIVE` + `TCP_KEEPIDLE=10s` + `TCP_KEEPINTVL=5s` + `TCP_KEEPCNT=3`，死链 25s 可感知。
   - `session_run` 每次循环前计算 `ts_elapsed_ms(&last_rx)`，超过 `SESSION_IDLE_TIMEOUT_MS=60000ms` 主动断开。

8. 指示灯闪烁抽象
   `light_blink_start/stop` 显式管理 `LightBlinkCtx`；回调 `manage_light_cb` 检查 `s_blink.running && SvcTimerActive(TMR_ADD_CARD)` 两个条件，任一不满足即调用 stop 清零。第二次进入添加卡模式时 `on` 显式复位为 0，闪烁相位稳定。

9. `check_packet` 健壮化
   先 `len < SHORT_PACK_LEN` 守卫；同时校验 `dest`（必须为本机或广播）；失败路径全部带 `LOG_W`。

10. `handle_packet` 参数校验完善
    `NET_MGR_DEL_CARD` 的非法 arg1 区间（>DECK_SIZE_MAX）加 `LOG_W` 明示；`NET_MGR_SET_CARD_PERM` 越界 idx 加守卫；`NET_MGR_GET_CARD` 对 `AppCardDeckPermGet` 返回值做空/负值检查。

11. 其他清理
    - `perror` 全部换成 `LOG_E(...strerror(errno))`。
    - 去掉 server socket 的 `O_NONBLOCK`（已由 select 处理）。
    - 去掉每次 recv 前的 1288B `memset`。
    - 去掉 `handle_packet` 末尾与上文冲突的 `(void)arg2`。

三、修改效果

可靠性
粘包/拆包、checksum 错误、SIGPIPE、部分写、死连接全部封堵；高危命令（格式化卡库）不再可能因通信抖动误触发。

安全性
所有包都经 sum + 起止字节 + 地址三重校验才放行命令执行。

可维护性
运行期状态由 3 个结构体封装；协议字段偏移、时序参数、设备 ID 全部常量化；session 入/出/清理分离为 `session_run` / `session_cleanup`；灯控抽象为 `light_blink_start/stop`。后续协议扩展只需在常量区加字段、在 `handle_packet` 增 case。

可读性
`tcp_read_once` → `framer_append` → `framer_try_parse` → `check_packet` → `handle_packet` → `framer_drop_front` 形成清晰的数据流，每一步职责单一。

---


## 修改时间：2026 年 4 月 14 日 17:30
修改文件：app/service/svc_audio.c

一、修复问题列表

1. 语音播放偶发静音（上一次 AMP_OFF 定时器到期瞬间触发的竞态）
   现象：连续播放两段语音，如果上一次播放结束启动的 `TMR_AMP_OFF` 恰好在下一段语音开始推帧的瞬间到期，这一段语音全程静音。
   根因：`audio_output_thread` 与 `on_amp_off` 回调（跑在 svc_timer 专用线程）对功放状态与定时器的"检查—操作"不是原子的。典型坏序：
   - 输出线程读出新帧，检查 `s_amp_off_flag==0`（功放还开着），跳过 AmpEnable。
   - 输出线程 `SvcTimerActive(TMR_AMP_OFF)` 返回 true。
   - 此时定时器线程已在 svc_timer 内把 slot.active 置 0 并取出 cb，马上要调 `on_amp_off`。
   - 输出线程 `SvcTimerRefresh` 把 active 重置为 1、deadline 推后 3s。
   - 定时器线程调用 `on_amp_off`：`DrvGpioAmpDisable()` + `s_amp_off_flag=1`。
   - 输出线程 `DrvAudioOutWrite` 写入——功放已关，整段语音静音；新定时器 3s 后才再响，而本段语音期间队列一直不为空，不会进入 idle 分支重开功放。

二、代码修改详情（svc_audio.c）

1. 新增 `s_amp_mutex`，把"检查 flag → 开功放 → 刷新定时器 → 写 AO"整段与 `on_amp_off` 回调互斥。
2. `on_amp_off` 拿到锁后重检 `SvcTimerActive(TMR_AMP_OFF)`：若为 true，说明输出线程已抢先把定时器推后，放弃本次关机，交给新定时器处理。避免 in-flight 回调把刚开的功放再关掉。
3. `DrvAudioOutWrite` 放在锁内：即使 `on_amp_off` 已被定时器线程提取，也必须等写入完成并看到 active=1 后放弃关机，保证本帧一定在"功放开"状态下写入。
4. 为 `s_amp_off_flag`、`on_amp_off`、输出线程内的加锁段补充注释，说明竞态场景与互斥意图。

三、修改效果
可靠性
消除"上一次语音尾巴 + 下一次语音开头"这段时间窗口内的功放竞态，连续语音播放不再出现随机整段静音。
性能
互斥段内的操作均为毫秒级（开功放 50ms 稳定 + 一次 AO write），`on_amp_off` 每 3s 才可能被调用一次，锁争用可忽略。
可维护性
功放状态机收敛到"s_amp_mutex + s_amp_off_flag + TMR_AMP_OFF"三元组，后续若新增其它功放操作源（例如对讲流），只需遵循同一互斥约定即可。

---


## 修改时间：2026 年 4 月 14 日 16:00
修改文件：app/service/svc_intercom_stream.c

一、修复问题列表

1. 流重启后 G711 累积缓冲残留（audio_rx_handle）
   原 `static accum_buf/accum_len` 在 SvcIntercomStreamStop 后不清零，下次 Start 会把上次通话残留的几百字节拼到新数据上，导致重新呼叫的头几十毫秒出现爆音/错位。

2. 流重启后接收分帧状态残留（audio_rx_thread）
   函数局部 `rx` 结构体在线程生命周期内持续存在（线程常驻），Stop 中断时 valid=1/got<total 会被保留，下次 Start 时继续向未完整帧 memcpy，混入错乱数据。

3. net_audio_send 静态 frame_idx 不归零
   流重启后帧序号延续递增，与对端去重/排序逻辑对齐异常。

4. open_video_tx_socket 的 SO_BINDTODEVICE 未检查返回值
   eth0 未 up 时绑定失败后继续发包，可能打到 lo 或其他接口，排查困难。

5. get_ifindex 缺显式 '\0' 结尾
   memset 在前实际无 bug，但与本文件其他位置风格不一致。

6. 魔数分散、尺寸换算关系不直观
   2048/4096/1152 这些 G711↔PCM 关联尺寸缺乏集中说明。

7. net_audio_send / net_video_send 手搓大端字节序填充
   两函数各有 12+ 行字节移位，完全可复用。

8. on_video_frame 锁粒度与控制流混在一起
   单一函数里嵌套 for + if + 多次 lock/unlock + 两处错误路径的回滚，读起来要跳步核对。

二、代码修改详情

1. 新增文件级会话状态 + reset_stream_state()
   ```c
   static unsigned char s_rx_accum[INTERCOM_ALAW_ACCUM_SIZE];
   static uint32_t      s_rx_accum_len       = 0;
   static uint32_t      s_audio_tx_frame_idx = 0;
   static RxParseState  s_rx_parse           = {0};
   static void reset_stream_state(void) { ... memset ... }
   ```
   将 audio_rx_handle 的 static accum、audio_rx_thread 的 local rx、net_audio_send 的 static frame_idx 全部上提到文件级，统一管理。

2. SvcIntercomStreamStop 结尾调用 reset_stream_state()
   在 SvcAudioFlush(AUDIO_SRC_INTERCOM) 后、DrvGpioAmpDisable 前执行，确保下次 Start 是干净状态。

3. open_video_tx_socket：SO_BINDTODEVICE 失败立即 close + return -1，并打印所绑接口名便于排查。

4. get_ifindex：strncpy 后补 `ifr.ifr_name[IFNAMSIZ - 1] = '\0'`。

5. 新增 `be32_put(uint8_t *dst, uint32_t v)` inline helper
   net_audio_send / net_video_send 的头部填充由 12+ 行字节移位替换为 3 行 be32_put 调用，可读性大幅提升。

6. 视频池抽出两个 helper：video_pool_claim_slot() / video_pool_release_slot()
   on_video_frame 从 37 行含嵌套锁的逻辑简化为线性 15 行；video_tx_thread 的末尾清理也复用同一 helper，逻辑一致且不再有重复代码。

7. 文件顶部补充"尺寸说明"注释块，画出 G711(2048) ↔ PCM(4096) = AUDIO_MSG_PCM_MAX 的换算关系。

三、修改效果
可靠性
第二次及以后的呼叫不再受上一次残留污染；eth0 异常时 Start 直接失败可被上层感知。
可读性
消除两处重复的大端序写入，on_video_frame 扁平化，静态魔数集中注释。
可维护性
会话级状态收敛到 4 个文件级变量 + 1 个 reset 函数，未来新增"每通话复位"字段只需在 reset_stream_state 追加。

---


## 修改时间：2026 年 4 月 14 日 14:30
修改文件：app/service/svc_voice.c、app/service/svc_intercom_stream.c、app/hal/drv_net_raw.c、app/service/svc_network.c、app/service/svc_audio.c、app/hal/drv_infrared.c、app/hal/drv_video_in.c

一、修复问题列表

1. MP3 解码缓存越界（svc_voice.c）
   mp3_output 每帧写入 2304 字节，VOICE_CACHE_MAX = 4096，阈值冲洗放在 while 循环外；第 2 帧在循环体内就会写到 4608 字节，越界 512 字节造成段错误或栈破坏。

2. MAD 可恢复错误刷屏（svc_voice.c）
   ID3 标签扫描阶段 libmad 每次失步都触发 mp3_error，打成 WARN 污染日志。

3. 对讲流线程阵列创建失败状态不一致（svc_intercom_stream.c）
   start_threads 循环中第 i 个线程创建失败直接 return -1，已启动的线程仍在跑；SvcIntercomStreamInit 后续 malloc 分配的 send_buf 也不回收。

4. ifreq 结构未初始化（drv_net_raw.c、svc_network.c）
   struct ifreq ifr; 栈上声明后直接 strncpy(ifr.ifr_name, ..., IFNAMSIZ-1)，iface 为 IFNAMSIZ-1 字节时不补 \0；其余字段为栈残留。已有正确写法参考 svc_intercom_stream.c（先 memset）。

5. pthread_create 失败后资源未回滚（svc_audio.c、svc_network.c、drv_infrared.c、drv_video_in.c）
   与 svc_voice 同类问题：线程创建失败时已申请的消息队列 / 套接字 / GPIO / running 标志未回收。

二、代码修改详情

1. svc_voice.c：mp3_output 阈值检查前移进循环
   while (n--) {
       if (ctx->cache_len + 2 > VOICE_CACHE_MAX) mp3_flush(ctx);
       ...
   }
   每次写 2 字节前先判断剩余空间，避免越界。

2. svc_voice.c：mp3_error 使用 MAD_RECOVERABLE 分级
   可恢复错误（ID3 扫描、坏帧）降为 DEBUG；仅高字节 0x02 的致命错误打 WARN。

3. svc_intercom_stream.c：start_threads 失败时将 threads_running 置 0
   已启动的工作线程会在下一轮循环检查标志后自动退出。

4. svc_intercom_stream.c：SvcIntercomStreamInit 失败时释放 send_buf + destroy 池锁
   video_send_buf 分配失败时也释放已分配的 audio_send_buf，避免双路径泄漏。

5. drv_net_raw.c / svc_network.c：struct ifreq 先 memset 再 strncpy，末尾显式补 \0
   NetRawMacGet 额外补上 SIOCGIFHWADDR 的返回值检查。

6. svc_audio.c：pthread_create 失败时 msgctl(IPC_RMID)
   将 initialized=1 后移至线程启动成功后才设置。

7. svc_network.c：pthread_create 失败时关闭 send_fd、清 running
   与 SvcNetworkDeinit 路径等价。

8. drv_infrared.c：pthread_create 失败时 GpioSysfsClose(PIN_IR_FEED)

9. drv_video_in.c：pthread_create 失败时清 running 标志

三、修改效果
可靠性
所有 *Init 失败路径均可完整回滚，重试初始化不再死锁或泄漏；MP3 长音频不再触发越界。
可维护性
日志分级合理：可恢复的解码错误不再刷屏，真正的致命错误凸显。
健壮性
ifreq 等内核接口结构均按要求清零，避免未定义行为。


---


## 修改时间：2026 年 4 月 14 日 11:11
修改文件：app/service/svc_voice.c
一、修复问题列表
初始化线程创建失败，资源泄漏 + 状态异常
SvcVoiceInit 中 pthread_create 失败时，已设置 initialized=1、已创建消息队列，直接返回错误导致下次初始化被跳过，消息队列资源无法释放，模块状态异常。
音频帧推送重试失败静默丢帧
push_pcm 推送 PCM 数据重试 10 次失败后，无任何日志直接丢弃音频帧，线上问题无法定位排查。
帧时长使用浮点运算，嵌入式性能差
frame_ms 使用浮点公式计算，ARM 嵌入式平台浮点运算效率低、无必要，不符合开发规范。
MP3 解码错误处理逻辑错误
mp3_error 中调用 mp3_flush 语义错误，注释 “冲洗最后一段” 具有误导性；出错时无需冲洗缓存，应直接打印错误并跳过坏帧。
二、代码修改详情
1. push_pcm 函数：优化帧时长计算 + 新增丢帧日志
新增：5 行 | 删除：4 行
浮点运算改为整数运算：frame_ms = len / 32
增加帧时长边界保护：最小 1ms
重试 10 次失败后打印 WARN 日志，明确丢帧信息
static void push_pcm(const uint8_t *pcm, int len)
{
    if (len <= 0 || !pcm) return;
    /* 帧持续时间(ms) = len / (16000Hz × 2byte) × 1000 = len / 32 */
    int frame_ms = len / 32;
    if (frame_ms <= 0) frame_ms = 1;

    /* 重试送入队列（队列满时退避）*/
    int retry = 0;
    while (SvcAudioFeed(AUDIO_SRC_VOICE, pcm, (uint32_t)len) < 0) {
        if (++retry >= 10) { LOG_W("feed drop %d bytes after %d retries", len, retry); return; }
        usleep((unsigned int)(frame_ms * 1000));
    }
    /* 流控：固定半帧等待（避免解码过快淹没AO）*/
}
2. mp3_error 函数：修复错误处理逻辑
新增：3 行 | 删除：3 行
删除无意义的 mp3_flush 调用
新增 mad 解码错误日志，输出错误码
明确注释：跳过坏帧继续解码
static enum mad_flow mp3_error(void *d, struct mad_stream *s, struct mad_frame *f)
{
    (void)d; (void)f;
    LOG_W("mad decode error: 0x%04x", s->error);
    return MAD_FLOW_CONTINUE;   /* 跳过坏帧继续解码 */
}
3. SvcVoiceInit 函数：修复初始化失败资源泄漏
新增：11 行 | 删除：6 行
线程创建失败：主动销毁消息队列 + 重置 mq_id
将 initialized=1 移至线程创建成功后执行，保证初始化原子性
优化日志打印，避免锁竞争
pthread_t tid;
if (pthread_create(&tid, NULL, voice_decode_thread, NULL) != 0) {
    LOG_E("create thread fail");
    msgctl(s_voc.mq_id, IPC_RMID, NULL);
    s_voc.mq_id = -1;
    pthread_mutex_unlock(&s_voc.lock);
    return -1;
}
pthread_detach(tid);

s_voc.initialized = 1;
s_voc.busy = 0;
int mqid = s_voc.mq_id;
pthread_mutex_unlock(&s_voc.lock);
LOG_I("init ok (mqid=%d)", mqid);
return 0;
三、修改效果
可靠性提升
初始化失败可完整回滚资源，无内存 / 队列泄漏，支持重复重试初始化。
可维护性提升
丢帧、解码错误、初始化失败均有明确日志，问题定位效率大幅提升。
性能优化
浮点运算替换为整数运算，降低 ARM 平台 CPU 开销，符合嵌入式开发规范。
代码规范
修正错误语义、删除无效逻辑、优化注释，代码可读性与健壮性增强。
