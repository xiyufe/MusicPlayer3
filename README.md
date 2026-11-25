# MusicPlayer3

一款开源音乐播放器，**以 Android 为主线**，Windows 为次要端（功能不残，只是排期在后）。

> **状态：早期开发中。** 目前只有核心层的接口与骨架，还不能播放音乐。

## 目标

- **真 gapless 播放**：预解码下一首，曲目交界处没有静音间隙（跨采样率的边界除外）。
- **可调交叉淡化**：0–12 秒，实时生效。
- **10 段图形均衡器**：采样级，非系统级音效。
- **响度归一化**：EBU R128 / ReplayGain，不再被不同专辑的音量差吓到。
- **位精确输出**：Windows 上可走 WASAPI 独占模式绕过系统混音器；Android 只能尽力而为（`BitPerfect` 档，多数设备不支持）。
- **广泛格式支持**：MP3 / FLAC / AAC / Opus / Vorbis / WAV / APE / WavPack / DSD / DTS 等，由 FFmpeg 提供。
- **短小精悍**：Android APK ≤ 20 MB，Windows 安装包 ≤ 40 MB。
- **两端各用原生 UI**：Windows = Qt 6 Quick / QML，Android = Kotlin + Jetpack Compose + Material 3。功能同等，各自贴合平台惯例。

## 架构

Core 一份共用，UI 两端各写（理由见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §9）：

```text
┌──────────────────────┐          ┌──────────────────────┐
│ Windows UI           │          │ Android UI           │
│ QML + Qt Quick       │          │ Kotlin + Compose     │
├──────────────────────┤          ├──────────────────────┤
│ Windows App          │          │ Android App          │
│ Qt 壳、平台适配       │          │ Activity + Service   │
│                      │          │ + Media3 会话        │
└──────────┬───────────┘          └──────────┬───────────┘
           └──────────────┬──────────────────┘
                ┌─────────┴──────────┐
                │ Core 纯 C++20      │  零 Qt、零平台假设
                │ 解码·混音·DSP·输出  │  Windows 与 Android
                │ ·元数据·媒体库·扫描 │  同一份源码
                └────────────────────┘
```

Core 层是唯一有实质性逻辑的地方：解码（FFmpeg）、混音与重采样、DSP、音频输出（WASAPI / AAudio）、元数据（TagLib）、媒体库（SQLite）。它不 include 任何 Qt 头文件，因此可以独立编译、独立测试，也可以被将来的命令行工具或服务端复用。

详细设计见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，版本规划见 [docs/ROADMAP.md](docs/ROADMAP.md)，依赖与第三方库的选择理由见 [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)。

### 关键设计：设备即时钟

不做"解码器时钟 + 声卡时钟"的双时钟同步。声卡的回调就是主时钟，解码器按需供帧；解码跟不上时表现为一次 underrun，而不是长时间累积的漂移。这消除了音频软件里最难调的一整类 bug。

## 目录结构

```text
core/                 纯 C++20 核心库
  include/mp3/        公开头文件
  src/                实现
  tests/              测试
apps/
  windows/            Qt 6 Quick 前端（尚未创建）
  android/            Kotlin + Compose 前端（尚未创建）
docs/                 设计文档
third_party/          第三方源码（尚未创建）
```

## 构建

尚不可用。依赖版本与体积预算见 [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)，构建脚本会在依赖就位后补齐并测试。

## 协议

GPL-3.0-or-later，见 [LICENSE](LICENSE)。

选择 GPL-3.0 而非 MIT/Apache 的原因：播放器会链接 FFmpeg（可选启用 GPL 组件）与 Qt（LGPL 下动态链接即可，但 GPL 更省心），GPL-3.0 与两者都兼容，且能保证任何改进都回流到社区。第三方依赖的许可证矩阵见 [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) §5。
