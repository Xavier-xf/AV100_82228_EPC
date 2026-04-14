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
