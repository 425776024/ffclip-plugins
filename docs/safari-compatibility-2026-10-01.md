# Safari 27 兼容性实测与 0.2.2 修复（2026-10-01）

本轮使用本机真实 **Safari 27.0 / macOS 27 arm64 / Node 23.9.0**，从已安装插件缓存启动独立 MCP 服务。未用 Chromium 模拟 Safari。测试只操作隔离工程副本，保留用户原工程和 Safari 起始页。

## 发现并修复

| 问题 | 修复 | 实际验证 |
| --- | --- | --- |
| AAC 校准失败：`InternalAudioDecoderCocoa decoding failed` | 将 Safari 返回的 ES 描述封装提取为 AudioSpecificConfig，用于校准和 MP4 音频封装；严格检查描述长度 | 实际输出 39 字节描述中提取 `11 90`；修复后测得延迟 2112 个采样点，相关度 0.923。见 `packages/render/aac-config.mjs`、`audio-encoder.ts` 和 `tests/aac-config.test.mjs` |
| H.264 导出停在前几帧，无错误回调 | Safari AVC 使用 realtime 编码；其他浏览器及 WebM 保留 quality。导出完成时检查实际编码帧数，缺帧直接失败 | 原先完整工程停在 6/240，独立样例停在第 8 帧；修复后两者均完成。上游有[同版本复现记录](https://github.com/Vanilagy/mediabunny/issues/541)，WebKit 也记录了[编码队列容量修复](https://bugs.webkit.org/show_bug.cgi?id=324943)；未据此假定本机 Safari 已包含上游修复 |
| AAC 丢失位于输入第一个采样点的短促声音 | Safari AAC 输入先加入 4096 个静音采样点，在 MP4 edit list 中与实际编码延迟一起移除 | 首、中、尾五个标记全部保留，独立解码互相关位置误差均为 0 个采样点，相关度 0.893–0.910 |
| 连续导出的下一次请求被忽略 | 将渲染请求排队，等待上一工作线程和 abort 状态清理完成；会话变化时跳过旧请求 | 新增两项针对延迟 finish 响应和失败后继续的队列回归；实际安装版连续导出记录见下方 |

AAC 标准接口描述应是 AudioSpecificConfig；本机 Safari 的编码器返回了外层 ES 描述。归一化不依赖 UA，原本的直接配置保持原样。Safari 的 AVC 模式和 AAC 首音保护仅针对 Safari UA。上游 [AudioDecoder 实现](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/platform/audio/cocoa/AudioDecoderCocoa.cpp)也说明了其接受配置的封装处理。

## 操作与运行证据

| 验证项 | 观察 |
| --- | --- |
| 打开与预览 | 1920×1080、29.97 fps、8 秒的五轨混合工程正常显示视频、图片、自定义花字和透明 HTML；Safari 报告 WebGPU、WebCodecs、OffscreenCanvas 可用，实际 renderer backend 为 webgpu |
| HTML 参数 | 在 Safari 属性面板改为“Safari兼容性验证”，应用后画面更新；撤销恢复旧值，重做恢复新值；完整保存重开保留变量 |
| 剪辑 | 在 0.7 秒分割视频，源区间和时间轴区间均连续；撤销、重做、再撤销回到原两段视频 |
| 图片拖动 | 在透明 HTML 覆盖之下，从时间轴选中图片并实际拖动；X 从 360 改为 461.296，Y 从 -185 改为 -124.219；撤销恢复。见[拖动后的工程记录](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/drag-session.json) |
| 双向播放头 | 实际拖到 6.8068 秒，再回到 2.4024 秒；HTML/视频重新取对应源帧，无 renderer/media 错误 |
| 连续播放 | 命令序号确认已被 Safari 处理；时间从 0.625 推进到 5.143 秒，帧计数从 184 到 288，跨入第二段变速视频，最终自动停在 8 秒；不是只看播放按钮。见[状态采样](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/final-playback-summary.json) |
| 保存重开 | 实际点击 Safari 的“保存完整作品”和“打开作品”，新会话保留所有六个片段、HTML 变量、自定义花字配方和字体样式；最终工程见下方 |
| 配音 | 使用现有已验证模型缓存，Safari auto 实际选择 WebGPU，生成 24 kHz、单声道、2.35 秒 WAV；56,400 个采样中 36,709 个非零，RMS 0.0336。见[采样验证](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/tts-validation.json)；未将波形等同于主观听音或可懂度验收 |
| 音频同步 | 3 秒、90 帧的独立 AAC 样例，有 0、0.25、1.25、2.25 和 2.958 秒五个非周期声音标记；独立 FFmpeg 解码后位置均精确对齐。见[同步验证](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/audio-sync/validation.json) |

测试服务只连接一个实际 Safari 客户端，并将可选 FFmpeg 路径设为不存在，所有成功视频任务均报告 `encoding: browser`。本机已有 FFmpeg/FFprobe 仅用于独立读回。HTML 内容仍按 SKILL 契约由本地 HTML 服务使用已安装 Chrome 渲染；Safari 负责编辑、预览合成及视频编码，这不证明 Safari 原生执行任意作者 HTML。

本轮先在 0.2.0 复现，验证 0.2.1 音视频修复，最后在 0.2.2 实际安装缓存追加连续导出复验；没有把所有手工动作描述为在每个中间版本都重复执行。验收用合成时间码媒体；花字验收副本显式设置 layoutWidth=960、template.style.fontSize=80，检查模板字号编辑及持久化。

![Safari 花字预览](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/final-safari-flower.png)

## 最终安装版连续导出

在实际安装的 0.2.2 缓存上，无间隔地提交 MP4 → WebM → MP4。三个任务均由同一个 Safari 客户端完成，FFmpeg 回退保持禁用；独立 FFprobe 数帧和 FFmpeg 全量解码全部通过，解码退出码为 0、stderr 为空。

| 顺序 | 编码 | 实际帧数 | 分辨率 / 帧率 | 容器时长 | 文件 |
| --- | --- | --- | --- | --- | --- |
| 1 | H.264 / AAC | 240 | 1920×1080 / 30000÷1001 | 8.000 秒 | [MP4](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/output/完整工程修复验收-1790838091703-5307d8.mp4) |
| 2 | VP9 / Opus | 240 | 1920×1080 / 30000÷1001 | 8.008 秒 | [WebM](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/output/完整工程修复验收-1790838166647-f749c0.webm) |
| 3 | H.264 / AAC | 240 | 1920×1080 / 30000÷1001 | 8.000 秒 | [MP4](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/output/完整工程修复验收-1790838249523-c079e0.mp4) |

[最终读回记录](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/delivered-render-validation.json)包含三个 jobId、编码方式和文件大小。[完整可编辑工程](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/output/完整工程修复验收-1790837706667-2fc1c9.vcutweb)保留花字、HTML 参数和混合时间轴。

最终 Safari 页面保留在 3.6 秒的 HTML 动画预览，属性面板标题与画面一致，显示最后一次导出成功：

![实际安装 0.2.2 的 Safari 最终状态](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/delivered-safari.png)

## 构建与分发

- 最终构建通过，根测试 **260 项**、文字运行时 **21 项**通过，0 失败、0 跳过。日志：[根测试](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/delivered-unit-test.log)、[文字测试](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/text-tests.log)。
- 最终版本 **0.2.2** 已更新到个人插件及实际安装缓存，MCP 初始化报告 0.2.2 并发现 38 个工具。当前聊天的既有工具表不会随磁盘更新自动重建；证据来自实际缓存启动的真实 stdio MCP。
- [npm 包](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/output/videocut-local-0.2.2.tgz)有 136 个分发文件；[独立插件目录](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/ready/videocut-local)携带 macOS arm64 原生桥。未发布 npm 或推送 Git。
- tarball 已在独立消费者目录通过 `--offline --ignore-scripts --omit=dev` 安装并通过 CLI、API、完整保存、搬移重启、冻结资源和浏览器必需检查。见[离线 smoke](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/installed-smoke-final.json)。
- 实际安装缓存与 npm 消费者的 127 个 runtime 文件均与最终构建摘要一致。见[文件校验](/Users/jxinfa/WebstormProjects/videocut/.local/safari-compat-20261001/evidence/package-hashes-final.json)。tarball SHA256：`55cbdceddea57564e923a7318bd27cf597853d451cb5fe580d075fcdc31ff147`。

## 验证边界

只覆盖本机 Safari 27；旧版 Safari、iOS/iPadOS Safari、无 WebGPU 设备、Safari 的 4K/长片压力及全字体/素材编码组合仍未实测。本轮没有独立复验 Safari ASR/FastVLM 推理、全部音色与语言或主观扬声器听音。原生 .vcut 不支持 HTML 的格式契约保持不变，完整工程使用 .vcutweb。

AAC 样例的 MP4 流时长均为 3 秒，原始 PCM 解码可暴露末包 320 个额外采样点；现有 edit list 定义播放终点，内部标记没有漂移。WebM 仍有毫秒时间精度及末包 padding 的既有边界，没有声称 sample-exact gapless。
