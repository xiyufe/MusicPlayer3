# MusicPlayer3

一款开源音乐播放器，面向 Windows 与 Android。

> **状态：早期开发中。** 目前只有核心层的接口与骨架，还不能播放音乐。

## 目标

- **真 gapless 播放**：预解码下一首，曲目交界处没有静音间隙。
- **可调交叉淡化**：0–12 秒，实时生效。
- **10 段图形均衡器**：采样级，非系统级音效。
- **响度归一化**：EBU R128 / ReplayGain，不再被不同专辑的音量差吓到。
- **位精确输出**：Windows 上可走 WASAPI 独占模式，绕过系统混音器。
- **广泛格式支持**：MP3 / FLAC / AAC / Opus / Vorbis / WAV / APE / WavPack / DSD / DTS 等，由 FFmpeg 提供。
- **一份 UI 跑两端**：Windows 与 Android 共享同一套 Qt Quick 界面。

## 架构

三层，边界干净：

```text
┌─────────────────────────────────────────┐
│  UI 层    QML + Qt Quick Controls       │  Windows: D3D11  Android: GLES/Vulkan
├─────────────────────────────────────────┤
│  App 层   Qt 壳                          │  平台适配、设置持久化、生命周期
├─────────────────────────────────────────┤
│  Core 层  纯 C++20，零 Qt 依赖           │  Windows 与 Android 同一份源码
└─────────────────────────────────────────┘
```

Core 层是唯一有实质性逻辑的地方：解码（FFmpeg）、混音与重采样、DSP、音频输出（WASAPI / AAudio）、元数据（TagLib）、媒体库（SQLite）。它不 include 任何 Qt 头文件，因此可以独立编译、独立测试，也可以被将来的命令行工具或服务端复用。

详细设计见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，版本规划见 [docs/ROADMAP.md](docs/ROADMAP.md)。

### 关键设计：设备即时钟

不做"解码器时钟 + 声卡时钟"的双时钟同步。声卡的回调就是主时钟，解码器按需供帧；解码跟不上时表现为一次 underrun，而不是长时间累积的漂移。这消除了音频软件里最难调的一整类 bug。

## 目录结构

```text
core/                 纯 C++20 核心库
  include/mp3/        公开头文件
  src/                实现
  tests/              测试
apps/                 Qt 前端（尚未创建）
docs/                 设计文档
third_party/          第三方源码（尚未创建）
```

## 构建

尚不可用。依赖（CMake、Ninja、Qt 6、FFmpeg 开发库、TagLib、SQLite）尚未安装，构建脚本会在依赖就位后补齐并测试。

## 协议

GPL-3.0-or-later，见 [LICENSE](LICENSE)。

选择 GPL-3.0 而非 MIT/Apache 的原因：播放器会链接 FFmpeg（可选启用 GPL 组件）与 Qt（LGPL 下动态链接即可，但 GPL 更省心），GPL-3.0 与两者都兼容，且能保证任何改进都回流到社区。
