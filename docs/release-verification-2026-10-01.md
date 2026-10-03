# 2026-10-01 发布前验收

> 本文保留 0.1.0 首轮验收记录。后续“全部修复”的 0.2.0 结果见 [发布修复与复验](release-hardening-2026-10-01.md)，其中已更新完整工程保存、原生自定义花字、英文配音分发、实际插件安装及长片/4K 验证结论。

本轮从 Codex 已安装 VideoCut Local 插件的使用入口开始，读取并执行视频剪辑与动效模板两个 SKILL，通过实际 MCP 工具创建工程、导入素材、创作 HTML/GSAP 动画与花字，再在 Codex 浏览器中编辑、播放、导出和读回成片。发现的两处运行错误已修复，并对修复后的独立插件构建重新进行了实际验证。

**结论：本机已测的核心创作、剪辑、预览、渲染和原生花字工程往返链路通过；暂不作“可以直接对外发布”的结论。** 仓库的 TTS 分发记录仍明确声明外部分发所需的引擎对应源码与许可确认未完成。HTML/自定义花字的原生保存限制，以及本轮未覆盖的环境见下文。

## 环境与安装来源

- 工作区：`/Users/jxinfa/WebstormProjects/videocut`；macOS 27.0 / arm64；Node v23.9.0。
- 实際浏览器：Codex In-app Browser；WebGPU、WebCodecs、OffscreenCanvas 可用；本机 HTML 渲染器可用。
- 首轮使用本对话已安装插件的真实 `mcp__videocut__*` 工具。安装缓存为 `videocut-local/0.1.0+codex.20261001024056`，读取其 `video-editing/SKILL.md`、`motion-templates/SKILL.md` 和 HTML 导入说明，未直接改写会话文件来替代工具操作。
- 修复后的最终插件构建位于 `.local/plugin-e2e/release-20261001/final/videocut-local`，构建时间 `2026-10-01T03:14:16.917Z`，包含本机原生桥。通过真实 stdio MCP 子进程调用最终包，再由 Codex 浏览器实际预览、编辑和编码。全部 118 个 runtime 文件的哈希与当前 `dist` 一致。
- 最终 npm tarball 为 `videocut-local@0.1.0`，126 个发布文件，压缩大小 19,619,439 字节；在隔离目录离线安装后，CLI 初始化、25 个 MCP 工具发现和创建会话成功。
- **本对话原有安装缓存没有被热替换。** 最终修复包已验证，但发布/更新时还需使用户安装到新构建，并在新 Codex 对话中使用更新后的工具。版本号本轮仍为 0.1.0；未发布 npm、上传插件、提交或推送 Git。

## 自动化与打包检查

| 检查 | 最终结果 | 证据 |
| --- | --- | --- |
| `npm test`，包括构建、Vue/TypeScript 检查和根目录测试 | 204 通过，0 失败，0 跳过 | [最终根测试日志](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/npm-test-after-cancel-fix.log) |
| `npm test --prefix packages/text-wasm` | 21 通过，0 失败，0 跳过 | [最终 WASM 测试日志](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/text-wasm-test-final.log) |
| `npm run check:package` | 通过发布白名单检查 | [打包检查](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/check-package-final.log) |
| npm pack、隔离离线安装和真实 CLI/MCP 启动 | 通过 | [包清单](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/npm-pack-final.json)、[安装日志](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/npm-clean-install.log)、[MCP 记录](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/npm-clean-smoke/mcp-transcript.jsonl) |
| 最终插件 runtime 与源码快照一致性 | 118 个 runtime 文件一致；本轮修改的 5 个源文件与打包快照一致 | [runtime 哈希](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/final-runtime-validation.json)、[源码检查](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/source-final-validation.json) |

总计 **225 项自动化测试通过，无跳过**。这些数量不包含下面的人工浏览器操作和导出解码检查。

首轮根测试已通过 204 项；额外执行 WASM 测试时发现 3 项失败，分别涉及过时的合成测试模板 ID、错误地要求不支持的模板接受叠加组件，以及两处既有 vendored C++ 修改未更新来源哈希。已按当前支持契约修正测试并保留拒绝断言，补全来源记录，没有跳过或删除失败用例。过程中一次重叠构建导致临时打包检查失败；后续顺序执行的最终根测试为上表中的全绿结果。

## 实际插件与浏览器验收

| 功能 | 实际执行和观察 | 结果 |
| --- | --- | --- |
| 素材发现和导入 | 通过 `list_files`、`add_media` 导入 12 秒 H.264 视频、WAV、PNG；另生成带音轨的 AV 素材测试关联操作 | 通过 |
| HTML 动画创作 | 编写透明 HTML、相对 CSS/JS 资源、暂停的 GSAP 时间线与绝对时间 tick；`add_html_clip` 冻结资源并加入普通视频轨道 | 通过 |
| HTML 编辑与回放 | 在属性面板把变量标题改为“最终版本验证”，应用、撤销、重做后用 MCP 读回；2.8→6.8→2.8 秒逆向定位的画布截图完全相同 | 通过 |
| 自定义花字 | 通过工具创建四字“发布验收”，组合 `flower-style-38`、`bubble-nine-slice`、`anim-lua-letter-transform`；观察入场字形、纹理、背景和逐字动画，随混合视频一起导出 | 通过 |
| 六款内置花字 | studio-glow、studio-radial、pattern-flower、layered-flower、cube、printer；编辑中文内容，逐段实际渲染并检查抽取帧 | 通过 |
| 剪辑和历史 | 裁剪、源入点、分割、1.5 倍速、AV 关联、分组、复制、波纹删除、撤销/重做；复制和波纹删除撤销后结构精确恢复 | 通过 |
| 实际拖动 | 透过透明 HTML 覆盖层拖动已选图片，只改变该图片的 X/Y；拖动 HTML 时间轴片段 1 秒，源范围保持不变；各一次撤销恢复 | 通过 |
| 播放 | 修复后快速 seek→play；读回匹配命令序号和工程版本，时间 0.9227→1.6747→2.4267 秒、解码帧数 22→30→48，playing=true，error 为空 | 通过 |
| 特效和转场 | 五镜头分别模糊、柔光、暖色/冷色/电影调色；四个剪切点分别叠化、淡色、擦除、推移；检查预览和成片中点帧 | 通过 |
| 关键帧与音频 | 图片位置、旋转、不透明度关键帧；4 秒读回 X=0、旋转 180°、不透明度 0.7；音频增益 0.2，0.4 秒淡入淡出；输出 PCM 采样验证首尾 RMS 下降 | 通过 |
| 视频导出 | 真实浏览器编码 MP4/H.264/AAC、WebM/VP9/Opus；最终混合工程 1920×1080、30000/1001 fps、240 帧；独立 ffprobe 与完整 ffmpeg 解码 | 通过 |
| 取消导出 | 从 UI 启动正在编码的任务，在第 41/240 帧通过 MCP 取消；UI 显示“已取消导出”，控件恢复，输出目录无临时文件 | 通过 |
| 原生工程 | 六款内置花字加视频/音频写出 `.vcut`，由 `open_project` 新建会话重新打开；8 个片段的内容、时间、变换、模板、画布和帧率一致，重开后再渲染 360 帧 MP4 | 通过 |
| 本地 TTS | 已安装的 fp32 模型，中文 zf_001 和英文 af_maple 实际使用 WebGPU；中文强制 WASM；验证生成 WAV、插入/不插入、队列取消，并把中文配音导出到视频 | 通过；未听音 |
| 异常处理 | 过时版本 409、非法批量操作完整回滚、超出源范围裁剪、锁定轨道、未授权目录、不支持的花字组合、HTML 嵌入 audio/video、无浏览器导出均明确拒绝 | 通过 |

原生往返比较对 JSON 键排序，并仅忽略保存时新增的模板 `packageDigest`。自定义逐字动画按原生程序的时间演进采样，部分后段字形会移出背景，不把单个后段帧当作静态标题效果。

实际素材为可追踪的合成测试图、源时间码、声音和图标；没有替换用户作品。最终混合工程中的变速视频在 7.2 秒结束，独立音频持续到 8 秒，末尾黑场来自测试工程的安排。

关键证据：[最终 MCP 全记录](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/final-process/mcp-transcript.jsonl)、[操作与异常](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/transactions.json)、[预览拖动](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/preview-drag-validation.json)、[时间轴拖动](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/timeline-drag-validation.json)、[HTML 逆向定位](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/html-rewind-validation.json)、[原生往返](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/native-roundtrip-final.json)、[特效关键帧属性](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/effects-properties.json)、[音频淡入淡出](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/effects-audio-validation.json)。

## 运行问题与修复

1. **快速定位后播放被取消的音频任务打断。** 安装缓存与新构建都复现了 `Media work was cancelled` 并停播。[Preview.vue](/Users/jxinfa/WebstormProjects/videocut/src/editor/Preview.vue:71) 现在忽略预期的 `AbortError`，实际解码/播放错误仍上报。最终包的真实播放状态确认时间和帧数持续推进。
2. **MCP 取消编码后 UI 显示错误的“导出任务已结束”。** [Editor.vue](/Users/jxinfa/WebstormProjects/videocut/src/editor/Editor.vue:261) 统一显示正常取消结果，接收取消进度时同时停止浏览器编码器。实际正在运行的导出取消后显示“已取消导出”，没有临时输出残留。

另外修改 [backdrop-recipes.test.mjs](/Users/jxinfa/WebstormProjects/videocut/packages/text-wasm/tests/backdrop-recipes.test.mjs)、[SOURCE.json](/Users/jxinfa/WebstormProjects/videocut/packages/text-wasm/vendor/SOURCE.json) 和 [NOTICE.md](/Users/jxinfa/WebstormProjects/videocut/packages/text-wasm/NOTICE.md)。来源记录披露两项本轮开始前已存在的 C++ 优化；本轮没有修改这些 C++ 文件。

## 可复查产物

| 产物 | 读回结果 |
| --- | --- |
| [最终 HTML + 花字 + 剪辑 MP4](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/output/当前构建_发布验收-1790824804601-d3aac5.mp4) | 1920×1080，30000/1001 fps，240 帧，8.000 秒，H.264/AAC，7,450,270 字节 |
| [同工程 WebM](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/output/当前构建_发布验收-1790824911027-a57c51.webm) | 1920×1080，30000/1001 fps，240 帧，8.008 秒，VP9/Opus，5,180,348 字节 |
| [六款花字 MP4](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/output/六款花字_原生往返验收-1790824119695-8ac90d.mp4) | 960×540，30 fps，360 帧，12 秒；来自重开的原生工程 |
| [六款花字 .vcut 目录](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/output/六款花字_原生往返验收-1790823975783-9c9f3c.vcut) | 原生桥写入、验证、重新打开通过 |
| [特效、转场和关键帧 MP4](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/output/特效转场关键帧-发布验收-1790825359816-968940.mp4) | 最终版本 3；960×540，30 fps，240 帧，8 秒，H.264/AAC |
| [中文配音 MP4](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/output/本地配音_发布验收-1790824759043-747974.mp4) | 1920×1080，30 fps，113 帧，AAC 48 kHz 双声道，容器 3.75 秒；视频/音频时长差小于一帧 |
| [最终 npm 安装包](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/output/videocut-local-0.1.0.tgz) | 实际隔离离线安装与 CLI/MCP smoke 通过；尚未发布 |
| [最终独立插件目录](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/final/videocut-local) | 最终源码对应构建；包含 macOS arm64 原生桥 |

这些成片均完成独立全帧解码，错误输出为空。完整视频读回记录见 [video-validation-final.json](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/video-validation-final.json)。TTS WAV 分别为中文 3.75 秒、英文 4.025 秒、WASM 中文 1.1 秒，24 kHz 单声道，峰值和 RMS 非零；这是音频数据证据，不代表发音质量或扬声器听音验收。

保留的 [实时混合工程预览](http://127.0.0.1:56541/projects/当前构建-发布验收) 依赖本轮本地 MCP 服务继续运行；会话是临时的。动画创作源文件在 `.local/plugin-e2e/release-20261001/title.html`、`title.css`、`title.js`，完整最终会话快照在 `evidence/final-main-session.json`。测试证据和素材都在被忽略的 `.local` 目录，不进入发布包。

## 发布门槛与未覆盖范围

### 尚未解决的发布门槛

[packages/tts/DISTRIBUTION.txt](/Users/jxinfa/WebstormProjects/videocut/packages/tts/DISTRIBUTION.txt:3) 明确记录：当前英文 phonemizer 包含编译后的 eSpeak NG 引擎，外部分发所需的引擎精确来源、Corresponding Source、生成补丁/工具链和组合许可确认仍未完成。最终 tarball 同样携带该记录和相关运行时。本轮没有解决或改写这项既有门槛；运行、构建、打包和生成语音成功不能替代该记录要求的工作。

### 已验证的功能边界

- HTML 动画和自定义花字 recipe 可在临时 Web 会话编辑并导出 MP4/WebM；当前不能保存为原生 `.vcut`，工具会明确拒绝。六款内置模板的原生保存与重开已经通过。若发布承诺“所有工程都能持久化保存”，当前实现不满足该承诺。
- 自定义 backdrop/animation 叠加仅支持 `flower-style-03` 和 `flower-style-38`。cube、studio 等复杂模板必须保留原始执行图，不支持任意组件混搭；拒绝行为已测。
- 原生桥验证范围为本机 macOS arm64。浏览器导出需要保持工程页面打开；无页面时明确返回 `BROWSER_REQUIRED`。

### 本轮没有声称完成的验收

- Windows/Linux、Safari、没有 WebGPU 的真实设备、ChatGPT/其他宿主安装，以及更新后全新 Codex 对话的插件注册与发现。
- 新机器首次模型下载、下载中断/恢复、未安装字体环境和不同语言/罕见字形的全覆盖。
- 扬声器听音、TTS 发音质量、长视频音画同步、人像/实拍色彩质量、4K/长时间压力和所有素材编码组合。
- 系统文件剪贴板完整人工流程，以及每个 UI 快捷键/属性/窄屏布局的逐项人工覆盖。相关根测试通过不等同于这些人工流程都已执行。
- Flutter/native VideoCut 窗口中的排版、播放与导出验收；本轮原生证据为桥接保存、结构重开和浏览器再渲染。

## 截图与成片抽帧

![最终 HTML 动画和剪辑工程](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/final-editor.jpg)

![六款内置花字的实际成片抽帧](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/six-native-templates.jpg)

![实际特效、关键帧和四种转场的成片抽帧](/Users/jxinfa/WebstormProjects/videocut/.local/plugin-e2e/release-20261001/evidence/effects-contact.jpg)
