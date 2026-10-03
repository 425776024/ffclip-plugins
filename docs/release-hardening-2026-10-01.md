# 0.2.0 发布修复与复验（2026-10-01）

后续真实 Safari 27 实测发现并修复了 AAC 配置/首音、H.264 队列与连续导出问题，最终交付版本更新为 **0.2.2**，见 [Safari 兼容性报告](safari-compatibility-2026-10-01.md)。以下保留 0.2.0 当时的证据和验证范围。

本轮承接 [首轮插件验收](release-verification-2026-10-01.md) 和“全部修复”要求，修复了已确认的保存、原生花字、英文配音分发、浏览器 Worker 初始化和插件版本问题，并补测首次模型安装、中断恢复、长片同步与 4K 导出。**本机已执行的创作、剪辑、保存重开和渲染链路通过；跨平台实机与主观听音仍未完成。** 没有发布 npm、上传 release、提交或推送 Git。

## 已修复

| 问题 | 修复后的行为 | 实现与回归证据 |
| --- | --- | --- |
| HTML 和自定义花字只能留在临时会话 | `save_project` / 编辑器“保存完整作品”写出可整体搬移的 `.vcutweb` **目录**，包含媒体、HTML 源码和变量、配方/样式、效果、关键帧、关联关系及冻结模板资源 | `packages/server/web-project.mjs`、`tests/web-project.test.mjs` |
| 重开依赖原媒体和当前安装的模板 | 媒体按内容摘要去重并校验；模板资源随工程冻结、按会话授权读取。搬移、服务重启、删除原输入和移走安装目录模板后仍可重开、修改 HTML、再次保存 | 同上；隔离安装 [跨平台 smoke 的本机结果](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/platform-smoke-final.json) |
| 自定义花字与改过颜色/字号的花字不能保存原生工程 | 更新桥将配方和样式写入可校验的原生文档来源信息，重建字面填充和字号；旧桥明确提示升级或使用 `.vcutweb` | `native/videocut_bridge.cpp`、`native/import_projection.h`；[实际缓存插件往返比较](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/native-style-roundtrip.json) |
| 英文前端包含来源材料未核齐的编译引擎 | 移除 `phonemizer` 依赖及分发的 eSpeak 引擎，改用 MIT HeadTTS JavaScript 规则和 Apache-2.0 Misaki US/GB 原始词典；记录版本、上游/本地哈希、修改和许可证 | `packages/tts/vendor/ENGLISH-SOURCE.json`、`NOTICE.txt`、`DISTRIBUTION.txt`、`tests/tts-runtime.test.mjs` |
| 大词典可在 Node 初始化、真实 Chromium Worker 却栈溢出 | 将词典编译载荷改为 `JSON.parse(string)`，保留原字典字节及校验；真实 WebGPU / WASM 合成通过 | 四个 WAV 的 [采样验证](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/video-audio-validation.json) |
| 插件版本与 MCP 初始化版本不同步 | npm 和插件升为 0.2.0，MCP 从实际 package.json 读取版本；更新注册的个人插件和 Codex 安装缓存 | `tests/mcp.test.mjs`；[安装缓存 MCP 全记录](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/cache-installed/mcp-transcript.jsonl) |
| SKILL / 文档仍声明旧保存限制 | 更新 HTML authoring reference、motion-templates、README 和 integration 文档，区分完整工程与原生桌面格式 | `plugins/videocut-local/skills/`、`docs/html-clips.md` |

`.vcutweb` 是独立的完整 Web 工程格式，使用 magic/version 和 SHA-256 文件清单；不是给原生 `.vcut` 增加一个丢失 HTML 的旁路文件。保存使用临时目录、独占目的目录和最后提交清单，拒绝覆盖、源素材变化、路径越界、符号链接及摘要不一致。HTML 已冻结的资源和变量不需要原 HTML 文件继续存在。

英文前端使用词典默认读音与未登录词规则回退，不包含上下文词性模型。此修复移除了旧编译引擎对应的技术分发缺口，未重新许可任何上游项目，也不代表对整个产品作法律许可结论。精确来源和全部适用通知均随包保留。

## 最终验证

- 环境：macOS 27 / arm64，Node v23.9.0，Codex In-app Browser，WebGPU / WebCodecs / OffscreenCanvas；现有本机 Chrome 用于隔离 HTML 图形渲染。
- 根目录 `npm test` 的 **254 项**全部通过，包括构建、Vue/TypeScript 检查、原生桥往返、打包/安装和功能回归；最终日志见 [npm-test-complete.log](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/npm-test-complete.log)。文字 WASM 的独立 **21 项**测试全部通过，无跳过，见 [text-wasm-tests.log](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/text-wasm-tests.log)。合计 **275 项，零失败、零跳过**。失败用例均保留，未通过跳过测试达成通过。
- 实际通过 `codex plugin add videocut-local@personal --json` 更新了已注册插件；缓存目录为 `/Users/jxinfa/.codex/plugins/cache/personal/videocut-local/0.2.0`。读取此缓存的 `.mcp.json` 启动真实 stdio 子进程，初始化报告 0.2.0，发现 **38 个工具**，包括 `save_project`，见 [交付版 MCP 记录](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/delivered/mcp-transcript.jsonl)。
- 两个创作 SKILL 和 HTML reference 已更新。当前对话初始化时取得的工具表不会因磁盘更新自动改写；此处证据为实际安装缓存的 MCP 发现和调用，不冒充“已打开全新 Codex 对话并验收”。后续新对话/应用刷新需重新加载工具表。
- 最终 npm tarball 在独立消费者目录以 `--offline --ignore-scripts --omit=dev` 安装；无需源码、开发依赖、原生桥或模型下载即可运行 CLI、API、冲突检查、完整保存、搬移重启和冻结模板读取。
- 发布白名单确认 **136 个 npm 文件**；独立插件的 **127 个 runtime 文件**和原生桥与当前构建一致。安装缓存与独立目录的 **136 个文件哈希相同**，仅分别生成的 BUILD 时间戳不同；最终 **1252 个源码输入**在校验时一致。隔离 npm 安装的 runtime 也与独立插件相同，见 [安装缓存与源码校验](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/installed-package-hashes.json)和 [npm 校验](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/npm-installed-hashes.json)。

## 实际运行与可复查产物

| 验收 | 观察与结果 |
| --- | --- |
| 完整混合工程持久化 | 视频、音频、图片、透明 HTML/GSAP 和自定义花字一起保存、搬移和重启重开；在属性面板将 HTML 标题改为“搬移重开后可编辑”，再次保存重开。最终还实际点击安装版编辑器保存按钮，[作品目录](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/完整工程修复验收-1790828786148-f90298.vcutweb)可直接重开 |
| 实际安装缓存渲染 | [混合 MP4](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/完整工程修复验收-1790830724524-2999ca.mp4)，1920×1080、30000/1001 fps、240 帧、8 秒，H.264/AAC；全部视频与音频独立解码成功，见 [交付版读回](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/delivered-video-validation.json) |
| 原生自定义配方和样式 | `flower-style-38` + `bubble-nine-slice` + `anim-lua-letter-transform`，颜色 `#ff5c70`、字号 180；[原生 .vcut 目录](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/原生花字修复验收-1790829085614-a001ae.vcut)重开后，模型比较通过。比较只规范化导入新增的包摘要及缺省 false 的轨道同步标志；[重开后的 MP4](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/原生花字修复验收-1790829208715-9a1770.mp4)全部 90 帧解码成功 |
| 最新文字框布局 | 实际拖动右边缘 80 屏幕像素，文字框收窄并换行，字号保持 180，源范围和时间轴位置不变；宽度 1052.4887 写入 [原生工程](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/原生花字修复验收-1790830190794-7707ea.vcut)后重开，模型比较通过，[再次渲染的 90 帧 MP4](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/原生花字修复验收-1790830289952-c2084a.mp4)完整解码成功。见 [宽度往返和 TTS runtime 一致性](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/width-tts-final-validation.json) |
| 长片音画同步 | [2 分钟 1080p MP4](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/两分钟音视频同步验收-1790828334971-9b1d79.mp4)，全部 3600 视频帧、5,760,960 PCM 样本解码成功；12 个闪光/声音标记的最大差约 **33.3 ms，小于一帧**，未见累积漂移。两条流均为 120 秒，见 [同步记录](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/long-sync-validation.json) |
| 4K 输出 | 原生 3840×2160 合成测试输入，[4K MP4](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/4K导出验收-1790828442130-2bba82.mp4)为 6 秒、180 帧、30 fps、H.264/AAC，完整解码无错误；未把短片结果等同于数小时压力验收 |
| 全新 TTS 模型安装与恢复 | 隔离空缓存显式下载 fp32 和三个音色，下载模型约 106 MB 时取消；保留已校验文件、删除部分文件。服务重启后继续安装、完整 SHA 校验并复用此持久缓存，见 `restarted-output.log` 和 `evidence/restarted/mcp-transcript.jsonl` |
| 新前端真实语音 | 美国/英国英文 WebGPU，以及中文/美国英文强制 WASM，共四个 WAV；24 kHz 单声道，时长 4.05 / 4.0 / 6.125 / 2.35 秒，全部 PCM 非零。英文再由本地 Whisper 实际识别为预期句子“Welcome to Video Cut. This video is ready to publish.”，这是可懂度数据证据，不是扬声器听音证据 |

首轮已执行的剪辑、速度、分割、AV 关联、拖动、撤销/重做、关键帧、特效、转场、MP4/WebM 和导出取消仍在前一报告保留。本轮对修改影响的持久化、原生花字、TTS、安装注册和编码作了追加实测；没有将首轮所有人工操作描述为在本轮再次执行。

素材是带时间码、声画标记的合成测试媒体，不是用户实拍项目。FFmpeg/FFprobe 仅用本机已有程序生成测试输入、独立读回导出结果；运行时仍是浏览器编码，没有新增、打包或自动安装 FFmpeg。

![安装版完整工程编辑与渲染成功](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/final-installed-ui.png)

![原生自定义花字重开和渲染成功](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/evidence/native-installed-ui.png)

## 发布包与剩余验证边界

- [0.2.0 npm 安装包](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/output/videocut-local-0.2.0.tgz)和[独立插件目录](/Users/jxinfa/WebstormProjects/videocut/.local/release-hardening-20261001/ready/videocut-local)已准备。独立目录携带 macOS arm64 桥；portable npm 不携带平台原生桥。测试模型、视频和证据都在被忽略的 `.local`，不进入分发包。
- 已添加 `scripts/platform-smoke.mjs` 和 `.github/workflows/installed-release.yml`，覆盖现成安装包的 Ubuntu / Windows / macOS × Node 22 / 24 矩阵；**远程矩阵没有运行**，本机实际仅 macOS arm64 / Node 23.9。没有因此声称 Windows/Linux/Safari 或其他宿主全部通过。
- 原生 `.vcut` 不表达 HTML；用户保存完整工程使用 `.vcutweb`，视频导出使用 MP4/WebM。自定义部件组合继续遵循已返回的 nativeComposition，复杂模板保留其原执行图。这些是明确契约，不是静默保存失败。
- 未执行扬声器主观听音、Flutter/native 桌面窗口验收、无 WebGPU 的真实设备、全字体/编码组合、数小时压力、全部 UI 快捷键和窄屏布局、或全新 Codex 对话注册。视觉理解新增工具的模拟/浏览器协议回归纳入根测试，但本轮没有下载其模型并作真实推理验收。
- 发布包对应验证时的源码和文件哈希。工作区中同时进行的文字布局修改已纳入构建和上述实测；后续继续修改源码时需重新构建、验收和打包，不能复用本报告声称新代码也已通过。

[交付版实时预览](http://127.0.0.1:55312/projects/完整工程修复验收)依赖当前本地服务；完整作品已持久保存，服务停止后可用 `open_project` 重新打开。
