# 浏览器合成、缓存与导出验收（2026-09-30）

固定 1280×720 的本机对照中，移动带 blur、glow、LUT 的图片层，单帧耗时中位数从 2.4–2.9 ms 降到 0.7–0.9 ms；四组配对减少约 67–72%。重复同一画面直接复用最终合成结果。四轮全部通过像素检查，报告保留的最后一轮 RGBA 逐字节一致。对照关闭自适应降清，收益没有来自降低分辨率。

这比较的是**本轮缓存升级前冻结的 npm 包与当前实现**，不是最初网页编辑器、桌面 GUI、所有素材或全部编码器的性能结论。完整报告和来源文件 SHA-256 已保存到 [render-performance-results.json](render-performance-results.json)；本文数值来自这些报告。

## 环境与计量方法

- 本机 Apple M4 Pro、48 GiB 内存，macOS，Codex 内置 Chrome 154；没有操作桌面编辑器进行像素对照。
- 基线在 `2026-09-29T15:05:51.604Z` 冻结。本次对照于北京时间 2026-09-30 07:37:56–07:37:59 完成。
- 基线 npm 包 SHA-256 为 `a4256b70c42232f3274b8d9ed8da7afdeb233e7754995a88a55a322a488af49c`。原报告字段 `baselineSha256` 指的是这个包，不是 worker 文件。实际加载的基线 worker SHA-256 为 `9384afcaa8e36c60fb4f4313cfc58d7dbdcb6332e98df5e3e5b95f1b7fcc2c61`。
- 测试场景是一张本地图片、系统字体中英文标题、图片层缩放 0.65，以及 blur radius 12、glow radius 16 / strength 0.3、warm LUT amount 0.7。两边项目、素材 URL、输出尺寸、字体和参数相同。
- 每组新建 worker，先渲染一帧，再重复相同画面 40 次，随后只改变位置 40 次。共四轮，交换 baseline/current 的执行顺序。计时来自 renderer 的 `frameMs`；另外记录主线程发送到 worker 回包的往返时间。
- 两边均渲染到独立的 `OffscreenCanvas(1280, 720)`。结束后从 worker 读回 PNG，在主线程解码为 RGBA 比较；PNG 读回、展示和资源销毁都在计时之外。等待 renderer 清理、GPU queue 完成和测试 device 销毁后，才终止 worker 并开始下一组。

“冷帧”是新 renderer 的第一帧，**不包括初始化前等待 ready 的时间**，也不代表操作系统、HTTP、字体或浏览器 shader 缓存全冷。`frameMs` 是浏览器观测耗时，不是 GPU timestamp 查询；往返时间不包括显示器实际呈现延迟。

## 固定画质 A/B 结果

单位为 ms；每个移动中位数来自 40 个样本。

| 轮次 | 基线移动 median | 当前移动 median | 配对减少 | 基线重复画面往返 median | 当前重复画面往返 median |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0 | 2.8 | 0.8 | 71.4% | 3.4 | 0.1 |
| 1 | 2.7 | 0.9 | 66.7% | 3.6 | 0.1 |
| 2 | 2.4 | 0.7 | 70.8% | 2.8 | 0.1 |
| 3 | 2.9 | 0.8 | 72.4% | 2.8 | 0.1 |

基线移动 P95 为 2.6–4.8 ms，当前为 1.6–2.0 ms。当前重复画面的 renderer median 原始读数为 0 ms，P95 为约 0.1 ms；这受计时精度影响，**不能表述为零成本**。相应往返 median 仍约 0.1 ms。

第一帧基线为 67.0–83.6 ms，当前为 64.7–80.2 ms，范围重叠，不宣称冷帧必然更快。每轮都执行像素阈值检查；最终保存的比较为 `meanByteError: 0`、`maxByteError: 0`，两边非暗 RGB 通道计数均为 819477，避免空画面被当成一致。

当前移动结束时观察到 `uploadCount: 0`、`effectHits: 7`、`textLayoutCount: 0`、`graphBuilds: 1`，只执行 4 个 GPU pass；它复用了输入、效果中间结果和文字布局，仅重算位置影响的合成节点。另一个缓存专项测试把热缓存移动后的结果与新 renderer 的完整计算比较，差异字节数也为 0。这里只把计数用作收益原因的证据，不把 pass 数直接换算成全项目加速比。

### 早期读回失败如何处理

早期测试把 HTMLCanvasElement 转移给 worker，再从主线程的展示画布读回；快速连续移动后曾得到非零差异。等待两个 animation frame 仍不可靠。从 worker 读回这个有 HTML placeholder 的画布时，又在反复创建 worker 的轮次出现 `NotReadableError: Readback of the source image has failed`；增加有 ACK 的资源清理仍出现一次失败。

这些失败记录没有作为性能结论使用。保留在 `.local/architecture-qa/render-results/fixed-quality-ab-initial-failed.json`、`fixed-quality-ab-initial.png` 和 `fixed-quality-ab-transferred-surface-failed-1790725002151.json`。单步隔离 12 个案例均逐字节一致；最终只修改测试的画布所有权和读回方式，使用不绑定 HTML placeholder 的独立 OffscreenCanvas，完整四轮通过。可以确认测试读回路径影响了早期观察，不能仅凭这些结果断言某个具体浏览器内部回收机制，也没有因此修改生产 shader 或放宽像素阈值。

## 浏览器正确性与资源专项

以下原始报告均在机器可读归档的 `reports` 中，测试不是仅加载模板 manifest。

| 组别 | 结果 | 实际覆盖 |
| --- | --- | --- |
| `webgpu40` | 40 / 40 | 3 个效果、4 个转场、6 个文字 recipe、6 种 blend 的预览与真实导出后解码比较；缓存、预算、字体失败重试、取消、过期帧、device loss / recovery |
| `canvas2d5` | 5 / 5 | 注入 WebGPU 不可用，基本视频 / 图片 / 系统文字、变换和混合绘制；真实浏览器 WebM 导出；明确拒绝效果、转场和复杂文字模板 |
| `isolatedParity12` | 12 / 12 | 无效果、blur、glow、LUT、blur+glow、三者叠加，各检查两个位置；每个案例 mean / max RGBA 字节差均为 0 |

Canvas2D 导出为 18 帧、100266 B，解码 PTS 0.2 秒与同时间预览比较 MAE 2.009 / 255、P99 31。这里含有损视频编码误差，不要求等同原始 RGBA。这个兼容分支的通过不等于 Safari 全矩阵通过，也不承诺 Canvas2D 支持复杂 GPU 模板。

缓存专项进一步验证：只改音量时没有视频上传或 GPU pass；文字移动不重新排版；效果参数仅使相关后续节点失效；真实源身份变化会重新上传。8 MiB 预算下观察到 4 次 LRU 淘汰。强制 512 KiB 预算时，预览从 640×360 降到可容纳的 240×135；恢复预算并暂停后补回 640×360。失败的新尺寸绘制保留上一完整画面，取消也不覆盖上一完整结果。最低活跃工作集仍放不下时返回明确预算错误，不把静默丢层当作成功。

GPU 预算按当前执行上下文共享，包含本实现跟踪的纹理、相关 buffer 与文字 GPU 分配；淘汰只针对未被当前工作集固定的资源。它不是整个浏览器或驱动的物理显存上限，不能包含无法直接测量的 shader / driver / swapchain 内存。不同 worker 的 device / pool 也不是全进程统一预算。自适应仅用于预览，导出始终保留项目尺寸；固定画质 A/B 没有使用上述降清路径。

## 60 秒 1080p 浏览器导出

`hd60` 于北京时间 2026-09-30 00:00:18–00:00:30 完成。项目为 1920×1080、30000/1001 fps、60 秒，使用本地 `long-play.mp4` 和脉冲 FLAC，含两段视频、LUT / glow、wipe 转场、60 秒系统字体 `studio-glow` 模板和淡入淡出音频。输出走纯浏览器编码路径，没有使用本地编码后备。

- MP4 实际写出 1799 帧，65328459 B，导出调用到 receipt 的实测耗时 **11.706 秒**；这不是从打开应用起算的时间，也不是所有 1080p 素材的保证。
- 重新导入成品后，在约 1、15、29.9、30.1、45、59.9 秒取六个真实解码帧，用各帧 actual PTS 重新渲染项目并比较 RGB。
- 六次 MAE 分别为 **0.875、0.854、0.765、0.683、0.608、0.566 / 255**。这是抽样验证，并非 1799 帧全部逐像素检查。
- 六个预览样本中最大的 `resourceBytes` 为 **189931524 B（181.1 MiB）**，预算 512 MiB；这是跟踪资源的观测值，不是整个导出阶段或整机 GPU 物理内存峰值。

本页关注图像合成和导出画面，音频起点、编码填充和重导入同步见其他音频验收记录，不能用这六帧图像比较替代音画同步测试。glow / soft glow 与桌面的封闭 Core Image CIBloom 算法只保持参数意义对应，当前仍是跨平台近似，没有桌面像素等价承诺。

## 复现入口与边界

使用授权素材目录、现有 Node API 和 Vite 同源代理；本次 Vite 为 5189，报告服务由 `node tests/render-report-server.mjs` 监听 127.0.0.1:4332。素材准备使用已有开发机工具，不增加产品下载依赖。

1. 固定画质对照：`/tests/browser-performance.html`，点击运行。需保留 `.local/architecture-baseline/package` 与 manifest；要求 8 条结果、`phase: complete`、`capture: detached-worker-offscreen-canvas`。
2. WebGPU：`/tests/browser-render.html`；Canvas2D：`/tests/browser-render-fallback.html`；效果隔离：`/tests/browser-render-parity.html`。
3. 高清导出：`/tests/browser-export-long.html`，先准备 `long-play.mp4` 与 `hour-pulses.flac`，输出目录需存在。

GPU 测试串行运行，不和其他渲染验收争用 GPU；测试期间不要修改所加载源文件。本次 QA Vite 关闭 HMR，产品配置未改变。报告服务器的 `browser-report.json` 会被后续页面覆盖，因此结论使用 complete 归档和本文保存的副本。文档写入时未改动已验证的产品源码。
