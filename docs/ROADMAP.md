# 路线图

版本按"能听 → 好听 → 好用"推进。每个版本结束时软件都是可用的，不是半成品。

**两端各用各的原生 UI 技术栈**：Windows = Qt 6 Quick / QML，Android = Kotlin + Jetpack Compose + Material 3。Core 层一份共用（见 [ARCHITECTURE.md](ARCHITECTURE.md) §9）。**两套 UI 都是有头有脸的产品，不是"一个正品一个凑合"**——只是排期有先后。

**主次关系**：当必须在两端之间做取舍时，Android 的权重更高；这不是让 Windows 委曲求全，而是"功能同等，但 Android 先做完、先打磨"。之所以 v0.1/v0.2 先在 Windows 上出声，纯属工程原因——开发机是 Windows，没有能出声的东西就没法调试解码器。**先在自己脚下踩稳，再去爬那座真正要爬的山。**

## v0.1 — 最小可听（开发中）

目标：打开一个音频文件，听到声音。

- [x] Core 层接口定义（`Status`、`AudioFormat`、`Decoder`、`AudioOutput`）
- [x] Null 输出后端（按墙钟速度渲染，可在无声卡环境测试）
- [x] 冒烟测试（38 项断言）
- [ ] CMake 构建脚本跑通
- [ ] FFmpeg 解码后端（开发期先用 MSYS2 大包）
- [ ] WASAPI 输出后端（共享模式即可）
- [ ] Qt Quick 最小界面：打开文件、播放/暂停、进度条
- [ ] Windows 上能听到声音

## v0.2 — 能听整张专辑（Windows）

目标：日常听歌不别扭。

- [ ] 播放列表（添加、移除、拖拽排序）
- [ ] 上一首/下一首、随机、循环（单曲/列表）
- [ ] 元数据读取（TagLib）：标题、艺术家、专辑、时长、封面
- [ ] 拖拽文件到窗口即播放
- [ ] 记住上次的音量、播放模式、窗口布局
- [ ] 键盘快捷键（空格播放/暂停、方向键跳转）
- [ ] 拖拽进度条时预览时间，松开才真正 seek
- [ ] **FFmpeg 改为自编译的音频专用版**（见 [DEPENDENCIES.md](DEPENDENCIES.md) §6），与 v0.1 的大包构建做 A/B 比对

## v0.3 — 曲库（Windows）

目标：不用每次手动找文件。

- [ ] SQLite 媒体库：扫描目录、增量更新、删除失效条目
- [ ] 按艺术家/专辑/文件夹浏览
- [ ] 搜索（标题、艺术家、专辑）
- [ ] 封面缓存（缩略图按需生成，不每次读原图）
- [ ] 播放统计（播放次数、最近播放）

## v0.4 — Android 打通

目标：手机上能听完整一张专辑。**这一步不是"移植"，是主线，而且是这一版最重的活。**

- [ ] Android 工具链接入（JDK 21 + SDK + NDK + Gradle）
- [ ] Kotlin + Compose 工程骨架，Core 经 JNI 薄包装接入
- [ ] Material 3 主题（含动态取色）、edge-to-edge + SafeArea
- [ ] AAudio 输出后端
- [ ] 通过 SAF 访问音乐目录；`Decoder` 支持从文件描述符打开
- [ ] 后台播放：`MediaSessionService` + 前台服务
- [ ] **Media3 `SimpleBasePlayer` 接入**：把播放/暂停/seek/位置转发给 C++ 引擎，通知栏、锁屏、蓝牙耳机按键、耳机拔出暂停由 Media3 接管（见 [DEPENDENCIES.md](DEPENDENCIES.md) §3.2）
- [ ] 手机版 UI：底部导航、大按钮、单手可达
- [ ] 移动端曲库扫描：SAF 树枚举而不是直接 `stat()`
- [ ] Android 侧 FFmpeg 交叉编译（NDK）

> 为什么这一版最重：Android 的坑（SAF、前台服务、MediaSession、JNI、交叉编译）全部集中在**平台集成层**，跟播放引擎的复杂度不在一个维度上，但**发现得越晚，返工越贵**。如果等 UI 和曲库都按桌面形态做完再上手机，"手机是主线"这句话就变成了事后重构。

## v0.5 — 音质

目标：发烧友愿意用。

- [ ] 10 段图形均衡器 + 预设
- [ ] WASAPI 独占模式
- [ ] `BitPerfect` 档（见 T-B15）：协商失败必须报错，不许静默降级
- [ ] 不同采样率连播时重开流（方案 A，见 T-B16）
- [ ] ReplayGain / EBU R128 响度扫描与归一化（用现成实现，不自写）
- [ ] gapless 播放
- [ ] 交叉淡化（0–12 秒可调）
- [ ] 输出设备切换、采样率显示
- [ ] DSD：Windows 走 DoP，Android 只转 PCM

## v0.6 — 打磨（两端同步推进）

- [ ] Material 3 完整主题（Windows 端对齐 Android 端的视觉规格）
- [ ] 频谱可视化
- [ ] 歌词（LRC 内嵌/外挂）
- [ ] 睡眠定时器
- [ ] 多语言（中文、英文）
- [ ] 无障碍（屏幕阅读器、键盘导航全覆盖）

## v1.0 — 发布

- [ ] Windows 安装包（MSI 或便携版 zip）
- [ ] Android APK/AAB（F-Droid 与 GitHub Releases）
- [ ] 自动构建与发布流水线（两条）
- [ ] 用户文档

## 明确不做

- **串流与云端曲库。** 两端各自维护本地库，不做服务端。
- **在线音乐服务集成。** 不做 Spotify/网易云之类的对接。
- **视频播放。** 只做音乐。
- **社交功能。** 不做分享、评论、关注。
- **单一 UI 代码库跑两端。** 已定案用两套原生 UI（理由见 ARCHITECTURE §9），代价是维护两遍一致性，这一点是清楚的取舍而非疏忽。

## 技术债与已知取舍

| 事项 | 现状 | 何时处理 |
| --- | --- | --- |
| 无 CTest 之外的测试框架 | 手写断言 | v0.2 后视需要引入 Catch2 |
| 无持续集成 | — | v0.1 完成时加 GitHub Actions |
| 交叉淡化在解码线程做 | 简单但占用解码线程 | v0.6 若成为瓶颈再移到混音线程 |
| DSD 原生输出 | 不做：Windows DoP、Android 转 PCM | 不处理 |
| 跨采样率边界无法 gapless | 有意接受：采样率优先于 gapless | 不处理 |
| Android bit-perfect | 设备特性可选，绝大多数手机不支持 | 遥遥无期（T-B17） |
| **两套 UI 的一致性维护** | 靠一份集中设计规格约束 | 持续；每次加功能都要过两端 |
| Windows 端 FFmpeg 自编译 | 开发期先用 MSYS2 大包 | v0.2 换成音频专用构建（2–3 MB） |
| Android 端 FFmpeg 交叉编译 | 未开始 | v0.4 |
| 体积预算 | Android APK ≤ 20 MB、Windows 包 ≤ 40 MB | 每版发布前量一次（[DEPENDENCIES.md](DEPENDENCIES.md) §6） |
