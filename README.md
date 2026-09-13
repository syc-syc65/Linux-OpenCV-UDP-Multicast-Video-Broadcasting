# Linux 屏幕实时采集与 UDP 视频广播

基于 Linux + OpenCV + FFmpeg 的**屏幕实时广播发送端**：采集本机屏幕画面，JPEG 压缩后通过 UDP（组播/单播）分片发送到局域网内的一台或多台接收设备。

## 功能特性

- **屏幕采集**：通过 FFmpeg `x11grab` 抓取 X11 屏幕（兼容 XWayland/Wayland 会话），960×540 @ 15fps
- **实时压缩**：OpenCV `imencode` 实时 JPEG 编码（quality=30，兼顾画质与带宽）
- **UDP 分片协议**：超过单包大小的 JPEG 自动切成 1400 字节分片，自定义 8 字节帧头（帧序号 / 分片序号 / 分片总数）
- **组播 / 单播双模**：目标地址以 `239.` 开头自动识别为组播并设置 `IP_MULTICAST_TTL=1`，其余按单播处理
- **生产者-消费者模型**：采集线程与发送线程解耦，通过线程安全队列衔接；队列容量为 1，发送拥塞时**丢弃旧帧**保证实时性
- **本地预览**：采集端弹出实时预览窗口，按 `ESC` 退出；关闭预览窗口后自动转为纯后台发送
- **异常恢复**：帧处理异常自动跳过不中断；FFmpeg 管道打开失败时回退 OpenCV 原生 x11grab

## 系统架构

```
┌──────────────┐   Mat 帧    ┌──────────────────┐  JPEG 分片   ┌───────────┐
│ capture 线程 │ ──────────▶ │ ThreadSafeQueue  │ ───────────▶ │ send 线程 │
│ (生产者)     │             │ 容量=1, 丢旧帧    │              │ (消费者)  │
│ ffmpeg 截屏  │             └──────────────────┘              │ UDP 发送  │
└──────────────┘                                               └─────┬─────┘
                                                                     │
                                          960x540 9000/UDP ◄─────────┘
                                     组播 239.x.x.x 或单播 IP
```

## 环境依赖

| 依赖 | 版本要求 | 说明 |
|------|---------|------|
| Linux | 带 X11 图形会话 | 桌面版 Ubuntu/Debian 等 |
| g++ | 支持 C++11（线程库） | 编译用 |
| OpenCV | 4.x（含 FFmpeg 后端） | `sudo apt install libopencv-dev` |
| FFmpeg | 任意较新版本 | `sudo apt install ffmpeg`，需支持 `x11grab` |

## 编译

```bash
g++ send.cpp -o send -std=c++11 `pkg-config --cflags --libs opencv4` -lpthread
```

## 运行

```bash
# 单播到指定主机（默认端口 9000）
./send 192.168.1.100

# 组播到 239.x.x.x（同一局域网内所有加入该组的主机均可接收）
./send 239.0.0.1

# 不带参数时使用代码内默认地址 10.210.198.146
./send
```

运行后终端持续打印发送日志：

```
发送目标: 239.0.0.1:9000 [组播模式]
屏幕采集模式，生产者消费者模型启动
[capture] 屏幕采集已启动 (960x540 @15fps)
[send] frame:0 jpeg_size:48213 cost:12ms
```

## 数据包格式

每个 UDP 分片 = 8 字节帧头 + 最多 1400 字节 JPEG 数据：

```c
#pragma pack(push,1)
struct UdpSliceHeader {
    int   frame_id;    // 帧序号（网络字节序）
    short total_slice; // 本帧分片总数
    short slice_id;    // 当前分片序号（0 起）
};
#pragma pack(pop)
```

接收端按 `frame_id` 收齐同一帧的全部分片后拼接、JPEG 解码即可播放。每片之间间隔 800μs 降低突发丢包率。

## 关键参数

| 宏/变量 | 值 | 含义 |
|--------|----|------|
| `MULTICAST_PORT` | 9000 | UDP 目标端口 |
| `SLICE_DATA_MAX` | 1400 | 单片最大载荷（避免超过以太网 MTU） |
| `MAX_QUEUE_SIZE` | 1 | 帧队列容量，越小延迟越低 |
| 分辨率 / 帧率 | 960×540 @ 15fps | 在 `capture_thread()` 的 ffmpeg 命令中修改 |
| JPEG quality | 30 | 在 `send_thread()` 中修改 |

## 文件说明

```
├── send.cpp   # 发送端完整源码（采集 + 压缩 + 分片 + 发送）
└── README.md
```

> ⚠️ 本仓库目前**仅包含发送端**。接收端需自行实现（加入对应组播组 → 按帧头重组分片 → `imdecode` 解码显示），或使用 VLC/FFmpeg 配合自定义解封装调试。

## 常见问题

- **提示"截屏初始化失败"**：确认 `echo $DISPLAY` 输出正常（通常为 `:0` 或 `:1`），且已安装 ffmpeg；Wayland 纯会话下需在 XWayland 兼容环境运行。
- **组播收不到画面**：确认接收端与发送端在同一二层局域网，接收端已 `IP_ADD_MEMBERSHIP` 加入组播组，且防火墙放行 UDP 9000。
- **画面花屏/卡顿**：无线网络丢包常见，可降低分辨率、JPEG 质量或改用有线/单播。
