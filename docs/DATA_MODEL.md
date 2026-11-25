# 数据模型

曲库、播放列表、设置的持久化结构，以及为「Windows ↔ Android 同步」预留的设计。

---

## 0. 先说结论：同步需要哪些字段

你问 `revision` / `updated_at` 是否必要。答案是**必要，但光有它们还不够**，而且每个字段干的活不一样：

| 字段 | 作用 | 少它会怎样 |
| --- | --- | --- |
| `updated_at` | 判断"谁更新"，合并时的新旧依据 | 无法做 last-write-wins，只能整份覆盖 |
| `updated_by` | 设备 ID，时间戳打平时的裁决者 | 两台设备同一毫秒修改时结果不确定 |
| `revision` | 单行被修改的次数，冲突检测与调试 | 能合并但说不清"这条被改过几次" |
| `deleted_at` | **墓碑**：记录"某条被删了" | **删除无法传播**，同步会把删掉的东西复活 |

最后一条是新手最常踩的坑，值得单独说：**删除一行后，那行就不存在了，而同步靠"对比两边的行"工作，看不见的东西没法同步。** 于是 A 设备删了播放列表，同步时 B 设备发现"我这有、你那没有"，会当成"B 更新"把它推回去。解决办法是**软删除**：不真删，只打一个 `deleted_at` 时间戳，所有查询都过滤掉它，同步时把它当作"这行存在且已被删除"来传播。真正的物理删除要等到确认没有设备还会同步这条为止（这里定 90 天）。

另外，**导出导入备份其实不需要上面任何一个字段**——整份快照直接覆盖就行。这些字段全部是为 WebDAV 双向合并服务的。你现在两个都要，所以留着是对的。

最后一个字段是我要主动加的，你没问但绕不开（后来变成了两个）：

| 字段 | 作用 |
| --- | --- |
| `fingerprint` | 曲目的**跨设备稳定标识**，见 §4 |
| `work_key` | 统计聚合键，见 §4.6 |

没有 `fingerprint`，Windows 上的播放列表到 Android 上全是"找不到这首歌"。没有 `work_key`，两端各听 2 次的一首歌，统计页会显示成两个"2 次"而不是一个"4 次"。

### 0.1 同步的范围：曲库不同步

这一条定下来之前走过弯路，值得写清楚：**曲库（`artists` / `albums` / `tracks` / `track_files`）不参与同步。**

| 同步 | 不同步 |
| --- | --- |
| 播放列表、列表条目 | 艺术家、专辑、曲目、文件 |
| 设置（`shared` 作用域） | 设置（`device` 作用域）、播放队列 |
| 播放历史 | 搜索用的归一化列 |
| 星级评分 | 同步水位与同步日志 |
| 听歌汇总 | |

最早的理由是"省流量"，这个理由是错的，实测数据摆在这里：

| 数据 | 大小 |
| --- | --- |
| 曲库 10000 首全量推一次（JSON） | 2.36 MiB |
| 同上，gzip 后 | **0.79 MiB** |
| 播放历史一年的量（16000 条），gzip 后 | **1.12 MiB** |

一年播放历史比整个曲库还大。真在乎流量，该省的是播放历史。

**真正的理由是同不了，而且同步过去会让用户困惑：**

1. **文件物理位置无法同步。** PC 的 `D:/Music/x.mp3` 在手机上不存在。同步来的 8000 条曲目在手机上全是"有记录、无文件"，曲库页面变成 8000 条灰条。
2. **两端扫描结果互相覆盖。** 手机扫出 3000 首、PC 扫出 8000 首，合并后手机端要显示 5000 首"缺失"，每次同步都在看这个数字。
3. **"用户主动删除"的语义崩坏。** PC 上删掉一首歌，同步过去手机也标删除，而手机本来就没这个文件；反过来手机的文件没扫到，同步到 PC 会让 PC 以为歌丢了，而 PC 上明明有。

**曲库不同步，两端仍然能对齐。** 靠的是两个**纯算法算出来的值**，不是存下来的数据：

| 值 | 怎么来 | 用途 |
| --- | --- | --- |
| `fingerprint` | 两端对同一段音频算出同一个值 | 文件身份：播放列表定位、评分对上号、播放历史回指曲目 |
| `work_key` | 两端对同一组标签算出同一个值 | 统计聚合：播放次数、总时长 |

这两个都是**函数**，不依赖同步。曲库表因此退回成一台机器上纯私有的缓存，怎么删怎么重建都不会影响另一端。

**曲库表因此可以简化**：没有 `updated_at` / `updated_by` / `revision` / `deleted_at`，用户删歌就是物理 `DELETE`。少掉的四列和四个索引，换来的是曲库导入导出、重建、修复逻辑全部变成单机问题。

---

## 1. 三条通用规则

**规则一：同步的单位是「实体」，不是「行」。**
`playlists`、`playlist_items`、`settings`、`play_history`、`track_ratings`、`play_stats`（后两张见 §2.10、§2.14）是实体，各有自己的 `uid` 或稳定键和完整同步字段，**逐条合并**。

**曲库表（`artists` / `albums` / `tracks` / `track_files` / `track_artists` / `tracks_search`）不参与同步**（§0.1），所以它们既没有 `uid` 也没有同步字段，互相之间是普通外键关系。

这里有两处改动过的地方，值得记下来：

1. **`playlist_items` 原本被归为"关系表，随列表整体替换"，现在改成逐条同步的实体。** 原因是整体替换做不了两件事——按条目删除无法传播（会复活），以及用户无法只拒绝同步进来的某一首歌。改成逐条同步后这两件事都成立，代价是多一列 `uid` 和一个墓碑字段。
2. **`tracks` 原本带全套同步字段，现在全部去掉。** 曲库不同步之后，它的同步字段没有任何消费者；留着只会让人误以为它参与同步（以及多四个索引的写入开销）。

**`order_key` 用稀疏整数（间隔 1024），不用连续 `position`。** 实测：在 300 首的列表中间插入一首，连续 `position` 要改动 **298 行**（后面全部顺移），稀疏 `order_key` 只改 **1 行**。如果逐条同步，这就是"拖动一次歌曲上传 298 条变更"和"上传 1 条变更"的差别。间隔用尽时本地重排一次（500 条重排耗时 0.45 ms），**重排不得更新 `updated_at`**，否则重排会变成一次全表同步推送。

**规则二：所有时间戳都是 Unix 毫秒（`INTEGER`）。**
不用 SQLite 的 `DATETIME` 文本，不用 ISO 8601 字符串。毫秒整数可排序、可比较、跨时区无歧义、占 8 字节定长。

**规则三：曲库表用自增 `INTEGER PRIMARY KEY` 作主键；同步实体额外有一个跨设备的稳定标识。**
自增整数在两端会撞车（Windows 的第 5 号播放列表和 Android 的第 5 号不是同一个），所以同步实体带 `uid TEXT UNIQUE`（UUID），播放历史用 `uid`、评分用 `fingerprint`、听歌汇总用 `(stats_key, device_id)`、播放列表条目用 `uid` 并按 `ref_fingerprint` 去重。同步只认这些键。

---

## 2. 建表语句

### 2.1 元信息与设备

```sql
-- 键值元信息：schema 版本、本机设备 ID 等
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
-- 'schema_version' -> '1'
-- 'device_id'      -> 本机 UUID，首次启动时生成并永久保存

-- 曾经同步过的对端设备
CREATE TABLE devices (
    device_id    TEXT PRIMARY KEY,   -- UUID
    name         TEXT NOT NULL,      -- "xiyufe 的 PC"
    platform     TEXT NOT NULL,      -- 'windows' | 'android'
    last_sync_at INTEGER,            -- Unix ms，上次成功同步的时间
    last_error   TEXT                -- 上次失败原因，成功时清空
);
```

### 2.2 艺术家

```sql
CREATE TABLE artists (
    id         INTEGER PRIMARY KEY,
    name       TEXT    NOT NULL,          -- 显示名，来自标签，保留原始大小写
    name_norm  TEXT    NOT NULL,          -- 归一化后的匹配键，见 §3
    pinyin_key TEXT                       -- 拼音排序键，见 §3.2。v0.3 起填充
);

-- 归一化名唯一：
CREATE UNIQUE INDEX idx_artists_norm ON artists(name_norm);

-- 拼音序，走覆盖索引
CREATE INDEX idx_artists_pinyin ON artists(pinyin_key)
    WHERE pinyin_key IS NOT NULL;
```

没有 `uid`、没有同步字段：曲库不同步（§0.1），这张表纯粹是本机的扫描缓存。

**原来这里还有一列 `sort_name`（"Beatles, The"），已删除。** 理由见 §3.2：它和 `name_norm` 排出来的顺序在纯 ASCII 名字上完全相同，而在带变音符号的名字上**反而更差**——`sort_name` 保留 `ö`（U+00F6），SQLite 默认按码位比较会把它排到 `z` 之后，于是 `Björk` 掉到 B 区末尾；`name_norm` 去掉了变音符号，`bjork` 落在 `o` 的位置。删掉它，排序直接用 `ORDER BY name_norm`，不需要第二套规则。

### 2.3 专辑

```sql
CREATE TABLE albums (
    id              INTEGER PRIMARY KEY,
    title           TEXT    NOT NULL,
    title_norm      TEXT    NOT NULL,
    album_artist_id INTEGER REFERENCES artists(id) ON DELETE SET NULL,
    year            INTEGER,
    release_type    TEXT,   -- 'album' | 'single' | 'ep' | 'compilation' | 'live'
    pinyin_key      TEXT    -- 拼音排序键，见 §3.2。v0.3 起填充
);

-- 同一专辑艺术家下的同名专辑视为同一张
CREATE UNIQUE INDEX idx_albums_norm
    ON albums(title_norm, IFNULL(album_artist_id, -1));

CREATE INDEX idx_albums_pinyin ON albums(pinyin_key)
    WHERE pinyin_key IS NOT NULL;
```

同样不带同步字段。

`album_artist_id` 与曲目的 `track_artists` 是两套东西，必须分开：

- **专辑艺术家**是整张专辑的署名。合辑里它是 "Various Artists"，同一位艺术家的专辑页靠它聚合。
- **曲目艺术家**是这首歌的演唱者。合辑里每首歌不同。

只存一个的话，要么合辑的所有歌都被标成 "Various Artists"（丢失真实演唱者），要么合辑碎成 N 张单曲专辑。

> `idx_albums_norm` 里用 `IFNULL(..., -1)` 是因为 SQLite 的 `UNIQUE` 认为多个 `NULL` 互不相等，不处理的话"无专辑艺术家的同名专辑"会重复入库。

### 2.4 曲目

```sql
CREATE TABLE tracks (
    id          INTEGER PRIMARY KEY,
    -- 曲目身份指纹 = 创建这条曲目时那个文件的指纹。一旦建立就不再改变，
    -- 即使本机所有文件都消失、或后来扫到另一个拷贝。见 §2.5 与 §4。
    fingerprint TEXT    NOT NULL UNIQUE,

    -- 统计聚合键 = 归一化(艺术家) + 归一化(标题)，见 §4.6。
    -- 可为 NULL：没有标题标签的文件在听歌汇总中的标记为“未识别”
    work_key    TEXT,

    album_id    INTEGER REFERENCES albums(id) ON DELETE SET NULL,

    -- 主艺术家：track_artists 里 role='main' 且 position 最小的那一位，冗余在此
    -- 用途一：列表页只显示主艺术家时省掉两跳 JOIN（实测 5 万首省 2.4–3.0 ms/页）
    -- 用途二：work_key 的艺术家部分取它，得到稳定且两端一致的值（见 §4.6）
    -- 完整的多艺术家关系仍在 §2.6 的 track_artists 里，这里只是它的派生冗余
    primary_artist_id INTEGER REFERENCES artists(id) ON DELETE SET NULL,

    title       TEXT    NOT NULL,
    title_norm  TEXT    NOT NULL,
    track_no    INTEGER,
    disc_no     INTEGER,
    duration_ms INTEGER NOT NULL,         -- 见 §5
    year        INTEGER,
    genre       TEXT,

    -- 音频属性。同一 fingerprint 意味着音频数据逐字节相同，所以这些属性属于曲目而非文件
    codec       TEXT,                     -- 'flac' | 'mp3' | 'aac' ...
    sample_rate INTEGER,
    channels    INTEGER,
    bitrate     INTEGER,                  -- 平均码率 bps

    -- 派生列：本机是否存在至少一个 present = 1 的 track_files 行。每次扫描结束时统一刷新。
    -- 它与文件列表是冗余的，但让"缺文件"的查询不必每次都走子查询。
    has_local_file INTEGER NOT NULL DEFAULT 0,

    added_at    INTEGER NOT NULL          -- 首次入库时间
);

CREATE INDEX idx_tracks_album   ON tracks(album_id);
CREATE INDEX idx_tracks_artist  ON tracks(primary_artist_id);
CREATE INDEX idx_tracks_added   ON tracks(added_at DESC);
CREATE INDEX idx_tracks_missing ON tracks(has_local_file);
CREATE INDEX idx_tracks_workkey ON tracks(work_key) WHERE work_key IS NOT NULL;
```

**`rating` 不在这里。** 它搬去了 §2.14 的 `track_ratings`，因为曲库不同步而评分要同步——评分必须以指纹为键独立存在。

**三个键各管一件事，不要合并。** `id` 是行身份，被 `track_files`、`track_artists`、`playlist_items` 三处外键引用；`fingerprint` 是内容身份，跨设备有效；`work_key` 是统计口径，跨版本有效。

有人会问既然有 `fingerprint` 为什么还要 `id`。实测（5 万曲目 + 6 万文件）用 `fingerprint` 直接做主键，查询速度**几乎无差别**（首页 50 条 3.039 ms vs 3.097 ms，文件表外键查 0.0114 ms vs 0.0112 ms），但库体积从 13.39 MiB 涨到 16.96 MiB（**+3.57 MiB，+26.7%**），因为 6 万条 `track_files` 每条要存 64 字节指纹而不是 4 字节整数。10 万首曲库约 +12 MiB。保留 `INTEGER id` 还留了一层余地：将来若允许重新编码文件（FLAC→AAC），指纹会变但行身份可以延续，评分与播放列表引用不断。

**没有 `raw_title` / `raw_artist` / `raw_album`。** 曾经有，理由是"标签解析错了能回溯"。但它是**可重建的缓存**，不是真源——文件还在，随时能重读；而它本来想兼职的"检测文件被外部改过"已经有 `file_size` + `file_mtime` 在做。实测 5 万首三列要 +1.74 MiB（**+89.5%**，每首约 36 字节，中文与日文标签还更占）。所以去掉：需要原文时重读文件，不需要在库里存第二份。

**没有 `deleted_at`。** 用户删歌就是 `DELETE` 这一行（连带级联删掉 `track_files` 与 `track_artists`）。曲库不参与同步，所以不需要墓碑：墓碑存在的唯一理由是把"删除"传播到另一台设备，而没有另一端需要被通知。

**`tracks` 表里没有 `file_path`。** 文件路径搬去了 §2.5 的 `track_files` 表。

**`codec` / `sample_rate` / `channels` / `bitrate` 留在曲目上而不是文件上**，因为同一个 `fingerprint` 意味着音频数据逐字节相同，这些属性必然一致；放在文件表里会重复存储 N 份完全相同的值。

**文件丢失不是删除。** 用 `has_local_file = 0` 表达，不用删除或墓碑。混用这两件事会导致一个真实故障：用户把移动硬盘拔掉，扫描后所有歌被当成删掉，插回去再扫，歌全没了。有了 `has_local_file`，拔盘后歌只是灰显，插回来立刻恢复。

### 2.5 文件（一首曲目可以对应多个文件）

```sql
CREATE TABLE track_files (
    id          INTEGER PRIMARY KEY,
    track_id    INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,

    file_path   TEXT    NOT NULL UNIQUE,  -- 绝对路径，本机唯一
    folder_path TEXT    NOT NULL,         -- 所在目录，供"按目录浏览"直接索引
    file_name   TEXT    NOT NULL,         -- 含扩展名，排序与显示用

    file_size   INTEGER NOT NULL,         -- 重命名检测与"内容是否变化"的判据
    file_mtime  INTEGER NOT NULL,

    present     INTEGER NOT NULL DEFAULT 1,  -- 0 = 上次扫描没见到（可能被删，也可能被移走）
    added_at    INTEGER NOT NULL,

    -- 这三列只服务于"低精度文件系统上的增量判断"，见下方小节
    fp_size         INTEGER,   -- 算 fingerprint 时的文件大小
    fp_mtime        INTEGER,   -- 算 fingerprint 时的 mtime
    fp_verified_at  INTEGER    -- 这次指纹计算结果被确认可信的时刻
);

CREATE INDEX idx_files_track  ON track_files(track_id);
CREATE INDEX idx_files_folder ON track_files(folder_path, file_name) WHERE present = 1;
CREATE INDEX idx_files_absent ON track_files(file_size, file_mtime)    WHERE present = 0;
```

**为什么文件要单独一张表。** 用户完全可能把同一首歌同时放在 `D:\音乐\` 和 `D:\下载\` 里。如果路径是 `tracks` 的列，一首歌就只能记住一个位置：

- 删掉其中一个拷贝，歌在本机就没有文件了，即使另一份还在；
- 反过来若要支持多份，就得在 `tracks` 里存逗号分隔的路径串——这是把关系塞进字符串，排序、按目录浏览、路径唯一性检查全部失效。

拆表之后，一条 `tracks` 行对应 N 个文件，删掉一份另外一份照常能播。实测验证（10 个场景）：两份拷贝 → 一条曲目两行文件；删掉一份 → `has_local_file` 仍为 1；全删 → `has_local_file` 变 0；文件回来 → 曲目行不变、播放历史不断；重命名认作改名（行数仍 1）；复制一份仍是同一曲目（两条文件行）；按目录浏览命中索引；`is_missing` 编译结果正确；主动删除的不会被扫描复活；级联删除生效。

**`present` 不是墓碑。** 文件表也要区分"用户删了这个文件"和"这次扫描没看到"——但两者在文件层面没有区别，也没有同步需求（路径本来就不跨设备同步），所以用一个布尔量就够。持久化它的用处是：下一次扫描可以拿 `(file_size, file_mtime)` 去和 `present = 0` 的行比对，从而把**重命名**认出来而不是当成"删了一个、新增一个"。

**重命名的识别顺序（扫描器对每个文件做的事）：**

```text
1. 这个路径已经在库里？
     是 → 更新 size/mtime，present = 1，完事
2. 有没有 present = 0 且 (size, mtime) 完全相同的行？
     有 → 认作重命名：改路径，present = 1，不新增行
3. 按 fingerprint 找曲目
     找到 → 挂上去（可能是另一份拷贝）
     没找到 → 新建曲目
4. 插入或更新 track_files 行
```

**第 2 步必须排在第 4 步之前。** 这一点踩过坑：先按 `file_path` 做 upsert，再跑重命名识别的 UPDATE，会撞 `UNIQUE constraint failed: track_files.file_path`——因为 upsert 已经把新路径那行插进去了，随后的 UPDATE 又想把它改成同一个路径。

实测：把一个文件改名后重扫，`track_files` 行数仍然是 1，没有变成两行。

**为什么 `tracks.fingerprint` 与文件指纹分开。** 曲目身份必须稳定：歌曲的 `uid` 被播放列表、播放历史、评分引用，一旦变了，用户的听歌记录就和歌对不上了。所以曲目的 `fingerprint` 在创建时定下、之后永不改变，它是"这条曲目最初是从哪个文件认出来的"的记录。文件自己不需要单独的指纹列——音频内容相同就意味着同一首曲目，路径与 `(size, mtime)` 已经足够做增量判断。

**已知局限：同一首歌的两个拷贝如果内容不完全相同（比如一份是 320kbps、一份是 V0），指纹不同，会被当成两首曲目。** 这是 §4 已声明的取舍，跨编码识别需要声学指纹，属于将来的工作。不过统计不会因此断掉——它们的 `work_key` 相同，见 §4.6。

**`file_path` 是 `NOT NULL`，但曲目可以没有文件。** 同步来一份播放列表，其中某首歌本机没有，对应的 `tracks` 行存在且 `has_local_file = 0`、`track_files` 里没有任何行。条目必须保留——丢掉它等于同步会破坏数据。

#### 低精度文件系统上的增量判断

FAT32 的 mtime 粒度是 **2 秒**，exFAT 是 10 毫秒。音乐库放在移动硬盘上时，"2 秒内改了两次"就会漏检：第一次扫描记下 `mtime=10000`，用户改写文件而 mtime 仍在 `10000` 这一格里，第二次扫描以为没变。

**解法不探测文件系统类型，一律按最坏情况（2 秒）处理。** 探测挂载点类型要写平台相关代码，而且网络共享、虚拟机共享目录的文件系统根本探测不出来——按最坏情况处理反而更简单也更可靠。

```text
可疑  ⇔  (已存 mtime + 2000ms > fp_verified_at)
      且  (已存 mtime <= 本次扫描时刻)          ← 必须有这条，否则 mtime 在未来的文件永远重算

fp_verified_at = max(本次扫描开始时刻, 该文件当时的 mtime)
```

第二条必须加。实测不加的后果：mtime 在未来的文件（从别的设备拷来、或系统时间被改过）会让条件恒真，**每一轮扫描都重算一次指纹，永远停不下来**。加上之后第一次算完就稳定。

**代价实测：** 20000 首的真实感曲库误判重算 **0 首**；批量导入 300 首之后一小时内多算 300 个指纹，约 **0.15 秒**（0.5 ms/首）。而 `library.scan_delay_seconds` 默认 30 秒，意味着启动扫描时这些文件早已"过气"，代价归零。

**另一个必须做的检查：** 算指纹前后各 `stat` 一次，两次结果（size 或 mtime）不一致就放弃这一轮，不写库。否则会把一个"算的时候正在被改写"的指纹记下来，之后文件没再变，这个错指纹就永久留在库里了。

### 2.6 曲目-艺术家（多对多）

```sql
CREATE TABLE track_artists (
    track_id  INTEGER NOT NULL REFERENCES tracks(id)  ON DELETE CASCADE,
    artist_id INTEGER NOT NULL REFERENCES artists(id) ON DELETE CASCADE,
    role      TEXT    NOT NULL DEFAULT 'main',  -- 'main'|'featured'|'remixer'|'composer'
    position  INTEGER NOT NULL DEFAULT 0,       -- 显示顺序
    PRIMARY KEY (track_id, artist_id, role)
);
```

没有同步字段：它属于 `tracks` 的一部分，随曲目整体替换。

### 2.7 播放列表

```sql
CREATE TABLE playlists (
    id          INTEGER PRIMARY KEY,
    uid         TEXT    NOT NULL UNIQUE,
    name        TEXT    NOT NULL,
    description TEXT,
    kind        TEXT    NOT NULL DEFAULT 'manual',  -- 'manual' | 'smart'
    smart_query TEXT,                               -- kind='smart' 时的查询定义
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL,
    updated_by  TEXT    NOT NULL,
    revision    INTEGER NOT NULL DEFAULT 1,
    deleted_at  INTEGER
);

CREATE TABLE playlist_items (
    id          INTEGER PRIMARY KEY,

    -- 条目自身的 UUID。逐条同步的去重键，也是"这一条"的身份
    uid         TEXT    NOT NULL UNIQUE,

    playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,

    -- 排序键：稀疏整数，间隔 1024。不用连续 position，理由见 §1 规则一
    order_key   INTEGER NOT NULL,

    -- 本机解析结果；本机没有这首歌时为 NULL
    track_id    INTEGER REFERENCES tracks(id) ON DELETE SET NULL,

    -- 便携引用：跟着条目一起同步，让对端能凭它找到自己的那份文件
    ref_fingerprint TEXT,            -- 内容指纹，唯一真正跨设备有效的引用键
    ref_title       TEXT,            -- 指纹没命中时的标签回退
    ref_artist      TEXT,
    ref_album       TEXT,
    ref_duration_ms INTEGER,

    added_at    INTEGER NOT NULL,

    -- 同步字段：条目现在是逐条同步的实体
    updated_at  INTEGER NOT NULL,
    updated_by  TEXT    NOT NULL,
    revision    INTEGER NOT NULL DEFAULT 1,
    deleted_at  INTEGER              -- 条目墓碑，见 §7
);

CREATE INDEX idx_items_playlist ON playlist_items(playlist_id, order_key)
    WHERE deleted_at IS NULL;
CREATE INDEX idx_items_sync     ON playlist_items(updated_at);
```

**不能建 `UNIQUE(playlist_id, ref_fingerprint)` 约束。** 两个设备可能各自往同一个列表里加同一首歌，生成两个不同的条目 `uid`——这是两条独立记录，数据库层面看不出来它们是"同一件事"。去重只能在合并时按 `ref_fingerprint` 判断，**不能交给约束**。实测确认：两台设备各加一首 `S`，合并后是两条 `uid` 不同的行。

**`ref_uid` 没有了。** 原来条目上有一列冗余的曲目 `uid`，曲库移出同步之后它失去了意义——两端根本没有共同的曲目 uid。跨设备唯一有效的引用键是 `ref_fingerprint`，它由音频内容算出，两端独立计算也能一致。

**为什么条目要冗余存标签？** 假设 Windows 上有 300 首的播放列表，Android 上只同步来 250 首。若只存 `track_id`，剩下 50 条无处可指，只能丢弃——下次你回到 Windows，那 50 首就永远从列表里消失了。冗余存 `ref_*` 后，Android 把 50 条标为「缺失」灰显保留，等你把那 50 首文件拷过去，它们自动复活。

**为什么必须是两张表，不能把条目塞进 `playlists` 的一个 JSON 列？** 这是最直觉的替代方案：一张表、一个 `items_json TEXT`，读写都是整体替换。实测 300 首的列表在中间插入 1 首：

| | JSON 单列 | 两张表 | 差异 |
| --- | --- | --- | --- |
| 一次插入的同步增量 | 56.0 KiB（整行重传） | 117 字节（1 行） | **490 倍** |
| 查「这首歌在哪些列表里」 | 0.33 ms（解析全部列表） | 0.049 ms（索引查询） | **7 倍** |

写入耗时两者相当（2.96 ms vs 2.63 ms），也就是说 JSON 方案在**单机**上完全够用。杀手是同步：`playlist_items` 是逐条同步的实体（§1 规则一），改一条就只传一条；JSON 列是一整块，改一条要传 56 KiB，3000 首的列表就是 560 KiB，而且**永远无法表达"只拒绝其中某一首"**，也没法给单条打墓碑。条目墓碑、浮窗里的逐条勾选、两设备各加一首的去重，全都依赖"条目是独立实体"，JSON 列一条都做不到。

另外两个次要理由：JSON 列没法建索引（按 `order_key` 排序要全量解析）；改一条要重写整行，SQLite 的 WAL 里留下完整的一份旧副本，写入放大与列表长度成正比。

### 2.8 设置

```sql
CREATE TABLE settings (
    key        TEXT PRIMARY KEY,
    value      TEXT NOT NULL,            -- JSON 编码的值
    scope      TEXT NOT NULL DEFAULT 'shared',  -- 'shared' | 'device'

    updated_at INTEGER NOT NULL,
    updated_by TEXT    NOT NULL,
    revision   INTEGER NOT NULL DEFAULT 1,
    deleted_at INTEGER
);
```

`scope` 是必要的：**有些设置天生不该同步。**

- `shared`：主题、语言、EQ 曲线、音量、播放模式、交叉淡化时长。
- `device`：输出设备名、缓冲帧数、**输出模式三档（`audio.output_mode`）**、**动态取色开关（`ui.dynamic_color`）**、窗口位置尺寸、Android 上是否用 SAF（存储访问框架）、**音乐库所在目录列表**。

把 `device` 的设置也同步，结果是 PC 的 "WASAPI 独占" 被推到手机上，手机找不到那个设备，播放直接哑掉。同理，音乐库目录是绝对路径，跨设备同步毫无意义且会让对端反复扫描一个不存在的目录。

`audio.output_mode` 取值 `Shared` / `Exclusive` / `BitPerfect`，默认 `Shared`。三档的语义与契约（尤其 `BitPerfect` 协商失败必须报错、不许静默降级）见 [TODOLIST.md](TODOLIST.md) 的 T-B15。这里只强调作用域：**它必须是 `device`**，因为它描述的是"这台机器上有没有那个 DAC"，不是用户的偏好。

`ui.dynamic_color`（bool，默认 `false`）同理是 `device` 作用域：它取的是**这台设备的壁纸**，换台手机就是另一套颜色，同步过去只会让另一台设备的品牌色莫名其妙地变掉。视觉规格的三条定案见 [TODOLIST.md](TODOLIST.md) 的 T-H16。

### 2.8.1 曲库相关的设置项

| 键 | 类型 | 默认 | 作用域 | 文案 |
| --- | --- | --- | --- | --- |
| `library.folders` | 字符串数组 | `[]` | `device` | 「音乐库目录」 |
| `library.scan_on_start` | bool | `true` | `device` | 标题「启动时扫描变更」，副标题「在后台检查音乐库有没有新增或移动的文件」 |
| `library.scan_delay_seconds` | 整数 | `30` | `device` | 启动后延迟多久开始扫描。不暴露在 UI 上，留给调试 |

**`library.scan_on_start` 默认开、缓慢扫描以减小资源占用。**

**为什么不做文件系统监听。** Windows 的 `ReadDirectoryChangesW` 有一个目录一个句柄的成本，监听 8 万个目录会耗尽句柄或触发系统限制；Android 上通过 SAF 拿到的 `content://` 树根本不提供变更通知。两个平台都不可靠的事，做了反而会给人一种"它是实时的"的错觉。宁可让用户明确知道"扫描是定时的、可以手动触发"。

**扫描过程中的进度要可中断。** 扫描在工作线程跑，播放不等待它；添加终止按钮，用户关软件或中途终止时，已经写入的行保留，下次扫描从"还没算指纹的那些"继续。

### 2.8.2 曲库扫描的算法（写在这里是因为它同时决定了两张表怎么用）

```text
启动 / 手动触发
  ↓
1. 读取 library.folders，递归枚举，对每个文件 stat() 得到 (path, size, mtime)
   —— 只读元数据，不打开文件内容       实测 10 万文件约 3.0 秒
  ↓
2. 一次性把所有 track_files.present 置 0
  ↓
3. 对每个枚举到的文件：
     a. 路径已在库中？ → 更新 size/mtime，present = 1
     b. 有 present = 0 且 (size, mtime) 相同的行？ → 认作重命名，改路径，不新增
     c. 按剥标签指纹查 tracks：
          找到 → 挂到该曲目（这是同一首歌的另一份拷贝）
          没找到 → 新建 tracks 行，再插入 track_files 行
  ↓
4. 刷新 tracks.has_local_file = EXISTS(present = 1 的文件行)
  ↓
5. 对还没算过指纹/标签的新文件，在后台逐步补齐（可随时中断）
```

**步骤 1 和步骤 3 必须分开。** 步骤 1 只有 `stat`，3 秒就跑完，可以给用户一个准确的"发现 N 个文件"的进度；步骤 3 要读文件内容，慢得多。合并成一步的话，用户会盯着一个不知道要多久的进度条。

**步骤 3c 的指纹计算是真正的瓶颈**，所以它必须只在"新文件"或"内容变了"时才发生。文件没变就连读都不读——这是增量扫描的定义。

**`present` 先全置 0 再逐个置 1 的做法**，比"扫描完再比对哪些路径消失了"简单，且不需要在内存里保留完整路径集合（10 万条路径字符串约 10 MB，不算大但没必要）。

### 2.9 播放队列（设备本地，不参与同步）

```sql
CREATE TABLE play_queue (
    id       INTEGER PRIMARY KEY,
    position INTEGER NOT NULL,
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,

    -- 这条是怎么进队列的：决定 UI 上的颜色，也是你之前提的"插播要区分开"
    origin   TEXT    NOT NULL,
    -- 'user'     用户主动排进来的
    -- 'play_next' 临时插播，播完即弃
    -- 'context'  由专辑/播放列表自动续上的
    -- 'shuffle'  随机顺序生成的

    added_at INTEGER NOT NULL
);
```

队列**不进同步**：两台设备的播放进度天然不同步，强行同步只会打架。它保存在本地，重启后恢复，仅此而已。

`origin` 正是支撑你要求的「插播歌曲用颜色区分」的字段——UI 不需要猜测某条是插播的，它直接读这个值。

#### 四个 `origin` 分别是什么意思

这一列回答的是**一个问题：这条是怎么进来的**。分开记的原因不是 UI 要四种颜色（UI 只上一种色），而是事后能回答"用户到底主动点了什么"，这是后面做队列行为和统计时唯一能依赖的依据。

| 值 | 谁写进去的 | 具体场景 | 播完之后 |
| --- | --- | --- | --- |
| `user` | 用户显式操作 | 在歌曲上点「添加到队列」、把整张专辑拖进队列、拖动排序后的那些行 | **留着**，除非用户清空 |
| `play_next` | 用户点「下一首播放」 | 正在听 A，想让 B 紧跟着播，但不想打乱后面已排好的顺序 | **用完即弃**（播完就从队列里移走） |
| `context` | 系统自动续 | 用户直接双击某专辑/某播放列表里的第 3 首开始播，系统把整个专辑/列表按顺序补进队列 | 留着，但当"当前播放上下文"换掉时整批撤掉 |
| `shuffle` | 系统生成 | 对同一个上下文打开了随机播放，队列顺序由洗牌算法生成而不是专辑/列表原顺序 | 留着；播完一轮触发重洗 |

**`user` 与 `context` 的区别在于"谁决定的顺序"。** 用户双击专辑第 3 首时，队列里第 4 首之后是系统按专辑原序补的（`context`）；如果用户后来又手动把第 7 首拖到第 4 位，那一行就变成了 `user`——因为它已经不代表专辑原序了。

**`play_next` 之所以要单独记，是因为它必须"插进中间"而不是"追加到末尾"。** 队列是排序的，插播行会被插到当前播放行的紧后面；它播完就删，所以用户不会看到它一直占着位置。

**`shuffle` 记的是"这一行的顺序来自洗牌结果"，不是"用户开了随机"。** 关闭随机后队列回到原顺序，那些行会重新变回 `context`（或 `user`，如果被手动调整过）。持久化的洗牌顺序存在队列行本身（`position` 就是洗牌后的位置），不需要额外的表。

**为什么不合并成一个 `bool is_inserted`：** 合并之后"用户主动排的"和"系统补的"就分不开了——而这两者在下一次用户点了「清空队列」时行为不同（前者是用户的东西，清空前要确认；后者是系统补的，可以直接丢）。分开记的成本只是一列文本，换来的是将来所有队列行为都有一致的判据。

### 2.10 播放记录（参与同步）

这里改了主意：最初写的是"设备本地、不参与同步"，但你要做**听歌统计**，而统计必须汇总两端的记录才有意义。

改成同步后，设计上的核心问题是**如何避免重复计数**：同一首歌可能被两台设备各播一次（这是两次真实收听，应该记两条），也可能同一条记录被同步了两遍（这是 bug，应该只有一条）。这两件事在数据上必须可区分，所以每条记录带自己的 `uid`，同步按 `uid` 去重——和曲目按 `fingerprint` 去重是同一个思路。

```sql
CREATE TABLE play_history (
    id         INTEGER PRIMARY KEY,
    uid        TEXT    NOT NULL UNIQUE,   -- 记录自身的 UUID，同步去重键

    -- 曲目标识：用指纹而不是 uid。曲库不同步（§0.1），两端没有共同的曲目 uid，
    -- 只有指纹是两端独立算出来的同一个值。
    track_fingerprint TEXT NOT NULL,

    -- 统计聚合键，写入时按当时的标签算好并冻结。NULL = 这个文件没有标题标签，
    -- 不参与听歌汇总（见 §4.6）
    work_key   TEXT,

    played_at  INTEGER NOT NULL,          -- 开始播放时间，Unix ms
    played_ms  INTEGER NOT NULL,          -- 实际播放时长（不是曲目时长）

    -- 结束原因
    end_reason TEXT    NOT NULL,
    -- 'completed'   播到结尾
    -- 'skipped'     用户主动跳过
    -- 'stopped'     用户停止/关闭
    -- 'error'       播放出错

    -- 记录发生时的曲目信息快照。即使曲目后来从库里删除，统计仍然成立。
    title_snapshot  TEXT,
    artist_snapshot TEXT,
    duration_ms     INTEGER,

    device_id  TEXT    NOT NULL,
    source     TEXT,                      -- 'album' | 'playlist' | 'search' | 'queue' | ...

    -- 写入时按当时的阈值算好并冻结，事后不再重算（理由见下）
    counted           INTEGER NOT NULL,   -- 0/1，这条算不算"听了一次"
    count_threshold   INTEGER NOT NULL,   -- 当时用的是百分之多少，仅供展示和排查

    updated_at INTEGER NOT NULL,          -- 与 §1 规则一致
    updated_by TEXT    NOT NULL,
    revision   INTEGER NOT NULL DEFAULT 1,
    deleted_at INTEGER
);

CREATE INDEX idx_history_time  ON play_history(played_at DESC);
CREATE INDEX idx_history_track ON play_history(track_fingerprint, played_at DESC);
CREATE INDEX idx_history_work  ON play_history(work_key, played_at DESC) WHERE work_key IS NOT NULL;
CREATE INDEX idx_history_sync  ON play_history(updated_at);
```

**为什么是 `track_fingerprint` 而不是 `track_uid`：** 曲库不同步，两端各自扫自己的文件，算出来的 uid 毫无关系。而指纹是**函数**——同一段音频在 PC 和手机上算出同一个值。实测：PC 记 2 次、手机记 2 次，按 `track_uid` 聚合成两组各 2 次（错），按 `track_fingerprint` 聚合成 4 次（对）。

**`work_key` 使"换版本后统计不断"。** 你把一首歌从 MP3 换成 FLAC，指纹变了（确实是另一个文件），但 `work_key` 只由标签算，不变。统计按 `work_key` 聚合，所以「晴天」的总次数不会因为换了音质而归零。详见 §4.6。

**`work_key` 也必须在写入时冻结，不能查询时再算。** 与 `counted` 同一条规则：查询时算意味着改了标签就会重写历史统计；而标签是会被用户的整理工具改的。

**为什么还要三个快照列：** 播放记录要跨设备合并，某一端的 `track_fingerprint` 在本机可能根本找不到——那首歌本机没有。这时按指纹 JOIN 会得到空行，统计里就丢了这次收听。所以存 `title_snapshot` / `artist_snapshot` / `duration_ms`：**统计优先 JOIN 到真实曲目，JOIN 不到就用快照**。这样即使你删掉了某个文件，历史记录里的"听过 37 次"依然成立。

**`played_ms` 与 `duration_ms` 的区分是统计正确性的关键：** 你听了一首歌的前 20 秒就跳过，这算不算"听过一次"？两种统计口径都要能算出来：
- 「播放次数」= `count(*) WHERE counted = 1`
- 「总听歌时长」= `sum(played_ms)`（包括跳过的）

如果只存一个"是否听完"的布尔值，这两个数就都算不准。

### 「算不算一次播放」在写入时定死，之后不改

用户可以在设置里定"听够多少百分比才算听过一次"（默认 50%）。**这个判断在记录写入的那一刻就做完，结果存进 `counted` 列，此后再也不重算。**

```text
写入时：counted = (played_ms * 100 >= duration_ms * 当时的阈值) ? 1 : 0
```

**为什么不做"改设置就重算全部历史"：**

1. **重算会让历史数字变来变去。** 用户看到「晴天」听了 42 次，把阈值从 50% 调到 90%，第二天变成 31 次。同一个软件、同一份历史，两次看两个数，这是 bug 的表现而不是功能的表现。
2. **重算和增量同步天然冲突。** 记录是跨设备合并的。如果每台设备按自己的设置重算，PC 和手机会对同一条记录得出不同的 `counted`，同步时这条记录到底算不算？没有正确答案。而把决定写进行里，它就是一条**普通数据**，同步规则不需要为它开特例。
3. **重算需要永远保留全部原始记录。** 一旦清理历史，早期记录已经没有百分比信息了。不重算就没有这个约束，清理策略可以独立决定。

**代价（说清楚）：** 用户把阈值从 50% 调到 90% 之后，90% 只对**新听**的歌生效，旧的 50% 判定保留。这是有意的取舍——统计口径应当稳定，而不是随设置漂移。

**设置项：**

| 键 | 类型 | 默认 | 作用域 | 文案 |
| --- | --- | --- | --- | --- |
| `library.play_count_threshold` | 整数（百分比） | `50` | `shared` | 标题「听够多少算一次播放」，副标题「更改阈值不会改变以前的计数」 |

副标题这句是必须的，不是装饰。用户把 50% 改成 90% 之后，统计里的旧数字**不会**变小，如果不提前讲清楚，第一反应会是"这个设置没生效"或者"软件坏了"。

**不在统计页面加任何解释文字。** 统计页面是看数字的地方，加脚注会让它变成说明书。用户想知道阈值怎么起作用，去设置里就能看到那句话；不想知道的人，也不该在统计页被一段文字打断。

`count_threshold` 列记下当时用的阈值，用途只有一个：排查"为什么这首歌只听了 40% 却算了一次"——看这一列就知道那次是按早期更宽松的阈值判的。

**判断依据不能是 `end_reason = 'completed'`。** 一首 4 分钟的歌听到 90% 跳过，`end_reason` 记的是 `'skipped'`，但在 50% 阈值下它**应该**算一次播放。所以 `counted` 由比例算，不由状态列算。

**数据量估算：** 每天听 3 小时、平均 4 分钟一首，约 45 条/天，一年 1.6 万条。每行约 220 字节（多了 `work_key`），一年约 3.5 MB，十年 35 MB。**不需要归档策略**（默认就是永久保留），但这意味着 WebDAV 同步时不要每次都传全量——按 `updated_at` 水位增量传输。实测 1 年 16000 条 gzip 后 **1.12 MiB**。

**重复计数防护：** 同步时按 `uid` 做 `INSERT ... ON CONFLICT(uid) DO NOTHING`。同一台设备重复上传自己已有的记录，是幂等的。两台设备各播一次，是两条不同 uid 的记录，正确地算两次收听。

### 2.11 搜索：先用 LIKE，别急着上 FTS5

最初的设想是 FTS5 配 `trigram` 分词器，理由是中英文都能做子串匹配。**我实测之后推翻了这个方案**，原因如下。

**trigram 无法处理少于 3 个字符的查询。** FTS5 的 trigram 分词器按三字符滑窗建索引，查询串短于 3 字符时匹配不到任何东西。实测（SQLite 3.53.1）：

| 查询 | 长度 | trigram 结果 | `LIKE '%…%'` 结果 |
| --- | --- | --- | --- |
| 夜 | 1 | ✗ 无结果 | ✓ 命中 |
| 夜曲 | 2 | ✗ 无结果 | ✓ 命中 |
| 晴天 | 2 | ✗ 无结果 | ✓ 命中 |
| 稻香 | 2 | ✗ 无结果 | ✓ 命中 |
| 七里香 | 3 | ✓ 命中 | ✓ 命中 |
| 周杰伦 | 3 | ✓ 命中 | ✓ 命中 |

中文歌名两字极多（«晴天»«稻香»«夜曲»«红豆»«后来»），用户搜索时打的往往正是这两个字。一个在搜索框里输「晴天」返回空白的播放器是不可用的——这比"搜索慢 50 毫秒"严重得多。

**而 LIKE 其实够快。** 同一个实测里，20000 行曲库上 `LIKE '%夜曲%'` 全表扫描耗时 **1.2 毫秒**。即使曲库到 10 万首，也只是约 6–10 毫秒，仍在"输入即时响应"的范围内。

所以：

```sql
-- 一张普通的表，一个普通的索引（仅用于前缀匹配和排序，不用于子串搜索）
CREATE TABLE tracks_search (
    track_id   INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
    title_norm TEXT NOT NULL,
    artist_norm TEXT NOT NULL,
    album_norm TEXT NOT NULL
);
```

搜索走 `WHERE title_norm LIKE '%'||?||'%' OR artist_norm LIKE '%'||?||'%'`，输入时做防抖（约 150 ms），结果集上限 200 条。

**什么时候再引入 FTS5：** 曲库超过 10 万首、或实测单次查询超过 30 ms 时。届时正确做法**不是** trigram，而是 `unicode61` 分词器 + 自己的中文二元组分词（把「晴天」索引成「晴天」，把「周杰伦夜曲」索引成「周杰」「杰伦」「伦夜」「夜曲」），这能同时支持中文双字查询和英文整词查询，代价是索引体积翻倍。这属于明确的将来优化，不是现在的问题。

顺带说明：`title_norm` / `artist_norm` / `album_norm` 与 §3 的归一化规则一致，搜索用的是归一化后的列，所以大小写、变音符号、多余空格都不影响命中。

### 2.12 同步状态与同步日志

```sql
CREATE TABLE sync_state (
    remote       TEXT PRIMARY KEY,   -- WebDAV 路径或对端 device_id
    last_cursor  INTEGER NOT NULL DEFAULT 0,  -- 上次同步到的 updated_at 水位
    last_sync_at INTEGER,            -- 上次成功同步的时间
    last_error   TEXT
);
```

光有"上次成功时间"和"最后一条错误"不够。设置页要展示的是一份**上一次同步的分项报告**（哪几类成功、哪几类失败、失败原因），而单个 `last_error` 字段只能记住最后一条，且会被下一次成功覆盖。所以另外存两份：

```sql
CREATE TABLE sync_runs (
    id          INTEGER PRIMARY KEY,
    started_at  INTEGER NOT NULL,
    finished_at INTEGER,
    trigger     TEXT    NOT NULL,
    result      TEXT    NOT NULL,

    CONSTRAINT ck_runs_trigger CHECK (trigger IN ('manual', 'auto')),
    CONSTRAINT ck_runs_result  CHECK (result  IN ('ok', 'no_change', 'partial', 'failed'))
);

CREATE INDEX idx_sync_runs_started ON sync_runs(started_at);

CREATE TABLE sync_run_items (
    run_id    INTEGER NOT NULL REFERENCES sync_runs(id) ON DELETE CASCADE,
    category  TEXT NOT NULL,
    result    TEXT NOT NULL,
    -- 对端推给本机的条数 / 本机推给对端的条数，用于"同步了几项"的展示
    count_in  INTEGER NOT NULL DEFAULT 0,
    count_out INTEGER NOT NULL DEFAULT 0,
    detail    TEXT,

    PRIMARY KEY (run_id, category),
    CONSTRAINT ck_items_result CHECK (result IN ('ok', 'no_change', 'failed', 'skipped'))
);
```

`category` 是同步的分类而不是表名，给用户看的是人能读的名字：`settings` / `playlists` / `ratings` / `history` / `devices`。理由：用户不关心 `playlist_items` 和 `playlists` 是两张表，他只关心"播放列表同步了吗"。

`sync_runs` 与 `sync_run_items` **都不参与同步**，是纯本机日志。实测验证过：外键 `ON DELETE CASCADE` 生效（删 run 后 items 归零）；超过 20 次后按 `id DESC LIMIT 20` 清理，行数稳定在 20；三个 `CHECK` 约束都真实拦住了非法值。一次同步约 5 行明细、不到 300 字节，保留 20 次总共 6 KB 量级，可以忽略。

### 2.13 歌词

**不建表。** 歌词从文件读，读完即用，不进数据库。理由：

- 内嵌歌词（ID3 的 `USLT`、FLAC 的 `LYRICS`、MP4 的 `©lyr`）本来就在音频文件里，TagLib 一次读取就能拿到。存进库里等于把同一份数据复制了一遍，还要处理"文件换了这个库没换"的失效问题。
- 外挂 `.lrc` 也是文件，随音频文件一起走。存进库里，跨设备同步时它变成了一份没有身份的数据（歌词属于文件，不属于曲目实体）。
- 歌词体积不小（一首中文歌 3–8 KB），全库十万首就是 300–800 MB 的纯冗余。

**解析顺序（这是唯一需要写死的规则）：**

```text
1. 内嵌歌词        总是优先，无论开关如何
        ↓ 没有
2. 同目录同名 .lrc  仅当设置开关打开
        ↓ 没有
3. 显示"暂无歌词"
```

**设置项：**

| 键 | 类型 | 默认 | 作用域 | 文案 |
| --- | --- | --- | --- | --- |
| `lyrics.external_lrc` | bool | `false` | `shared` | 标题「从 .lrc 中获取歌词」，副标题「总是优先获取内嵌歌词」 |

副标题不是装饰，它是这个开关的**行为说明**：打开它不会让 .lrc 覆盖内嵌歌词，只是在内嵌歌词缺失时多一条回退路径。用户看到这句就知道自己的内嵌歌词不会被抢。

**为什么默认关闭：** 内嵌歌词是歌曲的作者信息，通常比网上抓的 .lrc 准确、时间轴也对得上。外挂 .lrc 在整理过的曲库里好用，但在杂乱目录里经常是错的（同名不同版本）。默认关闭是"宁可没有，也不要错的"。

**编码问题得记一笔：** `.lrc` 没有统一编码。日文歌常见 Shift-JIS，中文老歌常见 GBK/GB18030，现代的多为 UTF-8（可能带 BOM）。读取时必须按顺序探测：BOM → UTF-8 严格校验 → 失败则 GB18030 → 再失败则 Shift-JIS → 最后 Latin-1 兜底。直接按 UTF-8 硬读会让一半的中文老歌词变成乱码。

**跨设备：** 不参与同步。歌词跟着文件走，文件在哪个设备，歌词就在哪个设备。一台设备上没这首歌，自然也没有它的歌词——这与播放列表条目"灰显保留"的处理不同，因为歌词不是用户数据，没有保留的价值。

### 2.14 星级评分（参与同步）

曲库不同步，所以评分不能挂在 `tracks` 上——那张表两端根本不对齐。评分必须以**指纹**为键独立成表：

```sql
CREATE TABLE track_ratings (
    id          INTEGER PRIMARY KEY,
    fingerprint TEXT    NOT NULL UNIQUE,   -- 键就是内容指纹，两端独立算得一致
    rating      INTEGER NOT NULL,          -- 0-5 星；0 = 用户明确清除到未评分

    updated_at  INTEGER NOT NULL,
    updated_by  TEXT    NOT NULL,
    revision    INTEGER NOT NULL DEFAULT 1,
    deleted_at  INTEGER
);

CREATE INDEX idx_ratings_sync ON track_ratings(updated_at);
```

**`rating` 是 `NOT NULL`，用 0 表示"未评分"。** 不能在这里用 `NULL`，也不能用"删掉这一行"来表达清除——那样清除就成了一次没有时间戳的删除，同步看不见它。实测过：PC 评 3 星（t=200）→ 手机改 4 星（t=300）→ PC 清除（t=400），三端最终都是未评分。**清除也是一次带时间戳的写。**

**换了版本评分就没了。** 评分以指纹为键，把 MP3 换成 FLAC 之后是一条新记录，评分归零。这是曲库不同步的必然后果，接受它：评分是"对这首歌的评价"，但系统只能以文件为身份认人。将来若要跨版本保留，可以用 `work_key` 做一次"新指纹继承旧评分"的一次性迁移，但**不能**把 `work_key` 直接当评分键——那样给专辑里一首歌打分会让整个同名版本组都亮星。

### 2.15 听歌汇总（参与同步）

播放历史是**明细**，听歌汇总是**累计值**。两者分开存，因为用途不同：明细是凭证（"什么时候听的"），汇总是统计页要读的东西（"总共听了多少次"）。

```sql
CREATE TABLE play_stats (
    -- 一台设备一个作品只写一行。两台设备永远不会写同一行，所以同步时永远是新插入、
    -- 不需要任何冲突处理 —— 合并就是"行取并集"
    stats_key  TEXT    NOT NULL,      -- 已识别：work_key（§4.6）；未识别：曲目 fingerprint
    device_id  TEXT    NOT NULL,

    identified INTEGER NOT NULL DEFAULT 1,  -- 1 = 有作品身份；0 = 未识别（当时没有标题标签）

    play_count      INTEGER NOT NULL DEFAULT 0,  -- 按 counted=1 累加的次数
    total_played_ms INTEGER NOT NULL DEFAULT 0,  -- 累计收听时长，含跳过

    last_played_at  INTEGER,          -- 最近一次收听时间
    title_snapshot  TEXT,             -- 显示用，从最近一次记录里取
    artist_snapshot TEXT,

    updated_at INTEGER NOT NULL,
    updated_by TEXT    NOT NULL,
    revision   INTEGER NOT NULL DEFAULT 1,

    CHECK (identified IN (0, 1)),
    PRIMARY KEY (stats_key, device_id)
);
```

**为什么不能"一首歌一行存总计数 + last-write-wins"。** 实测这个坑：PC 听 5 次、手机听 3 次，同步时 LWW 覆盖后得到 **3**，正确答案是 8。这和 §0 讲的"计数器列 + LWW 会丢掉另一台设备的收听"是同一个坑。

`PRIMARY KEY (stats_key, device_id)` 把这个坑绕开：**每台设备只写自己那一行**，两台设备永远不碰同一行。统计页读的时候 `SUM(play_count)`，得到 8。合并 = 行取并集，不需要冲突处理，也不需要三选一。

**这样设计的附带好处：清掉播放历史明细，汇总不缩水。** 实测：清掉 7 条明细后，汇总仍然是 10 次 / 2200 秒——因为汇总在写入时就已经累加了，明细只是凭证。没有这个分离，你一旦实现"播放历史保留 3 个月"，统计页的总次数就会跟着往回缩，用户会以为程序坏了。

**没有标题标签的文件归到「未识别」，不丢弃。** 这类文件（`01.mp3`、`Track 01.mp3` 这种批量抓轨命名）没有可靠的作品身份，但"这首歌我听了 12 次"仍然是真实发生的事，不该从总数里蒸发。

做法是：`stats_key` 退回用该曲目的 `fingerprint`（唯一、稳定、两端算得一致），`identified = 0` 标记"这不是一个作品身份，只是一个文件"。于是：

| 用途 | 算不算未识别 |
| --- | --- |
| 总收听时长、总听歌次数 | **算**，它们是真实发生的时间 |
| 总曲数 | **算**，一首没名没姓的歌也是一首 |
| 「听的次数」列表 | **不算**，榜单上出现 `01`、`Track 01` 三次毫无意义 |

实测依据：500 个无标签文件硬用文件名兜底只产生 80 个不同键，**平均每键 6.2 个文件被错误合并**——所以才不能拿文件名当键。用 `fingerprint` 则不存在碰撞，代价只是"同一首歌的两个编码版本会被算成两首"，这在这个场景下可以接受。

两类键存在同一个 `stats_key` 列里不会互相干扰：`work_key` 形如 `周杰伦\x1f晴天`，`fingerprint` 是 64 位十六进制串，两者的字符集不可能撞上。真要用 `identified` 之外的保险，可以在 `fingerprint` 前加 `fp:` 前缀，但没必要。

**为什么不在曲目识别出来之后再合并进作品键。** 那样做等于让统计回溯修改，破坏"写入时冻结"这条全项目一致的规则（§2.10 的 `counted` 用的是同一条规则）。用户给文件补上标签之后，新产生的收听会进正常作品键，旧的留在未识别类里——数字不会倒退，也不会莫名跳变。

**汇总表读取（三条，全部实测）。** 注意读的是视图 `v_stats` 而不是 `play_stats` 本身——被用户手动删掉的条目必须从统计里消失，而隐藏标记在另一张表上（见下）：

| 条目 | SQL |
| --- | --- |
| 总收听时长 | `SELECT SUM(total_played_ms) FROM v_stats` |
| 总曲数 | `SELECT COUNT(*) FROM v_stats` |
| 听的次数最多 | `SELECT stats_key, play_count AS n FROM v_stats WHERE identified = 1 ORDER BY n DESC LIMIT ?` |

「听的次数最多」做成一项**列表**，按次数从高到低排序，**0 次的不进列表**（视图里的 `HAVING SUM(play_count) > 0`）。**不做「听的时间最长」的条目**——用户明确删掉了它。`total_played_ms` 仍然要存，因为「总收听时长」这个总数需要它，只是不再单独排一个时长榜。

注意前两条**不带** `identified = 1` 过滤：总量要包含未识别文件，否则用户明明听了很多歌，总时长却比实际少。

#### 手动删除汇总条目

统计页的「听的次数最多」列表，每一项可以手动删除（用户要求的功能）。要删的是"这一条汇总不要再出现在我的统计里"，而不是删掉本机那一行——因为**本机只有自己那一行**：

- 物理 `DELETE play_stats WHERE stats_key=? AND device_id='pc'` 之后，对端那 42 次还在；下次同步对端把自己那行推过来，合计仍然是 42+8=50，删除等于没做。实测确认。
- 只删本机行、保留对端行也不行：列表项还在，只是数字变小了，用户会以为"删除"把听歌记录删掉了。

所以要一个**跨设备生效的隐藏标记**：

```sql
CREATE TABLE stats_hidden (
    -- 被用户手动从统计里移除的作品。带同步字段，是两个设备都能看见的实体
    stats_key   TEXT    PRIMARY KEY,     -- 与 play_stats.stats_key 同一套键
    hidden_at   INTEGER NOT NULL,        -- 用户点"删除"的时刻
    restored_at INTEGER,                 -- 之后又重新统计的时刻；NULL = 一直隐藏

    updated_at  INTEGER NOT NULL,
    updated_by  TEXT    NOT NULL,
    revision    INTEGER NOT NULL DEFAULT 1
);
```

可见性判定写进读取视图，`play_stats` 本身的聚合 SQL 不变：

```sql
CREATE VIEW v_stats AS
SELECT s.stats_key,
       s.identified,
       SUM(s.play_count)      AS play_count,
       SUM(s.total_played_ms) AS total_played_ms
FROM play_stats s
LEFT JOIN stats_hidden h ON h.stats_key = s.stats_key
WHERE -- 没有隐藏记录：正常显示
      (h.stats_key IS NULL
       -- 或者用户后来听过了，恢复显示
       OR (h.restored_at IS NOT NULL AND h.restored_at > h.hidden_at))
      -- 恢复之后，只认 restored_at 之后产生的计数，旧的作废
  AND (h.stats_key IS NULL
       OR h.restored_at IS NULL
       OR s.updated_at >= h.restored_at)
GROUP BY s.stats_key, s.identified
HAVING SUM(s.play_count) > 0;
```

**两条约束，缺一条功能就是坏的：**

1. **隐藏行必须先于 `play_stats` 行合并**。同步时先拉 `stats_hidden`、再拉 `play_stats`，否则对端那一行先到、视图在隐藏行到达前先算了一次，界面会闪一下 50 再跳回 0。合并顺序是协议的一部分，不是实现细节。
2. **必须有 `restored_at` 这个边界**。用户删掉 50 次之后，过两天又听了 1 次：如果只是删掉隐藏行，旧计数会跟着复活，显示 51 次；有了 `restored_at`，只算 `updated_at >= restored_at` 的行，显示 **1** 次。实测四步对账：初始 晴天 50 + 江南 5 = 55 → 删除后只剩江南 5 → 对端行推来仍是 5 → 又听 3 次后 晴天 3 + 江南 5 = 8。

**清理策略**：`restored_at` 已过去的隐藏行与 `updated_at < restored_at` 的 `play_stats` 旧行可以删掉，这是确定性动作，两端各自清理后结果一致。`last_played_at` 与快照列跟着 `play_stats` 行走，不需要额外处理。

**删除的是「作品」，不是「这次收听」**。同一 `stats_key` 下的所有设备的行一起被隐藏——因为列表一行只代表一首歌，用户的意思是"这首歌别再上我的榜"，不是"把我电脑上那 42 次删掉"。要删某一次具体的收听，那是播放历史明细页的事，删 `play_history` 里那一行即可，汇总不受影响（它早已累加过）。

**入口是长按 / 右键，且必须二次确认。** 排行榜每一行**不常驻删除按钮**——统计页是给人看的，每行挂个垃圾桶会让整页变成批量操作台，也容易被误触。做法是长按（Android）或右键（Windows）弹出上下文菜单，菜单里才有「从统计中移除」；点它之后还要一个确认对话框，写明"这个操作会把这件作品从所有设备的听歌次数排行里移除，播放历史记录不受影响"。二次确认是必需的：这个操作**跨设备生效**，本机点了之后手机上那条也消失，误触的代价无法靠"撤销"挽回（撤销要做成一次带时间戳的反向写，代价远大于确认框）。

### 2.15.1 播放历史的两种视图：列表 + 日历

播放历史页有两个视图，切换在页面顶部：

**列表**：按 `played_at` 倒序分页，每行显示歌名、艺术家、播放时长、结束方式、来源设备。直接走 `idx_history_time ON play_history(played_at DESC)`，实测一页 50 条 **0.11 ms**。

**日历**：像日历一样按月铺格子，每格一个日期，格子里显示当天的播放次数（可切换显示总时长）。点某天展开当天的记录列表。

```sql
-- 日历：按本地日期聚合。local_day() 是注册进 SQLite 的自定义函数，见下
SELECT local_day(played_at) AS d,
       COUNT(*)             AS plays,
       SUM(played_ms)       AS ms
FROM play_history
WHERE played_at >= ? AND played_at < ?      -- 先按毫秒范围裁剪，命中 idx_history_time
GROUP BY d
ORDER BY d;
```

**⚠️ 时区分桶必须用自定义函数，不能用 `played_at + 固定偏移`。** 上面这段 SQL **依赖应用注册的 `local_day()`**，直接丢进 `sqlite3` 命令行会报 `no such function: local_day`——注册函数是应用启动时做的事，属于实现责任，不是 schema 的一部分。

我本来打算在 SQL 里写 `(played_at/1000 + 28800)/86400`（东八区），实测在有夏令时的时区会错位：

| 本地时间（America/New_York） | 真实日序 | 固定 −5h 分桶 | 结果 |
| --- | --- | --- | --- |
| 2024-03-11 00:30 | 19793 | 19792 | ❌ 算到前一天 |
| 2024-03-12 00:30 | 19794 | 19793 | ❌ 算到前一天 |

夏令时切换后偏移从 −5 变成 −4，任何写死的常量都会在切换日之后把所有凌晨的记录算到前一天。中国没有夏令时，**但用户的设备会在别的时区**，而且历史记录会跟着账号跨时区存在。所以注册一个 `local_day(ms)` 函数，用系统时区库做真实转换：

| 视图 | 实现 | 实测 |
| --- | --- | --- |
| 日历（一个月） | `WHERE` 毫秒范围 + `GROUP BY local_day()` | **0.55 ms** |
| 日历（一整年） | 同上，范围放宽到年 | **9.4 ms** |
| 日历（三年全部） | 无范围裁剪，全表 4.9 万行 | **24 ms** |
| 列表（一页 50 条） | `ORDER BY played_at DESC LIMIT 50` | **0.11 ms** |

多花的那 20 毫秒换来的是"每格一个日期"永远正确，值得。而且日历视图默认只显示一个月，走的是 0.55 ms 那条路径。

**为什么日历不预先分桶存表。** 实测按月单独建一张汇总表（`ps_period`，3 年 41456 行）会多占 **1544 KiB**（gzip 441 KiB），是 `play_stats` 的 37%，而且只有月度粒度——"本周"还得回明细查。现在直接查明细，一个月 0.55 ms 就够了。

**时间维度依赖历史保留策略，这是有意的。** 用户把播放历史保留设成 1 年，日历就只能看到一年内；设成永久就都能看。这本来就是"我不想留这么久"的含义，不需要额外的降采样表来假装还记得。

**碰撞不处理。** `work_key` 用「艺术家 + 标题」做键，理论上 `Intro`、`Interlude`、`无题` 这类会撞。实测 5000 首真实感曲库里，4500 个作品键中 **4372 个只含 1 首（98.7%）**，最大的键也只有 3 个文件。碰撞的后果是两首不同的歌被合并统计——这对听歌汇总的影响可以接受（用户不会真的去数「Intro」听了几次），处理它需要额外的启发式规则和 UI 解释，成本远大于收益。

### 2.16 编辑曲目标签

设置里提供一个「编辑标签」入口：读出文件内的标签显示，改完写回文件，然后重新读取该曲目信息。

**改的是文件，不是数据库。** 这是本项目的常态——库是本机缓存，文件才是真源。所以写的顺序是：**写文件 → 更新库**。不能反过来先改库，否则文件与库不一致，下次扫描库里的信息会被文件覆盖回去，用户看到"改了又变回来"。

**这里不需要重算指纹。** 指纹是对**音频区间**算的（§4），写标签改的是标签区间，指纹值不变。需要跟着更新的只是 `tracks.file_size` / `tracks.file_mtime` 和 `track_files.fp_size` / `fp_mtime` / `fp_verified_at` 这几列——它们不是指纹本身，而是"下次扫描要不要再读一遍盘算指纹"的判据。实测对比（改完标签后连做三次增量扫描）：

| 写法 | 三次扫描是否重算指纹 | 白读磁盘 |
| --- | --- | --- |
| 库完全不更新 | 是 / 否 / 否 | 0.5 ms |
| 只更新 `file_size` / `file_mtime` | 是 / 否 / 否 | 0.5 ms |
| 连 `fp_size` / `fp_mtime` / `fp_verified_at` 一起更新 | 否 / 否 / 否 | 0 |

绝对差只有 0.5 毫秒，但既然文件已经在手里、新的 size/mtime 也是刚 `stat` 出来的，没有理由留个"下次再补"的尾巴。

**一次编辑只改一个文件。** 同一首歌在库里可能对应多个 `track_files` 行（§2.5 的多份拷贝），但标签是**落实在文件本身**的，所以编辑只针对用户点开的那个文件。另一份拷贝的标签保持原样，改完之后它们的元数据就不同了——这是用户的显式选择，不替他"顺便全改"。

**指纹不会因为改标签而变，这是整个功能的安全基石。** 指纹是剥掉 ID3v2 / APEv2 / ID3v1 之后对纯音频区间算的（§4）。实测同一段音频在 6 种标签形态下指纹完全一致：ID3v2 标签体 200 → 3000 → 40000 字节，音频区间从 `0..600000` 挪到 `40010..640010`，指纹始终是 `9b0a4c3745406782`；尾部加 APEv2、再加 ID3v1 也一样。

由此可以推出三件事，全部是用户能直接感到的好处：

| 因为指纹不变… | 结果 |
| --- | --- |
| 重扫不会新建曲目行 | 编辑后歌还是那首歌，不会在列表里多出一条"重复的" |
| `tracks.id` 不变 | `track_files` / `track_artists` / `playlist_items.track_id` 三处外键全部继续有效 |
| 内容身份不变 | `track_ratings`（键 `fingerprint`）和 `playlist_items.ref_fingerprint` 全部继续有效 |

**唯一会变的是 `work_key`。** 它是「艺术家 + 标题」的归一化结果（§4.6），改了标题它必然变——比如 `周杰伦\x1f晴天` 改成 `周杰伦\x1f晴天修正版`。而 `play_stats` 里那 42 次挂在**旧键**上（写入时冻结，见 §2.15）。

**这个残留在当前设计里不处理。** 已确认的取舍：改完标题之后，统计页用新标题搜不到历史，旧的那 42 次还在，只是挂在一个搜索不到的名字下；新产生的收听进新键。

写回标签的两个实现约束：一是写之前重新读一次原文件标签（不能拿库里可能过期的缓存去覆盖），二是写失败（文件只读、被占用、磁盘满）必须报错并保留原文件不动——宁可不改，也不能写坏文件。

---

## 3. 名称归一化（`name_norm` / `title_norm` / `pinyin_key`）

### 3.1 匹配键

入库时对艺术家名和专辑名做统一处理，生成匹配键：

1. Unicode NFC 规范化（Windows 与 Android 都是 NFC，但导入的文件可能带 NFD）
2. 小写化
3. 去掉变音符号（`Beyoncé` → `beyonce`）
4. 折叠连续空白为单个空格，去首尾空白
5. 去掉开头的 `the ` 前缀（`The Beatles` → `beatles`）

**这是给"判断是不是同一个人"用的，不是给显示用的。** 显示名在 `name` 列里原样保留，所以"是什么就填什么"这件事本来就已经成立——归一化只是并排的第二列。

**不归一化会怎样**（同一支乐队，标签被两个来源写成 `The Beatles` / `the beatles`）：

| | 艺术家行数 | 同名专辑行数 | 后果 |
| --- | --- | --- | --- |
| 归一化 | **1** | **1** | 两张专辑合成一张 |
| 不归一化 | **2** | **2** | 列表上两个 "The Beatles"，各挂一半专辑；再传染到 `primary_artist_id` 与 `work_key`，同一首歌的统计被拆成两行 |

各步的边际收益（30 个真实艺术家、20000 首、含现实变体）：

| 步骤 | 剩余艺术家数 | 这一步单独消灭的重复 |
| --- | --- | --- |
| 原样（不归一化） | 81 | — |
| NFC + 小写 + 折叠空白 | 49 | **32** |
| 再加去变音符号 | 46 | 3 |
| 再删开头 `The` | **39** | 7 |

去变音是收益最小的一步（只值 3 行），但零成本（入库算一次），仍然保留：英文库必然同时存在 `Beyoncé` 和 `Beyonce` 两种写法，不合并用户在列表里就会看到两个。

**本方案有意不合并的**（需要别名表或繁简转换表，另开设计）：`Simon & Garfunkel` vs `Simon and Garfunkel`、`AC/DC` vs `ACDC`、简体 `周杰伦` vs 繁体 `周杰倫`、`五月天` vs `MAYDAY`。

### 3.2 排序键：`pinyin_key`（替代已删除的 `sort_name`）

**已删除 `artists.sort_name`。** 它原本存 "Beatles, The" 这类"去掉 The 放到末尾"的排序串。删掉的理由不是"冗余"，是**它排得比 `name_norm` 差**：

纯 ASCII 名字下两种排序结果**完全相同**（`name_norm` 本来就是"去 The 后小写"，位置一致）。但带变音符号时 6 组测试 **6 组顺序都不同**：

| 输入 | 按 `sort_name` | 按 `name_norm` |
| --- | --- | --- |
| Björk / Bjorn Borg | Bjorn Borg **>** Björk | Björk **>** Bjorn Borg |
| Mötley Crüe / Motown | Motown **>** Mötley Crüe | Mötley Crüe **>** Motown |
| Beyoncé / Beyonce Knowles | Beyonce Knowles **>** Beyoncé | Beyoncé **>** Beyonce Knowles |

根因：`sort_name` 保留 `ö` = U+00F6，SQLite 默认按码位比较会把它排到 `z` = U+007A 之后，`Björk` 掉到 B 区末尾；`name_norm` 去掉了变音符号，`bjork` 落在 `o` 的位置，符合人的预期。**删掉 `sort_name` 之后，英文排序直接用 `ORDER BY name_norm`，不需要第二套规则。**

**但 `ORDER BY name_norm` 对中文是码位序，不是拼音序。** 实测同一批艺术家：

```text
库里的顺序（码位序）：五月天 周杰伦 张学友 李荣浩 林俊杰 王菲 薛之谦 邓紫棋 陈奕迅
用户期望（拼音序）   ：陈奕迅 邓紫棋 林俊杰 李荣浩 王菲 五月天 薛之谦 张学友 周杰伦
```

**方案：自带一张小型汉字→拼音表，扫描时算出一列 `pinyin_key`，排序走这列。**

```text
artists.pinyin_key = "zhou\x00jie\x00lun"     -- 逐字拼音，\x00 分隔
ORDER BY pinyin_key                            -- 走覆盖索引
```

为什么是这一列而不是"注册一个自定义 collation"：

| | 存 `pinyin_key` 列 | 自定义 collation |
| --- | --- | --- |
| 排序耗时（2 万行取前 50） | **0.01 ms**（覆盖索引） | 3.61 ms（临时 B 树全量排序） |
| 能否建索引 | ✅ 普通索引 | 能，但**索引在未注册该 collation 的连接上会直接报错**（备份换设备打开就崩） |
| 跨设备一致 | ✅ 纯文本规则，两端算得一样 | ❌ Windows 用 `CompareStringEx`、Android 用 ICU `Collator`，同一批名字可能排出不同顺序 |

成本：

| 项 | 数值 |
| --- | --- |
| 字典体积 | 6763 个常用汉字，紧凑表 **16 KiB**（2 字节音节编号 + 音节字符串池），朴素文本表 56 KiB |
| 存储 | 只放 `artists` / `albums`，不放 `tracks`。5000 个艺术家 **98 KiB**；2 万个 391 KiB |
| 生成耗时 | 2 万个名字 **7.3 ms**（Python）；C++ 快 20–50 倍，可忽略 |

**必须逐字比较，不能把整个名字的拼音拼成一串再比。** 实测 `李荣浩`(`lironghao`) vs `林俊杰`(`linjunjie`)：整串拼接得出 `lin < lir` → 林俊杰排前面（错）；逐字比较 `li` vs `lin`，`li` 是前缀 → 李荣浩在前（对）。用 `\x00` 分隔拼接等价于逐字比较。

**多音字按第一读音固定。** 不根据上下文猜「重庆」还是「重复」——那要另一套工作量的模型，而且会"有时对有时错"，比稳定地排在相邻位置更让人困惑。超出字典的汉字按原字符码位兜底。

**什么时候做：v0.3 扫描时填充。** 列现在就加进表结构（可 NULL），但 v0.3 之前不填值、不排序。理由：拼音只影响 `artists` / `albums` 的列表顺序，不影响搜索（搜歌走 `title_norm LIKE`），所以这不是阻塞项；但列现在就留好，避免将来 ALTER TABLE 并触发重建。


---

## 4. 跨设备曲目身份：`fingerprint`

这是整个同步设计里最麻烦的一块。

**问题：** Windows 上「晴天」的 `track_id` 是 42，Android 上是 917。播放列表引用哪个？文件路径？两端路径不可能一样。标签？同一首歌在专辑版和精选集版里标签可能不同，还会撞名。

**方案：内容指纹，但先剥标签。**

```text
fingerprint = SHA-256( audio_len_bytes
                     ‖ first 256 KiB of audio payload
                     ‖ last  256 KiB of audio payload )

其中 audio payload = 文件去掉 ID3v2 / APEv2 / ID3v1 / Lyrics3 之后的纯音频数据
```

**为什么必须先剥标签。** 最初的设计是不剥标签、直接对头尾 256 KiB 取哈希。实测发现这条路不成立：给一个文件写入 217 字节的 ID3v2 标签，头尾哈希立刻改变，**同一首歌变成了两首歌**，播放次数和评分随之丢失。在尾部追加 128 字节的 ID3v1 也一样。而用工具改标签、自动整理软件补封面、`foobar2000` 回写 ReplayGain，这些都是每天都在发生的事。

剥标签之后，同一段音频在下列五种形态下指纹完全一致（实测）：

| 文件形态 | 音频区间 | 指纹 |
| --- | --- | --- |
| 裸音频 | 0..614400 | `120820363b058fda` |
| ID3v2 体 200 字节 | 210..614610 | `120820363b058fda` |
| ID3v2 体 3000 字节 | 3010..617410 | `120820363b058fda` |
| + APEv2 尾（100 字节） | 210..614610 | `120820363b058fda` |
| + APEv2 + ID3v1 | 210..614610 | `120820363b058fda` |

剥标签的代价是**零**：仍然只读 512 KiB，只是把起点从 0 挪到 `10 + syncsafe(tag_size)`，把终点从文件末尾挪到标签之前。

**头部标签的剥法：** 读前 10 字节，`ID3` 魔数命中则标签长度 = 10 + syncsafe 长度（syncsafe：每字节只用低 7 位），若 flags 的第 4 位（`0x10`）置位则再加 10 字节 footer。

**尾部标签的剥法（循环最多 8 次，因为可能叠加）：** `TAG` 结尾 → 减 128 字节；`APETAGEX` 结尾 → 从 footer 的 size 字段减掉；两者可交替出现。

选这个方案的理由：

- **与路径无关**：文件从 `D:\Music\` 拷到 `/sdcard/Music/`，指纹不变。
- **与时间无关**：不用 mtime（复制会改 mtime，但内容没变）。
- **与标签无关**：重新打标签、补封面、回写响度信息，指纹都不变。
- **快**：不用读完整个文件。实测 40 MB 文件全量 SHA-256 要 25.9 ms，头尾各 256 KiB 只要 0.5 ms，**快 49 倍**。1000 首从 25.9 秒降到 0.5 秒。
- **碰撞概率可忽略**：文件大小 + 首尾各 256 KB 已足够区分同专辑内不同曲目。

**局限（必须接受）：**

1. **同一首歌的 FLAC 版和 MP3 版指纹不同**，会被当成两首歌。这是有意为之——它们确实是两个文件。跨编码识别同一首歌需要声学指纹（AcoustID/Chromaprint），那是另一个量级的工作，列入将来。
2. **文件中间音频数据的改动检测不到。** 头尾法本来就有这个盲区，剥标签只是让它显式化。对音乐播放器无影响：没有人会去改一首歌中间的一段并期望它被识别成另一首歌。
3. **如果标签长度被改动但音频相同，指纹不变**——这正是我们要的。

**解析链：** 对端拿着 `ref_fingerprint` 来找，命中就绑定 `track_id`；没命中退回用 `(ref_title, ref_artist, ref_album, ref_duration_ms)` 做模糊匹配（时长容差 ±2 秒）；都不中，标记为「缺失」保留条目。

### 4.6 `work_key`：统计聚合键

指纹解决"这个文件是哪首歌"，但统计要回答的是另一个问题：**换了个版本之后，还应该算同一首歌吗？**

```text
fingerprint  一首歌 = 一个文件身份。换版本 = 换指纹 = 新曲目。
             用途：文件去重、播放列表定位、评分、播放历史回指曲目

work_key     统计聚合键 = 归一化(艺术家) + 归一化(标题)
             换版本后 work_key 不变 → 统计接上
             用途：播放次数、总收听时长
```

**为什么不用文件名做这个键。** 最初考虑的是「文件名（不含扩展名）」，实测行不通。17 个真实感文件只有 8 个不同的 basename，**5 个发生冲突**：

| basename | 被几个文件共享 |
| --- | --- |
| `01` | 4 个（专辑 A/B/C/D 各一首） |
| `晴天` | 4 个（mp3、flac、Live 版、伴奏版） |
| `安静` | 2 个（原版 + 翻唱） |
| `童话` | 2 个（原唱 + 儿歌） |
| `月光` | 2 个（贝多芬 + 古典合集） |

而且它想救的场景也只救得了六分之一：

| 旧文件 | 新文件 | basename 相同？ |
| --- | --- | --- |
| `晴天.mp3` | `晴天.flac` | ✅ |
| `晴天.mp3` | `晴天 (FLAC 无损).flac` | ❌ 用户自己加了后缀 |
| `晴天.mp3` | `01. 晴天.flac` | ❌ 加了音轨号 |
| `晴天.mp3` | `周杰伦 - 晴天.flac` | ❌ 加了艺术家前缀 |
| `Track01.mp3` | `01 - 晴天.flac` | ❌ 编号 vs 歌名 |

**归一化规则：**

```text
work_key = norm(artist_name) + "\x1f" + norm(title)

artist_name = tracks.primary_artist_id 指向的 artists.name（主艺术家）
title       = tracks.title

norm(s):
  1. NFKC 规范化（全角转半角）
  2. casefold()（比 lower() 更彻底，能处理 ß→ss 这类）
  3. 去掉空白字符
  4. 去掉标点 [\s\-_·・.,，。、!！?？'"“”‘’()（）\[\]【】]
```

用 `\x1f`（单元分隔符，ASCII 不可打印）而不是空格分隔，避免"艺术家末尾空格 + 标题开头空格"和"艺术家 + 标题"撞在一起。

**艺术家必须取「主艺术家」，不能取原始标签串。** 取原始串的话同一首歌换个写法就断开了——实测 `A feat. B` 与 `A ft. B` 归一化后得到 `afeatb` 和 `aftb`，判成两首。取 `primary_artist_id`（`track_artists` 里 `role='main'` 且 `position` 最小那一位）才稳定：它由标签解析得出，两端解析同一份标签得到同一个主艺术家。

**实测能力边界：**

| 场景 | 合并？ |
| --- | --- |
| MP3 → FLAC（标签不变） | ✅ |
| 标题尾部多余空格 | ✅ |
| 全角括号 vs 半角括号 | ✅ |
| `Qing Tian` / `qing tian` | ✅ |
| 繁简（`周杰伦` vs `周杰倫`） | ❌ |
| `晴天` vs `晴天 (Live)` | ❌（有意分开） |
| `晴天` vs 别人的翻唱 | ❌ |

**为什么不带专辑。** 实测加上专辑确实能把碰撞从 402 个降到 0，但会把「换了来源、专辑不同」这个**最常见的换版本场景**断开——同一首歌从精选集换成原版专辑，专辑名变了，统计就断了。为消除一个可接受的碰撞，牺牲一个高频的正常场景，不划算。

**为什么不剥括号。** `(Live)`、`(Acoustic)`、`(Remastered 2011)` 和标题挤在同一对括号里，剥括号会把它们一起剥掉。现场版是另一段真实的演出，分开统计更诚实。

**没有标题标签的文件归到「未识别」，`stats_key` 退回用 `fingerprint`，`identified = 0`。** 实测硬用文件名兜底：500 个 `01.mp3` / `Track01.mp3` 类命名只产生 80 个不同键，**平均每键 6.2 个文件被错误合并**，所以文件名不能当键。但"这首歌我听了 12 次"是真实发生的事，不该蒸发——所以归到未识别类，计入总量、不进排行榜（细节见 §2.15）。

**碰撞不处理。** 5000 首真实感模型里 4500 个作品键中 **4372 个只含 1 首（98.7%）**，最大的键只有 3 个文件。碰撞会让两首歌合并统计，对听歌汇总的影响可接受；处理它需要额外启发式规则，成本大于收益。

---

## 5. 为什么时长存毫秒

`duration_ms` 是 `INTEGER` 毫秒，不是秒。三个理由：

1. **它是 seek 的乘数。** 拖进度条时 `目标 = 比例 × 时长`。截断到秒的话，每次 seek 有最多 1 秒的系统性偏移。
2. **它是曲库总时长的被加数。** 截断误差不抵消，是随机游走：1000 首的累计偏差约 `√1000 × 0.29 ≈ 9 秒`。
3. **成本可以忽略。** SQLite 的 INTEGER 变长编码，`221400` 占 4 字节，`221` 占 3 字节。十万首差 100 KB。

原则：**入库存源数据能给的精度，只在显示层取整一次**，而不是每个用到它的地方各取整一次。

---

## 6. 智能播放列表：用 JSON 语法树，不用 SQL 也不写文本 DSL

你提到对这块不熟悉。先说结论：**智能播放列表的条件存成 JSON 结构，不存 SQL 字符串，也不发明一门文本查询语言。** 下面是三条方案的取舍，然后是可用的具体结构。

> **实现顺序：这一整节推后到 v0.5 之后的版本再做。** 你说过"一开始并没有做智能列表的期望"，所以它不进 v0.1–v0.4 的路径。设计留着的原因是：表结构里已经有 `playlists.kind` 和 `smart_query` 两列（现在删掉将来还要改表），而且它是数据模型的测压点——`play_stats` 的跨设备聚合、`track_ratings` 的 JOIN、`work_key` 的统计口径、`is_missing` 的反向映射，全都是为了让智能列表查得对才被逼着设计对的。设计已经收敛，写代码时照抄即可。

**这件事比听起来简单：白名单编译器本身原型只有 130 行。** 真正的成本在编辑条件的那套 UI（下拉联动、算子随字段类型变、条件行增删、嵌套分组拖拽），那是界面工作量，不是算法难度。

### 6.1 为什么不用 SQL 片段

最省事的做法是 `smart_query = "duration_ms > 180000 AND genre = 'Rock'"`，执行时直接拼进 `WHERE`。这条路有三个不能接受的后果：

1. **注入面完全敞开。** 字符串里出现 `'; DROP TABLE tracks; --` 就是灾难。你可以做转义，但转义是"每个调用点都要记得做对"的事，而漏一个就等于零。
2. **导入别人的播放列表就是远程代码执行。** 你的设计里有「导入备份」，用户可能从论坛下载一个播放列表文件。如果那是 SQL 字符串，它就能读你整个磁盘的数据库。
3. **绑死在 SQLite 上。** 将来若真要换存储（比如同步时改用服务端查询），SQL 片段要重写。

### 6.2 为什么也不发明文本 DSL

`artist:"周杰伦" AND year:>2000` 这种东西看起来优雅，实际成本是：写词法分析器、写语法分析器、给每个错误设计用户能看懂的中文提示、处理引号转义、处理字段名同义词。而这套语法最终服务于**下拉菜单和输入框**——用户是从列表里选"艺术家 等于 周杰伦"，不是手打一行查询。为一个没人手打的语法写编译器，是纯浪费。

### 6.3 采用方案：JSON 语法树 + 白名单编译

`playlists.smart_query` 存一段 JSON：

```json
{
  "all": [
    { "field": "genre",       "op": "is",      "value": "Rock" },
    { "field": "duration_ms", "op": "greater", "value": 180000 },
    { "any": [
        { "field": "play_count",   "op": "less", "value": 3 },
        { "field": "last_played",  "op": "before_days", "value": 90 }
    ]}
  ]
}
```

三个逻辑节点：`{"all": [...]}`、`{"any": [...]}`、`{"not": {...}}`，可任意嵌套。一个叶子节点是 `{field, op, value}`。

**安全性来自编译方式，不来自输入校验。** 编译过程是这样的：

```text
1. field  → 查白名单表，得到固定的 SQL 列名。查不到 → 拒绝。
             （用户提供的字符串永远不会出现在 SQL 文本里）
2. op     → 查该字段类型允许的算子表，得到固定的 SQL 片段。
             （"is"/"contains"/"between"/"before_days"/"within_days" 各自映射到硬编码的片段）
3. value  → 永远作为绑定参数 ? 传入，从不拼接。
4. 嵌套深度超过 8 层 → 拒绝（防构造病态查询拖垮 UI）。
5. 叶子节点总数超过 64 → 拒绝。
```

结果是：**编译出来的 SQL 语句结构完全由白名单决定，用户数据只以 `?` 占位符的形式进入。** 即使 JSON 被恶意构造，最坏结果是"查询条件不成立"，不可能是"执行了别的语句"。

### 6.4 字段白名单

| 字段 | 类型 | 允许的算子 | 说明 |
| --- | --- | --- | --- |
| `title` | 文本 | `is` `contains` `starts` | 走归一化列 |
| `artist` | 文本 | `is` `contains` | |
| `album` | 文本 | `is` `contains` | |
| `genre` | 文本 | `is` `contains` | |
| `year` | 整数 | `is` `greater` `less` `between` | |
| `duration_ms` | 整数 | `greater` `less` `between` | UI 上用分钟输入，存储用毫秒 |
| `bitrate` | 整数 | `greater` `less` `between` | |
| `sample_rate` | 整数 | `is` `greater` `less` | |
| `codec` | 文本 | `is` | `flac`/`mp3`/… |
| `channels` | 整数 | `is` | |
| `added_at` | 时间 | `within_days` `before_days` | 距今天数（UI 上填天数，不填毫秒戳） |
| `last_played` | 时间 | `within_days` `before_days` `never` | 关联 `play_history`；`never` = 从没听过 |
| `play_count` | 整数 | `is` `greater` `less` `between` | 关联 `play_history` |
| `total_played_ms` | 整数 | `greater` `less` `between` | 总收听时长 |
| `rating` | 整数 | `is` `greater` `less` | 星级评分，0–5，0 = 未评分；见 §2.14 |
| `is_missing` | 布尔 | `is` | `true` = 库里但本机无文件 |

**`last_played` / `play_count` / `total_played_ms` 是派生字段**，不在 `tracks` 表里，编译时 LEFT JOIN 到 `play_history` 的聚合子查询。因为 `play_history` 参与同步（§2.10），默认就是两端合并后的数字，这符合"统计要算全"的预期。

#### 为什么 `within_days` 和 `before_days` 必须是两个算子

它们底层是同一个原语（都拿 `now - N 天` 做比较），但**不能用 `NOT before_days` 代替 `within_days`**，因为 `NULL` 的三值逻辑。实测（数据：A 在 1000 听过、B 在 5000 听过、C 从没听过即 `last_played IS NULL`；现在 6000，「最近 2 天内听过」正确答案只有 B）：

| 编译形式 | 结果 |
| --- | --- |
| `before_days` → `col IS NOT NULL AND col < 4000` | `['A']` ✓（2 天**之前**听过＝很久没听） |
| `NOT (上面那条)` | `['B', 'C']` ✗ **从没听过的 C 混进了"最近听过"** |
| `within_days` → `col IS NOT NULL AND col >= 4000` | `['B']` ✓ |

三条理由：「从没听过」和「最近听过」语义相反；`NULL` 参与比较得 `NULL`，`NOT NULL` 仍是 `NULL`；若为了让 `NOT` 可用而去掉 `IS NOT NULL` 守卫，那 `before_days` 自己就会把"从没听过"算成"很久以前听过"。

这两类需求恰好都是智能列表最常用的：「最近 7 天我听的新歌」（`within_days`）和「90 天没听了，帮我重新发现」（`before_days`）。

**删掉了 `after_days` / `before` / `after` 三种算子。** `after_days` 与 `before_days` 方向重复（"N 天之后"没有实际查询意义），`before` / `after` 要求用户填绝对时间戳，界面上没法输入。`added_at` 现在配 `within_days` / `before_days`，于是「最近 30 天加入的歌」终于能表达了。

**`rating` 编译成 LEFT JOIN `track_ratings`**（§2.14），连接键是 `fingerprint` 而不是 `work_key`——评分是"对这个文件的评价"，换版本就是另一首歌。

### 6.5 存储与同步

`smart_query` 是一个 `TEXT` 列，里面是 JSON 文本。它属于 `playlists` 实体，随播放列表一起同步（§1 规则一：整实体替换）。

**关键点：同步的是查询定义，不是查询结果。** Windows 上同步来一个"90 天没听过的摇滚"，Android 拿到的是一段 JSON，它用自己的曲库去执行，得到自己那份结果。这是正确的——两端的曲库本来就不同，同步结果集毫无意义。

**版本兼容：** JSON 里带 `"version": 1`。将来扩展算子或字段时，旧客户端遇到不认识的字段就**把整个智能播放列表标记为"需要更新版本"并显示为不可用**，而不是忽略那一条条件继续执行——静默忽略条件会让列表内容和名字不符（"90 天没听过"的列表里出现了昨天刚听的歌），这比不可用更糟。

### 6.6 一个具体例子：完整编译过程

输入（"被我冷落的好歌"）：

```json
{ "all": [
    { "field": "play_count", "op": "less", "value": 3 },
    { "field": "rating",     "op": "greater", "value": 3 },
    { "field": "is_missing", "op": "is", "value": false }
]}
```

编译输出：

```sql
SELECT t.id
FROM tracks t
LEFT JOIN (
    SELECT track_fingerprint, COUNT(*) AS play_count
    FROM play_history
    WHERE deleted_at IS NULL AND counted = 1   -- 写入时已按阈值判定，见 §2.10
    GROUP BY track_fingerprint
) h ON h.track_fingerprint = t.fingerprint
LEFT JOIN track_ratings r ON r.fingerprint = t.fingerprint AND r.deleted_at IS NULL
WHERE 1 = 1
  AND (   IFNULL(h.play_count, 0) < ?          -- 3
      AND IFNULL(r.rating, 0) > ?              -- 3
      AND t.has_local_file = ?                 -- 1（is_missing=false 反向映射）
      )
```

注意 `is_missing = false` 编译出来是 `t.has_local_file = 1`，参数也要跟着取反——这类映射必须集中在一张表里、有单元测试覆盖，散落在各处迟早出错。

另外 `rating` 是 `NOT NULL`（0 = 未评分），所以 `IFNULL(r.rating, 0)` 处理的是"这首歌根本没评分记录"的情况；`tracks` 表里已经没有 `rating` 列了（§2.14）。

同一份 JSON 在 UI 上的另一种呈现（这就是为什么用结构化存储）：

```text
播放次数 少于 3
并且 评分 高于 3
并且 文件不缺失
        [并且] [或]  ← 用户点按钮加新条件，直接往 JSON 里 push 一个节点
```

UI 增删条件只是数组操作，不需要解析或重新格式化任何文本。

### 6.7 结果不设上限：全部显示，靠分页控制开销

**查询不加 `LIMIT`。** 条件命中多少，就是多少。之前担心"一个宽条件可能命中上万首"，实测下来这个担心用错了地方：真正要控制的不是结果条数，而是**一次读进内存的字段数**。

实测（20 万首曲库）：

| 场景 | 命中行数 | 查询耗时 | 结果内存 |
| --- | --- | --- | --- |
| 全命中（`year > 1900`），取全部显示字段 | 200000 | 176 ms | 18.3 MiB |
| 命中 40%（`year > 1990`），同字段 | 105552 | 108 ms | 9.7 MiB |
| 只取 `t.id` | 200000 | 45 ms | 3.1 MiB |

5 万首的库更轻：全命中 2.4 MiB、16 ms。

**列表控件本身不需要一次拿到 20 万行。** Qt 的 `ListView` / `TableView` 只渲染可见区域，但它需要数据模型能按索引取行——所以正确的做法是**游标分页**，不是"先全部读进来再交给界面"：

```text
SELECT t.id, t.title, a.name, t.duration_ms, t.album_id
FROM ... WHERE <编译出的条件>
ORDER BY <排序键>, t.id ASC
LIMIT 100 OFFSET ?          -- 实测跳过 2 万行再取 100 行：4.7 ms
```

列表只需要 id / 标题 / 艺术家 / 时长四五个字段；`year`、`bitrate`、`codec`、封面路径这些等用户点进某一首、或滚动到该行需要渲染次要信息时再按 id 单独查。这样即便 20 万首全命中，常驻内存也只有一个窗口的内存。

**排序键必须有确定性兜底。** 智能列表的排序通常是"按添加时间""按标题"，这两者都可能重复；分页时若顺序不稳定，会出现同一首歌在两页里都出现、或哪一页都没有。所以任何排序后面都要追加 `, t.id ASC`。

---

## 7. 同步协议草案

分两阶段，先做简单的。

### 阶段一：快照备份（v0.3）

用户点「导出」，得到一个文件（SQLite 数据库或 JSON），包含所有 `shared` 范围的表，含墓碑。用户点「导入」，按照规则同步。

这个阶段**不需要 `revision` / `updated_at`**，纯快照。但它已经把表结构用对了，所以阶段二不用改表。

**导出内容就是 §0.1 右边那一列。** 曲库不在里面——所以导出的备份文件很小（几百 KB 量级），可以随手放进网盘。恢复备份到一个空白的曲库上：播放列表条目会全部变成「缺失」灰条保留，等你把音乐文件拷过去、扫描一次，它们凭 `ref_fingerprint` 自动复活。

### 阶段二：WebDAV 双向合并（v0.6）

```text
musicplayer3/
├── snapshots/
│   ├── {device_id}-{timestamp}.db     ← 每台设备上传自己的
│   └── ...
└── manifest.json                       ← 各设备最后同步时间
```

合并算法，对每一行：

```text
对 (表, 稳定键) 这个键：
   本地没有、远端有        → 插入远端行
   远端没有、本地有        → 保留（可能是对方还没同步过）
   两边都有：
      按 (updated_at, updated_by) 取大的，整行替换
      deleted_at 只是这一行里的一个普通列，不参与特殊判断
```

不同表的稳定键不同：

| 表 | 稳定键 |
| --- | --- |
| `playlists` | `uid` |
| `playlist_items` | `uid` |
| `settings` | `key` |
| `play_history` | `uid` |
| `track_ratings` | `fingerprint` |
| `play_stats` | `(stats_key, device_id)` |
| `stats_hidden` | `stats_key` |

**`play_stats` 是唯一不需要合并规则的表。** 两台设备永远不会写同一行（主键里有 `device_id`），所以同步就是"把对端的行搬过来"，没有冲突可言。

**`stats_hidden` 要先于 `play_stats` 合并。** 它是"这条汇总被用户删掉了"的跨设备标记（§2.15）。如果对端的 `play_stats` 行先到、隐藏标记后到，统计页会先闪出旧的次数再归零。合并顺序写进协议，不是实现细节。

**注意最后一句：不要把 `deleted_at` 写成单独的判断分支。** 我最初的草案里写了两条特殊规则（"远端 `deleted_at` 非空且更晚 → 本地也软删除""本地 `deleted_at` 非空且更晚 → 保留墓碑"），原型测出一个真实的 bug，见下节。

`updated_by` 参与比较的原因：两台设备时钟可能差几秒，同一毫秒修改时用设备 ID 做确定性裁决，避免"每次同步结果都不一样"。

**已知局限（说清楚，不藏着）：** 这是 last-write-wins，靠系统时钟。手机时钟被人为调快 5 分钟，它在冲突里就永远赢，可能覆盖掉 PC 上更晚的真实修改。

### 为什么不用 HLC（混合逻辑时钟）

HLC 是解决 last-write-wins 时钟问题的主流方案，评估过，**决定不用**。理由不是"它不好"，而是它的收益覆盖面比看上去窄，代价却不小。

HLC 把时间戳从 `t` 变成 `(l, c)`：`l` 是物理毫秒，`c` 是逻辑计数。本地事件时若物理时钟没前进就 `c += 1`；接收对端时间戳时把 `l` 抬到 `max(本地, 对端, 物理)`。这样整点 `(l, c)` 按字典序严格单调递增。

**它治得了的（实测）：**

| 场景 | 纯系统时钟 | HLC |
| --- | --- | --- |
| 单机时钟往回跳（NTP 校正 / 手动调时间） | 第一次改 `1000`、第二次改 `400`（真实更晚）→ **旧值覆盖新值 ✗** | `(1000,0)` → `(1000,1)` → **正确 ✓** |
| 两机有因果关系、手机慢 30 分钟：同步→PC 改名→同步→手机改名 | PC `600000`、手机 `0` → **PC 赢，手机改动被覆盖 ✗** | PC `(600000,0)`、手机 `(600000,5)` → **手机赢 ✓** |

**它治不了的（实测）：**

| 场景 | 结果 |
| --- | --- |
| 两机离线各改各的，互相没见过对方的写（无因果关系） | 手机时钟慢 5 分钟 → **PC 赢，方向错**。HLC 在这里退化成比物理时间 |
| 增量水位漏行 | HLC 的物理部分仍跟随写者的钟，慢钟设备离线写的行照样偏小，**照样漏** |

HLC 的代价：

1. 时间戳从 8 字节变 16 字节（或牺牲物理精度）。`updated_at` 在这份 schema 里出现 8 处，10 万行播放历史多 800 KB。
2. **失去人类可读性**：不能直接拿 `updated_at` 显示"3 分钟前"，要拆成两列或反解。
3. 合并比较从一列变三元组 `(l, c, device)`，索引重建。

**结论：HLC 那两件它真擅长的事（单机单调、因果链次序），在这个应用里都不构成实际损失——单机时钟回跳极罕见，且后果是"一条列表顺序看起来怪"，不是数据丢失；而有因果关系的两台设备，只要遵循下面的水位规则，晚写的那次本来就会赢。为了这两点去付上面三项代价，不划算。**

**因此也不引入"本地单调钳制"**（`updated_at = max(系统时间, 见过的最大值 + 1)`）：它需要维护一个"本机见过的最大时间戳"状态并写进每一行的写入路径，同样是为了一个极罕见的场景加状态。**接受这个已知局限，记录在案，不实现。**

### 增量水位的定义

`sync_state.last_cursor` **必须定义为「从对端见过的最大的 `updated_at`」，不是「本机上次同步的时钟时刻」。** 这个区别很大，实测：

手机时钟慢 1 天，离线写了 3 条播放记录，然后与 PC 同步：

| 水位怎么定义 | 拉到几行 |
| --- | --- |
| 本机上次同步的时钟时刻 | **0 / 3 ✗**（3 行的 `updated_at` 全在 PC 水位之下） |
| 从对端见过的最大的 `updated_at` | **3 / 3 ✓** |

原因：水位来自对端写过的值，慢钟设备写入的下一行总会比它自己的上一行大，所以能被持续拉到。这个规则**不需要额外状态，也不需要时钟同步**，只是把 `last_cursor` 的赋值来源从"本机 `now()`"改成"本次拉到的最大 `updated_at`"。

**这个规则唯一的漏洞**：同一台设备的时钟往回跳（手机本来准，某天被校正慢 1 天），此后新写的行全部低于 PC 水位 → 漏行。这是上面"不用 HLC / 不用单调钳制"所接受的代价。**补救手段不需要代码：设置页提供「强制全量同步」按钮，把 `last_cursor` 置 0 重拉一遍。** 数据幂等，重拉无害。

### 增量传输：按 `updated_at` 水位

每次同步不必传全量，只传 `updated_at > 上次水位` 的行。曲库不同步之后，数据量比原先估算的小得多（原估算里 748 KiB 的曲库快照已经不存在了）：

| 场景 | 大小 |
| --- | --- |
| 播放列表 + 设置（几百条），gzip | 几十 KiB |
| 每天新增播放记录（45 条） | 7.0 KiB 原始 / 约 1.8 KiB 压缩后 |
| 播放历史 1 年（16000 条），gzip | **1.12 MiB** |
| 播放历史 10 年 | 十 MB 量级 |

**播放历史是唯一会长大的那一项。** 稳态每年约 1 MiB（gzip 后），日常增量不到 2 KiB。**结论：不需要归档策略，"每天自动备份"完全可行。**

⚠️ **增量拉取看不到删除。** 只拉"比我新的行"是不够的——被删掉的行不会再被推送。所以删除必须表现为一次带时间戳的写入：`deleted_at` 被赋值，`updated_at` 随之更新，这样增量拉取才能看到它。这就是墓碑不能省的根本原因。

### 播放列表合并：不需要「覆盖 / 不覆盖 / 融合」三选一

**这是本轮改动最大的地方，原来的三选项方案被推翻了。**

原来的设想是：检测到冲突后弹窗问用户「覆盖 / 不覆盖 / 增量融合」。放弃它的原因是，**加了条目墓碑之后，合并变成了确定性的自动操作**，三选一就退化成一个粗糙的替代品——用户真正想要的是"这几首我不要"，而不是"整个列表我要哪一份"。

**合并规则（对所有 `playlist_items` 按 `uid` 做）：**

```text
对端有、本机没有      → 插入
两边都有              → 按 (updated_at, updated_by) 取大的，整行替换
```

就这两条。**`deleted_at` 不是一个需要单独判断的东西**——它就是行里的一个时间戳列，随整行一起被 last-write-wins。这条看起来太简单，但它是被原型逼出来的，见本节末尾"我写错过一次"。

### ⚠️ 我写错过一次：墓碑不能写成特殊规则

我最初的草案里，"两边都有"这一支拆成了三条：

```text
远端.deleted_at 非空 且 更晚  → 本地也软删除
本地.deleted_at 非空 且 更晚  → 保留墓碑，推给远端
否则按 (updated_at, updated_by) 取大的
```

第二条**会让墓碑永久粘住**。实测场景：

```text
t=10   PC 和手机都有 B
t=60   PC 删了 B           → PC:  deleted_at=60
t=70   手机把 B 又加回列表  → 手机: deleted_at=NULL, updated_at=70
```

| 合并方式 | 结果 |
| --- | --- |
| 带那条特殊规则 | `[]` — **B 丢了**。用户明明在手机上重新加回来了，另一台却再也收不到 |
| 纯 last-write-wins | `[B]` — 正确 |

原因：那条规则说的是"本机有墓碑就保留墓碑"，它根本没看对端那条是不是**更晚的重新加入**。它把"删除"实现成了一个单向棘轮——只能删、不能恢复，而用户重新勾选进列表就是恢复。

**改成纯 LWW 之后，原来要墓碑支撑的五件事全部仍然成立**（都实测过）：对端删除传到本机 ✅、本机删除传到对端 ✅、对端新增插入 ✅、拒绝的新增（写墓碑）不再出现 ✅、重新勾选（清墓碑）能传播 ✅。而且合并**仍然顺序无关且幂等**（PC 合并后 `[A,C,D]`、手机合并后 `[A,C,D]`，再同步一轮不变）。

**这条经验的普适形式：任何"某列非空就永远保留"的规则都是单向棘轮，先问一句"用户要怎么撤销它"。** 撤销路径存在就必须让撤销也走同一条 LWW 通道。

### 顺序无关是取消三选一的前提

用 PC `[A,B,C]` 和手机 `[A,C,D]`（手机上有 `B` 的墓碑）两个方向同步：

| 同步顺序 | PC 结果 | 手机结果 |
| --- | --- | --- |
| PC 先拉 | `[A,B,C,D]` | `[A,C,D]` |
| 手机先拉 | `[A,B,C,D]` | `[A,C,D]` |

两次结果完全一致。**如果合并结果取决于谁先同步，就必须让用户裁决用哪一份；既然结果确定，用户只需要在浮窗里事后微调。** 这就是三选一不再需要的根本理由。

**条目墓碑解决了原来的"复活"问题。** 实测：PC `[A,B,C]`、手机删了 `B`，同步后双向都是 `[A,C]`，`B` 不再回来。原来"融合会复活删掉的歌"这个已知缺陷，随条目墓碑一起消失了。

### 同步结果的三种提示：Toast / 浮窗 / 持久横幅

「只在有变化时弹浮窗」这句话还漏了一种情况：**没变化时也得让用户知道同步确实跑过了**。但不同结果该用不同强度的提示，混用会让提示失去意义。

| 结果 | 提示形式 | 消失方式 | 理由 |
| --- | --- | --- | --- |
| 成功，**有变化** | 浮窗（条目列表 + 勾选框） | 用户手动关 | 需要用户决策 |
| 成功，**无变化** | Toast「同步完成，无变化」 | 3 秒自动 | 无需决策，只是确认收到了指令 |
| **失败** | 顶部持久横幅 + 「重试」按钮 | 重试成功或用户手动关 | 见下 |

**失败绝不能用 Toast。** Toast 三秒后消失；如果用户正好低头、切了窗口、或者手机锁屏，回来看到界面一切正常，实际同步已经失败——**他会以为数据同步了，但它没有**。失败必须留下痕迹，直到被处理。

**Toast 只给"手动同步"用。** 自动同步（后台每 N 分钟一次）在没有变化时**静默**，只更新 `sync_state.last_sync_at`。理由：一天几十条「同步成功」会让用户迅速学会无视所有提示，反而连真正的失败也忽略掉。自动同步有变化时照常弹浮窗，因为那需要决策。

**文案用「同步完成，无变化」而不是「播放列表同步成功」**，两个原因：①同步的不止播放列表，还有设置、评分、播放历史，说"播放列表"会让人以为别的没同步；②光说"成功"用户会去翻找"到底更新了什么"，翻完发现没变化——"无变化"三个字直接回答了这个问题。

### 设置页：上一次同步的分项报告

Toast 和浮窗都会消失，但用户需要一个随时能查的地方——**这是设置里的同步页面，不是常驻在主页的横幅**。同步是辅助功能，不该占据播放界面的显著位置。

页面顶部一行摘要，下面是可以展开的分项明细：

```text
上次同步：2026-11-23 15:42（手动）· 成功

设置        ✅ 成功     接收 3 项，发送 1 项
播放列表    ✅ 成功     接收 2 项，发送 4 项
评分        ✅ 无变化
播放历史    ✅ 成功     接收 40 项，发送 12 项
汇总        ✅ 成功     接收 66 项，发送 15 项
```

失败时对应那行变成「❌ 失败 · HTTP 507 Insufficient Storage」，并保留「重试」按钮。数据来自 `sync_runs` / `sync_run_items` 两张表（§2.12），不参与同步、只存最近 20 次。

**为什么失败要精确到分类而不是一句话：** "同步失败"没法排查。是服务器满了（507）、密码错了（401）、还是某一类数据格式不对？分类后用户至少知道该去改哪件事。

### 同步结果浮窗：勾选框就是墓碑的写入口

浮窗**只在确实有变化时**出现，**只列新增的曲目和被移除的曲目**（不列整个列表——300 首全列出来等于没列表）。

浮窗里每一项带勾选框，默认勾选状态是：

| 类别 | 默认 | 取消勾选的含义 |
| --- | --- | --- |
| 新增曲目（对端有、本机没有） | ✅ 勾选（加入） | 本机不要这首 → **写墓碑** |
| 被移除曲目（对端删除） | ✅ 勾选（移除） | 本机保留这首 → **清墓碑** |

**这两个勾选框必须落成带时间戳的写，不能只"这次跳过"。** 实测对比：

| 实现方式 | 用户取消勾选新增的 `D` | 下次同步 |
| --- | --- | --- |
| 只是不插入 | PC `[A,B,C]` | ❌ `D` 又出现，每次同步都问一遍 |
| 写一条墓碑 | PC `[A,B,C]` | ✅ 不再出现 |

原因很简单：对端那条 `D` 还是"活着"的，本机没留下任何记录说"我不要它"。**取消勾选就等于删除这一条**，和正常删除必须完全一样。

反向也成立：用户重新勾选一条有墓碑的歌，**必须清掉墓碑**。清墓碑本身也是一次带时间戳的写，会传播。实测：PC 重新勾上 `B` → 同步后手机上 `B` 回来了，并且留住了。

这跟评分清除是同一条规则：**"没有"也必须是一次显式的、带时间戳的写。**

### 「清除所有播放列表的墓碑」：保留，但必须二次确认

这个设置项保留，它的用途是给用户"抛弃前嫌、重头再来"的权利——不想再被一年前的删除记录影响，就把它们全部放掉。

**但它有一个真实的破坏性**，实测过最坏情况：

```text
PC 删了 B → 本机有 B 的墓碑
PC 点了"清除墓碑" → 墓碑行被物理删除，本机不再记得 B 被删过
之后与手机同步 → 手机说"我有 B" → B 回来了
```

**所以必须有二次确认窗口**，并且说明后果是"这些被删掉的歌可能在下次同步时回来"。

**安全水位：加，但单设备时不设限。** 判定规则：

```text
有对端设备（devices 表里除本机外还有记录）：
    只清 deleted_at < min(所有设备的 last_sync_at) 的墓碑
没有对端设备：
    直接全清
```

实测多设备时这样清完再同步，`B` 不会回来。而单设备用户本来就不存在"对端有 B"这种情况，全清是安全的；如果他们点按钮却看到"清了 0 条"，只会以为功能坏了。**二次确认和逻辑正确是两件事，两个都要。**

**数据量上这个设置其实没必要。** 一条墓碑约 90 字节：

| 场景 | 原始 | gzip |
| --- | --- | --- |
| 1 个列表 300 首，无删除 | 27 KiB | 2.6 KiB |
| + 300 条删除（一年） | 52 KiB | 4.0 KiB |
| + 3000 条删除（重度折腾） | 279 KiB | 18.5 KiB |
| 10 个列表各 300 首 + 3000 删除 | 523 KiB | 40.7 KiB |

带 3000 条墓碑时查"活着的条目"是 **0.057 ms**，没有性能影响。所以这个设置的定位应当是**心理上的"清理"**，不是性能优化——文档和 UI 描述都不该暗示用户"不清会变慢"。

此外还有一条**自动**的墓碑回收规则在跑：`deleted_at` 早于 90 天、且所有已知设备都已同步过该时间点的墓碑，物理删除。这条不需要用户干预，和上面的手动按钮是两回事。

### 设置的冲突：仍然只给"覆盖 / 不覆盖"

设置的标识就是 `key`，不存在"两个同名设置"，所以合并没有意义，仍然只在真冲突时问用户。**判据和播放列表一样**：

```text
真冲突  ⇔  本地.updated_at > 对端.last_sync_at
       且 远端.updated_at > 本地.last_sync_at
```

只有一边改过就静默取那一边，不问。

**同名但 uid 不同的播放列表不构成冲突。** 两台设备各自新建一个都叫「我的最爱」的列表，uid 不同——按 uid 合并的结果就是两个列表并存，同名。这本身就是"各留一份"，用户去改个名就行，不需要任何"重名检测"逻辑。

### 字段级同步语义：`rating`

星级评分存在独立的 `track_ratings` 表里（§2.14），键是 `fingerprint`。规则是**新覆盖旧**（last-write-wins），包括**清除评分**：

| 操作 | 结果 |
| --- | --- |
| PC 评 3 星（t=200） | 手机同步到 3 星 |
| 手机改 4 星（t=300） | PC 同步到 4 星 |
| PC 清除评分（t=400，写 `rating = 0`） | 手机也变成未评分 |

最后一条是关键：清除评分仍然是一次**带时间戳的写操作**（`rating = 0`），所以它正常传播。**不要**把"值为空"实现成"不参与同步"——那样取消评分永远传不到另一台设备，用户会看到"我明明取消了，手机还是 5 星"。

**为什么用 0 而不是 `NULL` 表示未评分：** `NULL` 在 `NOT NULL` 约束下写不进去，而如果把列改成可空，`NULL` 和"行不存在"就无法区分——行不存在意味着"这台设备从没评过"，清理或迁移时应该被丢弃；`rating = 0` 意味着"用户明确取消了评分"，必须传播。这两种状态不能用同一个值表示。

**换版本会丢评分。** 评分挂在 `fingerprint` 上，换成另一个编码版本就是另一个指纹，评分归零。这是有意的：`work_key` 适合做听歌次数（版本无关），但评分是"我对这个文件/这个音质的判断"，跨版本继承反而是错的。

### 墓碑回收

**只对 `playlist_items` 生效**——曲库已经不同步，`tracks` 上没有 `deleted_at` 列了（用户删歌就是 `DELETE` 行）。

`playlist_items.deleted_at` 早于 90 天、且所有已知设备都已同步过该时间点的行，物理删除。这需要读 `devices.last_sync_at`——所以那张表不是摆设。

---

## 8. 表清单

| 表 | 用途 | 同步 | 说明 |
| --- | --- | --- | --- |
| `meta` | 键值元信息 | 部分 | `device_id` 不同步 |
| `devices` | 对端设备 | 是 | |
| `artists` | 艺术家 | **否** | 本机扫描缓存，不是同步实体 |
| `albums` | 专辑 | **否** | 本机扫描缓存 |
| `tracks` | 曲目 | **否** | 本机扫描缓存；`fingerprint`/`work_key` 两端各算各的也能一致 |
| `track_files` | 曲目的本地文件 | 否 | 设备本地；一首曲目可对应多个文件 |
| `track_artists` | 曲目-艺术家关系 | 否 | 随曲目，本机派生 |
| `track_ratings` | 星级评分 | 是 | 稳定键 `fingerprint`；曲库不同步但它必须同步 |
| `playlists` | 播放列表 | 是 | 稳定键 `uid` |
| `playlist_items` | 列表条目 | 是 | 逐条同步；`order_key` 稀疏排序；条目墓碑 |
| `settings` | 设置 | 按 `scope` | `device` 的不走同步 |
| `play_queue` | 播放队列 | 否 | 设备本地 |
| `play_history` | 播放记录 | 是 | 按 `uid` 去重；统计用快照兜底 |
| `play_stats` | 听歌汇总 | 是 | 主键 `(stats_key, device_id)`，两端永不撞行 |
| `stats_hidden` | 手动删除的汇总条目 | 是 | 键 `stats_key`；先于 `play_stats` 合并 |
| `v_stats` | 汇总视图（隐藏判定） | 否 | 派生视图，读 `play_stats` + `stats_hidden` |
| `tracks_search` | 搜索用归一化列 | 否 | 派生，可重建 |
| `sync_state` | 同步水位 | 否 | 设备本地 |
| `sync_runs` | 同步日志（每次一行） | 否 | 只留最近 20 次 |
| `sync_run_items` | 同步日志分项 | 否 | 随 `sync_runs` 级联删除 |

**注意「同步」那一栏打「是」的只有 8 张**：`devices`、`track_ratings`、`playlists`、`playlist_items`、`settings`（仅 `shared` 部分）、`play_history`、`play_stats`、`stats_hidden`。其余全部是本机私有的缓存或状态，怎么删怎么重建都不影响对端。

---

## 9. 已定与未定

### 已定（本轮确认）

| 议题 | 结论 | 依据 |
| --- | --- | --- |
| 曲库真源 | 库优先，保留"打开即播"旁路 | §1 |
| 同步范围 | **曲库不同步**；只同步播放列表、设置（`shared`）、播放历史、星级评分、听歌汇总 | §0.1 |
| 艺术家/专辑 | 规范表 + 保留原始字符串 | §2.2 §2.3 §2.4 |
| 播放队列 | 双队列，插播条目用 `origin` 区分颜色 | §2.9 |
| 洗牌 | 持久化洗牌顺序 | §2.9 |
| 时长精度 | 毫秒整数 | §5 |
| 同步字段 | `updated_at` / `updated_by` / `revision` / `deleted_at` | §0 |
| 播放记录 | **参与同步**，为听歌统计服务 | §2.10 |
| 智能播放列表 | JSON 语法树 + 白名单编译 | §6 |
| 歌词 | 默认只读内嵌；`.lrc` 由开关控制，内嵌永远优先；不建表 | §2.13 |
| 文件夹层级 | 不建 `folders` 表，用 `track_files.folder_path` 索引 | §2.5 |
| 文件与曲目分离 | 一首曲目可对应多个文件；同一首歌放在两处不会丢歌 | §2.5 |
| 缺文件 vs 已删除 | 文件丢失＝`has_local_file = 0`（记录保留）；用户主动删除＝`DELETE tracks` 行（曲库不同步，无需墓碑） | §2.4 §2.5 |
| 曲目指纹 | 先剥 ID3v2/APEv2/ID3v1 标签，再对纯音频区间取哈希 | §4 |
| 统计聚合键 | `work_key = 归一化(艺术家) + 归一化(标题)`，不带专辑、不剥括号；跨版本合并 | §4.6 |
| 无标签文件归为「未识别」 | `work_key` 为空时 `stats_key` 退回 `fingerprint`、`identified = 0`；计入总量、不进排行榜 | §2.15 §4.6 |
| 扫描时机 | 设置页手动扫描按钮 + 「启动时扫描变更」开关（默认开）；不做文件系统监听 | §2.5 |
| 低精度文件系统 | 不探测文件系统类型，一律按最坏 2 秒；每文件存 `fp_verified_at` 做 racy 保护 | §2.5 |
| 播放次数阈值 | 设置项，默认 **50%**；判定在**写入时冻结**进 `counted` 列，**不追溯重算** | §2.10 |
| 播放历史保留 | 默认 **永久保留**；不重算阈值，因此清理策略独立 | §2.10 §7 |
| 星级评分 | 独立表 `track_ratings`，键 `fingerprint`；同步「新覆盖旧」，清除＝`rating = 0` 也是一次带时间戳的写 | §2.14 §7 |
| 听歌汇总 | 独立表 `play_stats`，主键 `(stats_key, device_id)`，两台设备永不撞行、无需冲突处理 | §2.15 |
| 汇总只做一项排行 | 「听的次数最多」列表，按次数降序，0 次不参与；**不做「听的时间最长」** | §2.15 |
| 汇总条目可手动删除 | 独立表 `stats_hidden`（键 `stats_key`，带 `restored_at` 边界），跨设备生效；**先于 `play_stats` 合并**；入口为**长按/右键 + 二次确认**，行上不挂常驻按钮 | §2.15 §7 |
| 编辑曲目标签 | 写回文件本身、**一次只改一个文件**；指纹不变所以 `id` 与评分/列表引用都不受影响；`work_key` 变了**不处理**（不迁移、不加别名表） | §2.16 |
| 增量备份 | 按 `updated_at` 水位；曲库不同步后，播放历史是唯一会长大的项，每年约 1 MiB（gzip） | §7 |
| 播放列表合并 | **自动按 `uid` 合并，不弹三选一**；结果与同步顺序无关 | §7 |
| 条目墓碑 | `playlist_items` 加 `deleted_at`，删除可传播、不再复活 | §2.7 §7 |
| 同步结果浮窗 | 只在有变化时弹，只列新增/移除曲目，勾选框＝写墓碑/清墓碑 | §7 |
| 同步提示分级 | 有变化＝浮窗、无变化＝Toast、失败＝持久横幅；自动同步无变化时静默 | §7 |
| 同步日志 | `sync_runs` + `sync_run_items`，设置页显示上次同步的分项结果，保留最近 20 次 | §2.12 §7 |
| 清除墓碑 | 保留该设置项，**必须二次确认**，说明可能导致已删歌曲回来 | §7 |
| 设置冲突 | 仍为「覆盖 / 不覆盖」两选一，不做融合 | §7 |
| 统计时间维度 | **不做**按月/年预聚合表；时间维度直接查 `play_history` 明细 | §2.15.1 |
| 统计时区 | 按**设备当前本地时区**，用注册进 SQLite 的 `local_day()` 函数算，**不用固定偏移** | §2.15.1 |
| 播放历史视图 | **列表 + 日历**两种，页面顶部切换；日历每格显示当天播放次数/总时长 | §2.15.1 |
| 「只统计本机」开关 | **不做**。加了两端数字不一致，用户会怀疑哪个对 | §2.15 |
| 清除墓碑的安全水位 | **加**：多设备时只清 `deleted_at < min(所有设备 last_sync_at)` 的墓碑；**单设备时直接全清** | §7 |
| 歌词展示形态 | **全屏页 + Windows 桌面悬浮窗**；**不做侧栏** | §2.13 |
| 队列来源 | `origin` 四值各回答"这行怎么进来的"：`user` 用户显式排、`play_next` 下一首播放（播完即弃）、`context` 系统按专辑/列表原序补、`shuffle` 洗牌生成；**视觉层只高亮 `play_next`** | §2.9 |
| 重新洗牌时机 | 首次开启随机时生成；**播完一轮自动重洗且避开刚播完那首**；手动按钮可随时重来；**切换列表不重洗** | §2.9 |
| 智能列表结果上限 | **不设上限，全部显示**。查询不加 `LIMIT`；界面用游标分页按需读，列表只取 id/标题/艺术家/时长 | §6.7 |
| `counted` 冻结的告知方式 | **只在设置项描述里说**：「更改阈值不会改变以前的计数」。统计页面不加说明文字 | §2.10 |
| 同步日志保留量 | **保留最近 20 次**，不做"保留 N 天" | §2.12 |
| 浮窗的"这次都这样" | **不做**。不加全局开关，也不加列表级同步开关；保持逐条勾选 + 写墓碑 | §7 |
| 排序键 | **删除 `sort_name`**，英文用 `ORDER BY name_norm`。`sort_name` 在带变音符号时反而更差（`Björk` 被排到 B 区末尾） | §3.2 |
| 中文排序 | 自带 6763 字拼音表（紧凑 16 KiB），扫描时算出 `artists.pinyin_key` / `albums` 对应列，排序走索引；**必须逐字比较、不可整串拼接**；多音字按第一读音固定；列现在就在表里、**v0.3 才填值** | §3.2 |
| HLC / 单调钳制 | **都不用**。HLC 治不了并发和水位漏行，却要付 16 字节时间戳 + 失去可读性 + 合并逻辑改造；单调钳制同为极罕见场景加状态。接受局限，靠「强制全量同步」按钮兜底 | §7 |
| 增量水位定义 | `last_cursor` = **从对端见过的最大的 `updated_at`**，不是本机此刻时钟 | §7 |
| `within_days` 算子 | **补进白名单**（`added_at` / `last_played`）。不能用 `NOT before_days` 代替——`NULL` 三值逻辑会让「从没听过」混进「最近听过」 | §6.4 |

### 未定

- **Toast / 浮窗用现成组件库还是自建**：Qt Quick Controls 没有 Toast，自建一个不难，但有现成的可参考。留到写 UI 时一并决定。
