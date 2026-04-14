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


修改日志：

修改时间：2026 年 4 月 14 日 11:11
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