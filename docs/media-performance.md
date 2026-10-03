# 浏览器媒体计算验收（2026-09-29）

本页记录真实浏览器解码和数值验证，不能替代所有编码、设备、长视频播放或音画同步验收。原始结果见 `media-performance-results.json`。测试页是 `tests/browser-media.html`，不进入产品页面或发布包入口。

## 执行环境与素材

- macOS、Apple M4 Pro、48 GiB 内存；Codex 内置浏览器 Chrome 154，DPR 2。
- Mediabunny 1.61.0；使用已有本地服务授权的 HTTP Range URL，浏览器 WebCodecs 解码；没有新增 FFmpeg 包或替代包装。
- 640×360、30000/1001 fps、H.264，4.004 秒；VFR 文件保留前两秒全部帧、后两秒每三帧保留一帧。
- 每个请求时间的参考帧由已有 ffprobe 的整数 PTS / time_base 计算，避免六位小数显示值在边界误选上一帧。
- 完整 3600 秒、48 kHz、双声道 FLAC；每十秒左声道在 +0.5 秒有 +0.9 脉冲，右声道在 +5.5 秒有 −0.8 脉冲。共 720 个单样本瞬态；文件约 1.2 MB，但若解为整段 Float32 PCM 将约为 1.29 GiB。

## 验证内容

1. 29.97 与 VFR 各 16 个随机请求时间，返回 actual PTS 与参考帧一致（允许 2 微秒表示误差）。第二遍命中帧缓存且没有新解码任务。
2. 持久视频 reader 连续采样 120 次，加三次向后或远跳定位；全部返回正确帧。
3. 12 个相同请求共享一个解码任务；8 个不同请求的观测并发上限为 2；取消请求及时返回 AbortError，资源按引用释放。
4. `ClipVisual.vue` 实际挂载并通过独立 worker 生成多格 filmstrip 与每声道数值波形，DPR 2 backing canvas 对应约 721 CSS 像素的可见区，显示第一格与最后一格实际 PTS。
5. 一小时波形处理完整区间，每声道 360 个脉冲全部存在，末尾 3590.5 / 3595.5 秒也有正确极值；7200 列输出没有截断。PCM 长期缓存字节数仍为 0，波形 LRU 限额 16 MiB。
6. 不同授权 session 使用同一完整 dev:ino:size:mtime 源身份时，缓存命中且没有新解码任务；单独文件大小不能作为共享身份。

## 结果与边界

首轮完整复核于 2026-09-29 21:56:52–21:57:02（Asia/Shanghai）完成，10 组浏览器验收全部通过，`longAudioFreshCache: true`。视频“冷”是本次 engine 的新帧任务，HTTP / 操作系统磁盘缓存可能已热。

| 验收 | 本次实测 |
| --- | --- |
| 29.97 随机帧，16 个 | 冷 median 15.7 ms / P95 32.7 ms；热 P95 0.1 ms |
| VFR 随机帧，16 个 | 冷 median 17.5 ms / P95 22.4 ms；热 P95 0.1 ms |
| 持久 VFR cursor，120 连续 + 3 跳转 | 共 73.1 ms；连续帧 P95 0.7 ms |
| 12 个相同请求 | 1 次生产任务；取消响应 1.9 ms |
| 8 个不同请求 | 观测最大并发 2 |
| 跨 session 同源帧 | cache hit +1，新解码 0 |
| ClipVisual worker + DPR 2 | 110.1 ms，1442 backing px / 720.7 CSS px |
| 完整一小时波形，未复用既有峰值 | 8638.3 ms；720 / 720 指定通道瞬态存在 |
| 完整一小时波形，紧接着读取缓存 | 83.1 ms |
| 观测缓存峰值 | frame 7.86 MiB；waveform 15.98 MiB；PCM 0 |
| 浏览器报告 JS heap 峰值 | 125.64 MiB |

此前独立完成过 5844.9 ms 冷波形 / 58.3 ms 热缓存，本页保留最终更完整一轮的结果，不把单次较小值当作保证。

耗时为该设备一次运行的观测值，浏览器与其他验收进程可能争用资源；这不是稳定吞吐基准或所有素材的速度承诺。JS heap 数字是浏览器报告值，不包含全部 GPU、系统解码器或浏览器进程内存。并发上限是本次媒体实例的任务数，并非整机解码器数。

波形以 256 PCM samples 为基础 bucket 保存 min/max，再构建二倍尺度金字塔；在极高缩放下尚不提供每个 sample 的独立绘图，但不会因平均值抵消正负瞬态。播放与导出当前采用同一分块线性重采样混音；改变速度会改变音高，保调按计划后置。双声道之外的布局明确拒绝自动混音。

Native Format 1 已验证 blur / glow / LUT 与四种转场往返，LUT amount 支持 itemLocal 曲线，黑白 fade 均保存和回读。当前 native catalog 不允许 blur、glow 数值参数动画，网页同步禁止。glow 参数映射到原生 Core Image CIBloom；WebGPU 实现与该封闭算法只有参数含义对应，未验证与桌面逐像素等价。

## 本次真实验收发现并修复

- FLAC 首包初始化与时长探测并行会触发 Mediabunny 1.61.0 的 blocking-bit 断言；现在先完成首包初始化，再探测时长。
- IDB 淘汰扫描原本会反复读取和复制完整峰值数组；现在用独立的小型 usage 元数据表决策淘汰，媒体值只在实际请求时读取。
- 初始音频块仍在准备时，旧 AudioContext 的 currentTime 不能作为已经开始的播放时间；现在待首块准备完成才建立时钟基准。
- 缩略图和波形独立发布，冷波形不会阻塞已完成的 filmstrip。

## 复现

1. `node tests/prepare-media-fixtures.mjs` 使用已有 ffmpeg/ffprobe 准备 `.local/architecture-qa`，不改产品依赖。
2. 启动现有授权 Node 服务并使 Vite 代理指向它；测试目录需位于授权 roots 内。验收时 Vite 端口为 5189。
3. `node tests/media-report-server.mjs` 将只接受该本地测试页的 JSON 报告，监听 127.0.0.1:4333。
4. 打开 `http://127.0.0.1:5189/tests/browser-media.html?autorun=1&cold=1`。测试不会清除用户缓存，`cold=1` 仅为长音频选择新的 session 缓存键。
5. 结果写到 `.local/architecture-qa/media-results/browser-report.json`，要求 `phase: complete`、`logs: []`，并确认 `longAudioAnalyzedSeconds: 3600`。

普通模块测试：`node --test tests/media-runtime.test.mjs`（11 项），覆盖 LRU 字节预算、去重与消费者取消、资源释放、通道极值金字塔、DPR、混音区间/增益/静音、首块时钟与源身份。

Timeline 虚拟化的产品 DOM 计数可用 `.timeline-clip` 与 `[data-track]`，总项目 item 数和实际挂载节点数需分别报告；本页不把单个 ClipVisual 的通过等同于 1000 clip 的产品 UI 验收。

## 默认元数据不需要 FFprobe

最终 Node `probe()` 默认使用 Mediabunny FilePathSource，仅解析容器和有限包时间戳，不启动外部进程；只有调用方明确提供 fallback 命令且 JS 不支持时才会调用它。没有新增、打包或下载 FFmpeg 依赖。上述 `prepare-media-fixtures.mjs` 中 FFmpeg / FFprobe 仅是开发验收素材生成器和独立参考数据工具，用户导入与 native 保存不要求安装它们。

`node --test tests/media-probe.test.mjs tests/media-native.test.mjs` 实测 8 / 8 通过。覆盖：无需任何外部程序的合成 WAV、JPEG EXIF 90° 显示方向、仅显式 fallback 会启动程序、MP4 / Matroska 保留字幕和未知轨道的物理 index、真实 29.97 / VFR / 3600 秒 FLAC、旋转 MOV、字幕放在第一个轨道的 MP4 和 MKV。Native 完整导入→保存→打开测试把 FFprobe 配为不存在路径，仍通过。

帧率指标最多检查前 256 包并记录 probedPacketCount，不能据此前缀证明整段素材恒定帧率。图片尺寸读取最多 1 MiB 头部；特殊文件或不支持的容器会明确报错，若显式配置可使用原有 FFprobe 兜底。HDR 继续在 SDR 项目入口明确拒绝，不允许通过兜底跳过。

## 缓存、高清缩略图与可见调度补完

`packages/media/index.ts` 按源身份保存只读元数据，以及真实解码观察到的 PTS / duration 区间。索引按 10 秒分页，每页最多 4096 条；未知时间点仍走实际解码，不把 FPS 推算当成帧索引。它共享的是可复用的源事实，而非可变 decoder 状态；Mediabunny Input 的内部完整解复用表仍由各个浏览器 worker 独立管理。连续播放保持独立 reader，向后或远距离跳转才重建解码游标。

`MediaArtifactStore` 的 IndexedDB schema 为 3，产物键含算法版本、完整源版本和像素尺寸，最终存储键只包含类别和 SHA-256。磁盘值只有数值元数据、峰值数组或重新编码的 PNG，不保存原始路径、会话 token 或授权 URL。索引合并使用单个 read/merge/write 事务，避免两个 worker 同时写入时互相覆盖。读取会更新 LRU；淘汰扫描独立的小型 usage 表，不复制 PNG 或 PCM。缓存缺失后可以重算。显式失效有独立 epoch，即便某个 engine 只读过持久缓存、从未打开过 Input，也不会再次命中被失效的产物。

| 缓存 | 字节预算 |
| --- | --- |
| 每个 media engine 的帧 / 图片 | 96 MiB |
| 每个 engine 的分块 PCM / 波形金字塔 | 32 MiB / 16 MiB |
| 每个 engine 的只读 metadata / 稀疏 PTS 页 | 1 MiB / 2 MiB |
| 主线程所有 ClipVisual 共用的 tile 缓存 | 16 MiB |
| IndexedDB 总计 | 128 MiB |
| IndexedDB 缩略图 / 波形 / 索引分类 | 64 MiB / 56 MiB / 8 MiB |

这些是有明确所有权的缓存载荷预算。它们不是整个浏览器的内存上限，也不包含 WebCodecs 内部解码队列、活跃 reader 的少量 VideoSample、GPU 纹理或 JS GC 尚未回收的临时数据。Input 的 Range 缓存为每源 4 MiB，空闲源保留数量为 6；当前被使用的源不会为了凑空闲上限而强制中断。

`ClipVisual` 按完整槽宽、槽高、DPR 和裁剪比例生成像素尺寸，直接从解码 sample 绘制到目标槽，消除竖屏先按高度缩小、再填充 80 CSS 像素槽时的放大模糊。视频缓存包含实际 PTS、宽高、fit 和 crop；图片预览也把目标宽高纳入缓存。滚动时同步复用现有高清 tile，35 ms 后请求可取消的低分辨率缺失 tile，稳定 140 ms 后补精确高清；已高清的格子不会重新覆盖成低清。图片整条 filmstrip 共享同一 bitmap。完全命中的 tile 不再发送空 thumbnail RPC。

预览 / 导出读取帧时广播一个不包含源信息的短期前台活动信号。缩略图与波形使用独立的单并发后台队列，在新解码与后续块边界让步；离开可见区或更新 viewport 会取消消费。已缓存的 tile 可以立即绘制。后台让步不是中断系统解码器内部的一帧操作，因此不能据此承诺整机完全无资源争用。

`tests/browser-media-cache.html` 通过真实 ClipVisual 和 Timeline 验证几何、实际 PTS、跨 engine / worker 持久缓存、LRU 字节预算与不含私有路径的键。`tests/browser-media-long.html` 使用另一个 120 秒的 H.264 B-frame 源，按原分辨率持续向前读取，同时滚动 1000 个不同源裁剪位置的 clip。测试只使用已有 FFmpeg 制作开发素材，不增加产品依赖。报告服务器为 complete / failed 自动保留唯一归档，避免后续 HMR 覆盖验收证据。


## 最终补完实测（2026-09-29 23:47–23:49）

原始机器可读结果归档在 `media-cache-results.json`。长源页六组全部通过；首轮短源五组记录仍保留在 `.local/architecture-qa/media-results/cache-geometry-validation.json`，报告服务器还按时间保留唯一 JSON。本节是同一新实现的冷 / 热缓存对比；没有把它当成旧版到新版的加速比例。

| 验收 | 最终实测 |
| --- | --- |
| 29.97、VFR、竖屏、旋转、PAR；DPR 1 / 2 | 50 张缩略图实际 PTS 与 ffprobe 参考一致，误差小于 2 µs |
| 同目标尺寸、裁剪的全分辨率绘图参考 | 普通 / VFR / 竖屏 / 旋转 MAE 为 0；PAR 最大 0.121 / 255 |
| 每组 5 张，首次新媒体缓存身份 | 61.7–91.3 ms，均实际解码 5 张 |
| 紧接着相同画质、尺寸和时间点热读 | 每组 0–0.2 ms |
| 新 engine / 新 worker 读取持久 PNG | 各 3 / 3 命中，Input 数 0，解码数 0；新 engine 总计 1.8 ms |
| 无 Input 实例显式 invalidate 后 | 同三张重新解码，未复活旧持久产物 |
| 图片目标尺寸与裁剪 filmstrip | 160×90 / 320×180 分别缓存；独立绘图参考 MAE 1.09 / 255 |
| trim + split + 2× 速度、DPR 2 | 源采样时间与 core 映射一致，160×120 tile，滚动即时复用 2 格 |
| 独立 IDB 800000 B 预算测试 | 三个 300000 B 产物触发 LRU，保留刚访问项；最终 600000 B，两项，键仅为类别加 SHA-256 |
| 120 秒源、3596 个连续请求，同时滚动 1000 clip | 120.204 s；每次 PTS 均通过；帧请求 median 0.3 ms / P95 0.4 ms / max 5 ms |
| 1000 clip 的实际挂载 DOM | 最大 80 个 clip；停止播放后 215.4 ms 首个真实可见高清节点恢复 |
| 长播期间观察值 | 主 engine 帧缓存 28.39 MiB；后台 worker 帧缓存峰 1.18 MiB；后台活动任务峰 1 |
| 浏览器报告 JS heap 峰 | 61.94 MiB；30 / 60 / 90 秒采样为 51.32 / 50.86 / 44.63 MiB |
| 当前稳定源码的一小时波形复核 | 3600 s、720 / 720 脉冲；冷 6309.7 ms / 热 83.7 ms；波形缓存 15.98 MiB、PCM 0 |

冷缓存控制只给测试选择新的 session 缓存身份，不清除用户持久缓存；HTTP 和操作系统磁盘页可能已经热。连续帧计时包括 MediaReader 取帧与 bitmap 生成，不包括 GPU 效果合成、音频设备播放或显示器呈现。后台统计每 30 帧观察一次，不能证明采样间隔内不存在更短的波峰；JS heap 是浏览器提供的采样值，不能代表所有 worker、GPU、系统解码器和进程的合计内存。120 秒压力覆盖持续向前读取，仍不等于所有时长、格式和硬件的稳定性保证。

这次实测还发现并修复完全命中的 filmstrip 仍发送空批次 RPC的问题。最终长播中后台 decode 在播放期间维持 24 张，期间约 3.2 万个过期消费被取消；停止后才继续生成当前可见高清图。记录这一行为是为了说明后台工作的让步边界，不把任务取消次数本身当成性能收益。

复现最终补完：准备上述素材后打开 `http://127.0.0.1:5189/tests/browser-media-long.html?autorun=1&cold=1`，约 125 秒。要求 `phase: complete`、六组通过、3596 个逐帧 PTS 断言通过。该页同时运行所有高清几何、持久缓存、失效、图片尺寸和实际组件检查。原一小时页仍为 `tests/browser-media.html?autorun=1&cold=1`。媒体相关普通模块测试本轮为 21 / 21，通过 TypeScript 检查；这两项不能代替以上浏览器运行证据。
