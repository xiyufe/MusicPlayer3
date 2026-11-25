# 依赖与「不重复造轮子」

这份文档定两件事：**哪些活交给别人，哪些活必须自己干，以及判断的依据是什么。**

它存在的实际原因：每次选一个库都要重新吵一遍"用现成的还是自己写"，吵完还没留下记录，下次接着吵。把判断标准写死，后面每个选型都有依据可引。

## 1. 原则：分清"别人的责任"和"我们的责任"

正确的说法不是"能用现成的就用现成的"。那是懒，会把项目变成一堆库的胶水，最后出问题谁都不好查。

正确的说法是：

> **凡是"已经有人把它做成公共基础设施、且做对了"的部分，一律用现成的；凡是"决定这个产品为什么存在"的部分，自己写。**

对 MusicPlayer3 来说，分界线很清楚：

| 属于别人 | 属于我们 |
| --- | --- |
| 把 MP3/FLAC/DSD 变成 PCM | 让两首歌之间没有间隙、按你的耳朵调 EQ |
| 把 PCM 送进声卡、协商采样率 | 决定"采样率优先于 gapless"这个取舍 |
| 字符编码检测、Unicode 归一化 | 用什么规则判断"这两条记录是同一首歌" |
| 数据库引擎、B 树、事务 | 表怎么设计、同步怎么合并 |
| 媒体通知、锁屏、蓝牙按键 | 播放队列的顺序语义 |
| 客户端渲染、动画曲线 | 界面长什么样、信息怎么组织 |

右栏空着的话，这个项目就没有存在的理由。左栏自己写的话，就是在浪费生命。

## 2. 判定标准：引入一个库之前问四个问题

1. **它解决的是不是我的核心问题？** 是 → 自己写。不是 → 用现成的。
2. **它是不是被大量项目在同类场景下用过？** 一个人维护、star 三千、两年没提交 → 不用，即使它能用。
3. **许可证是否与 GPL-3.0 相容？** 见 §5。不相容 → 直接排除，不必再评估。
4. **它退出时我有多痛？** 会在接口里扩散到十处的（比如渲染框架）要谨慎；只在一个函数里用一次的（比如编码探测）随便换。

第 4 条决定我们**用什么方式隔离依赖**：所有第三方库都只能出现在 `core/src/` 的某几个 `.cpp` 里，头文件不外泄。`core/include/mp3/` 下面一行 `#include <libavcodec/...>` 都不许有。这样换库的时候，改动范围是可预期的。

## 3. 依赖清单

### 3.1 Core 层

| 依赖 | 许可证 | 用途 | 为什么是它 | 考虑过但没选 |
| --- | --- | --- | --- | --- |
| **FFmpeg**（libavformat/libavcodec/libavutil/libswresample） | LGPL-2.1+（`--disable-gpl`） | 解码全部格式 | 唯一能覆盖从 MP3 到 DSD、APE、WavPack、DTS 的完整解码栈；libmpv 能做的它都能做，反过来不成立 | libmpv（GPL 主体、无交叉淡化、是"整机"不是零件）；dr_libs（格式太少，无 APE/DSD）；miniaudio 自带解码器（只够 WAV/FLAC/MP3 子集） |
| **miniaudio** | 公有领域 或 MIT-0（二选一） | 音频设备封装 | 单头文件、零构建成本、后端覆盖 WASAPI（含独占）与 AAudio/OpenSL | PortAudio（老牌但构建麻烦、Android 支持弱）；自己写 WASAPI + AAudio（两套平台代码几百行，收益为零） |
| **TagLib** | LGPL-2.1 或 MPL-2.0（双许可） | 读写元数据与封面 | 标签格式支持最全（ID3v1/v2、APE、Vorbis Comment、MP4、ASF），**且能写**——我们要做编辑标签 | 只用 FFmpeg 的 metadata（读可以，写不行，且它把标签当通用字典不是领域模型） |
| **SQLite** | 公有领域 | 曲库 | 唯一合理选择：单文件、零运维、事务可靠、跨平台一致 | 自己写存储（自杀）；LMDB（无 SQL，统计查询要手写） |
| **utf8proc** | MIT | NFC/NFKC 归一化、大小写折叠 | `name_norm`（DATA_MODEL §3.1）需要 NFKC + 全量 Unicode 大小写折叠。**Unicode 大小写映射表有两千多个条目且随 Unicode 版本变**——自己维护是纯负债 | 自写映射表（版本跟进是大坑）；ICU（正确但约 30 MB，见 §4） |
| **uchardet** | MPL-1.1 / GPL-2.0+ / LGPL-2.1+（三选一，取 LGPL） | `.lrc` 编码探测 | `.lrc` 没有统一编码（DATA_MODEL §2.12），必须猜。它是 Mozilla 通用编码探测器的 C 移植，是这件事的既定答案 | 自己写 GB18030/Shift-JIS 判别（准确率会差很多）；ICU（体积）；iconv（只能转换，不能探测） |
| **nlohmann/json** | MIT | 智能播放列表 `smart_query`、设置、同步载荷 | 头部单文件、API 自然、错误信息可读。我们解析的是几百字节的小 JSON，性能不是问题 | rapidjson（快但 API 啰嗦）；simdjson（为 GB/s 设计，我们不需要）；自写 JSON 解析器（解析器是最不该自己写的东西之一） |
| **小体积 SHA-256** | 公有领域 / MIT | 曲目指纹（DATA_MODEL §4） | 只需要一个函数，不需要整条密码学栈 | OpenSSL（为一个哈希拖进 3 MB 与一堆历史包袱）；libsodium（同样太重） |

> **FFmpeg 为什么不选"现成封装"：** Android 上一度最流行的 FFmpeg-kit 已于 2025-01-06 正式退役、二进制于 2025 年 4 月从公共仓库下架。这恰好证明**解码层不能依赖第三方封装**——封装会死，FFmpeg 本身不会。所以直接用自己的构建，只加一层薄薄的 C++ 包装。

> **`--disable-gpl` 是必须的：** 我们只解码不编码，不需要 x264/x265 这类 GPL 组件。保持 LGPL 让许可证关系保持简单，同时也减小体积（见 §6）。因为整个应用本身是 GPL-3.0，静态链接 LGPL 库的"提供可重新链接的目标文件"义务可以被"提供完整源码"直接满足，不存在合规障碍。

### 3.2 平台层

| 依赖 | 许可证 | 用途 | 说明 |
| --- | --- | --- | --- |
| **WASAPI** | 系统 API | Windows 音频输出 | 不是第三方库，是操作系统接口。`OutputMode` 三档全靠它实现 |
| **AAudio** | 系统 API | Android 音频输出（8.0+） | 同上。OpenSL ES 作为兜底 |
| **Qt 6 Quick**（qtbase/qtdeclarative） | LGPL-3.0 / GPL-3.0 / 商业 | Windows 端 UI | 应用是 GPL-3.0，走 GPL 路径最省事。**动态链接**，不静态链接（原因见 §6） |
| **Media3**（`androidx.media3`） | Apache-2.0 | Android 媒体会话 | **这是"不重复造轮子"最值钱的一处**。用 `SimpleBasePlayer` 实现播放/暂停/seek/位置转发给 C++ 引擎，通知栏、锁屏、蓝牙按键、耳机拔出暂停、Android Auto 全套由它接管。自己用 `MediaSessionCompat` 写要几百行且要处理一堆系统版本分支 |
| **Jetpack Compose** + **Material 3** | Apache-2.0 | Android 端 UI | `androidx.compose.material3` 是 Material 3 的**参考实现**，不是模仿 |

### 3.3 明确不用（记下来省得以后再讨论）

| 不用 | 原因 |
| --- | --- |
| **Qt Multimedia** | 做不了精确 gapless、交叉淡化、采样级 EQ、ReplayGain。用它等于放弃产品定位 |
| **libmpv / mpv 库** | 全栈播放，我们要的是可控的零件；没有交叉淡化；GPL 主体 |
| **ICU** | Unicode 的正确但沉重答案（约 30 MB）。我们只需要归一化 + 编码探测两件事，utf8proc + uchardet 加起来不到 1 MB |
| **Qt for Android** | Android 走原生 Kotlin + Compose，详见 ARCHITECTURE §9 |
| **Electron / Flutter / .NET MAUI** | Core 是 C++，跨语言边界要穿 FFI，高频位置与频谱更新脆弱；启动体积也不符合"短小精悍" |
| **Boost** | 我们需要的部分（文件系统、格式化）C++20 标准库已有 |

## 4. 必须自己写的部分，以及为什么没有现成轮子

| 自己写 | 为什么不能买/借 |
| --- | --- |
| **播放引擎**（环形缓冲、设备即时钟、gapless、交叉淡化、采样率切换） | 没有库能同时满足：精确 gapless、可调交叉淡化、采样率优先、位精确可选。这是产品的核心差异 |
| **DSP 链**（10 段图形 EQ、淡入淡出、响度归一化） | EQ 与淡化的算法本身没有秘密，难的是**嵌进实时线程**且不爆音——这跟具体引擎绑定，买不到 |
| **数据模型与同步合并** | 表结构、墓碑、`order_key`、`work_key`、冲突语义全部是产品决策，不是技术选型 |
| **智能播放列表的白名单编译器** | 核心是"安全"，安全性来自编译方式本身（DATA_MODEL §6.1）。买一个查询 DSL 就等于买一个注入面 |
| **扫描器** | 要跟 `track_files` 增量判据、racy 保护、指纹剥标签、SAF 能力降级全部咬合。原型的全部价值就在这些细节上 |

**响度归一化是个例外**：EBU R128 的算法有标准（ITU-R BS.1770），社区已有成熟实现（如 libebur128，MIT）。**这个应该用现成的**——它不是我们的差异点，自己实现反而容易算错。等 v0.5 做音质时再引入。

## 5. 许可证相容性

应用是 **GPL-3.0**（见仓库根 `LICENSE`）。结论：

| 依赖许可证 | 与 GPL-3.0 组合 | 备注 |
| --- | --- | --- |
| 公有领域（SQLite、miniaudio 可选项、SHA-256 实现） | ✅ 无任何义务 | 最省心 |
| MIT / MIT-0 / BSD / zlib（utf8proc、nlohmann、miniaudio） | ✅ | 保留版权声明即可 |
| Apache-2.0（Media3、Compose、AndroidX） | ✅ 与 GPLv3 相容 | 注意它**与 GPLv2 不相容**，所以不能把项目的 GPL 版本降回 v2 |
| LGPL-2.1+（FFmpeg、TagLib） | ✅ | 静态链接时用"提供完整源码"满足重链接义务；动态链接更简单 |
| LGPL-3.0（Qt 6） | ✅ | 应用是 GPL-3.0，直接走 GPL 路径 |
| MPL-2.0（TagLib 可选） | ✅ 有二级许可条款 | 取 LGPL 分支更省事 |
| **MPL-1.1**（uchardet 的可选项之一） | ❌ **不相容** | uchardet 是三许可，**必须显式取 LGPL-2.1+ 分支，不能取 MPL-1.1** |
| GPL-2.0-only | ❌ 不相容 | 遇到这类库直接排除 |

**给未来的一条规则：引入任何依赖前先确认 `LICENSE` 文件里写的具体版本，不看 README 的徽章。** uchardet 这种三许可的库，选错分支就是合规问题。

## 6. 体积预算

"短小精悍"是明确要求。下面是由此推出来的硬约束：

| 目标 | 预算 | 现状 |
| --- | --- | --- |
| Android APK（发布签名） | ≤ 20 MB | Kotlin + Compose 原生约 12–18 MB（Compose 运行时基础开销 1–2 MB） |
| Android 装完占用 | ≤ 40 MB | — |
| Windows 安装包 | ≤ 40 MB | Qt 动态链接 + 精简 FFmpeg 约 30 MB |
| Windows 装完占用 | ≤ 90 MB | — |

**FFmpeg 必须自编译成音频专用版。** 这是整份文档里对体积影响最大的一条：

| 构建方式 | 体积 |
| --- | --- |
| MSYS2 发行版完整 FFmpeg | 开发包约 120 MB，DLL 总计约 134 MB（含全部编码器/视频/滤镜/网络） |
| 完整构建（社区实测） | 134.4 MB |
| 基础构建 | 20.5 MB |
| **`--disable-everything --enable-small` + 只开音频解码** | **2–3 MB** |

我们的配置大致是：

```text
--disable-everything --disable-gpl --disable-network --disable-autodetect
--disable-encoders --disable-video --disable-devices --disable-doc
--disable-ffplay --disable-ffprobe --enable-small
--enable-decoder=<音频解码器白名单>
--enable-demuxer=<容器白名单>
--enable-protocol=file
--enable-swresample
```

开哪些解码器由「支持的扩展名」反推（`core/src/decoder.cpp` 的 21 项）。**每加一个格式要重新算一次体积账**，这是有意的摩擦。

**Windows 端 Qt 用动态链接而不是静态链接：** 静态链接 Qt 需要自己构建整套 Qt（数小时），且会让 GPL 合规更绕（静态链入 GPL 的 Qt）。动态链接的代价是发布时是一棵 DLL 树而不是单个 exe，用安装包或 `windeployqt` + 压缩解决。

## 7. 引入新依赖的流程

以后想加一个库，按这个顺序走，缺一步就别加：

1. **说清它解决什么问题**，以及不解决会怎样。
2. **填一张表**：许可证、最后提交时间、同类场景的使用者、退出成本。
3. **看它会不会出现在 `core/include/mp3/` 里**。会 → 设计一个自己的抽象接口，把库关在 `.cpp` 里。
4. **算体积**，对 §6 的预算做加减。
5. **在本文档 §3 加一行**，写明为什么不选别的。**没写进这张表的依赖，不算被批准。**

## 8. 已知风险

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| FFmpeg API 在 major 版本间变动 | 升级会编译失败，甚至行为变化 | 锁定 major 版本；升级前跑解码回归测试（各格式样本各一份） |
| uchardet 准确率有限（短文本尤其） | 极少数 `.lrc` 猜错编码、显示乱码 | 探测结果作为默认值，设置里允许用户手动指定编码覆盖 |
| Compose 与 AndroidX 版本强耦合 | Gradle 依赖对齐出问题 | 用 Gradle version catalog 集中管理，不散落各模块 |
| Qt 6 小版本间的 QML 行为差异 | Windows UI 升级后样式漂移 | 锁定 Qt 次版本；升级时人工过一遍界面 |
| 第三方库成为供应链风险（投毒、撤包） | 构建突然不可复现 | 关键依赖锁到 commit/tag，vendor 进 `third_party/` 的保留源码 |

## 9. 一张总表

| 名称 | 层 | 许可证 | 引入方式 | 是否已在仓库 |
| --- | --- | --- | --- | --- |
| FFmpeg | Core | LGPL-2.1+ | pkg-config（Windows）、NDK 交叉编译（Android，v0.4） | 未引入 |
| miniaudio | Core | PD / MIT-0 | vendor 单头文件 | 未引入 |
| TagLib | Core | LGPL-2.1 | pkg-config | 未引入 |
| SQLite | Core | PD | pkg-config / 系统 | 未引入（`MP3_HAVE_SQLITE3` 已预留） |
| utf8proc | Core | MIT | vendor 源码或 pkg-config | 未引入 |
| uchardet | Core | LGPL-2.1+（**不取 MPL-1.1**） | vendor 源码 | 未引入 |
| nlohmann/json | Core | MIT | vendor 单头文件 | 未引入 |
| SHA-256 | Core | PD | vendor 单文件 | 未引入 |
| Qt 6 (qtbase, qtdeclarative) | App/UI（Windows） | GPL-3.0 | 动态链接 | 未引入 |
| Media3 | App（Android） | Apache-2.0 | Gradle | 未引入 |
| Compose + Material3 | UI（Android） | Apache-2.0 | Gradle | 未引入 |
