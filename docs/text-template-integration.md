# 正式编辑器文字模板集成 · 2026-09-29

## 交付范围

正式入口 `http://127.0.0.1:4318/`，文字面板默认显示 6 个实际渲染的模板缩略图。

- 模型：`TextContent.template = { id, version: 1 }`；白名单验证、最多 100 字符，JSON、分割、撤销重做均保留模板身份。
- 渲染：共享 WASM 引擎和字体；缩略图串行生成后释放渲染器；活跃时间轴片段独立渲染，合并过期请求，不积累播放任务。
- 时钟：`(timelineTime - placement.begin + source.begin) / 120000`；分割后继续使用源局部时间。超过模板原有时长时停在末帧。
- 资源：9 个原生模板包组合成 6 个预设。正式构建只加入所选包的资源闭包、2 个字体和依赖许可证。Vite 的散列 WASM 文件只打包一份；开发服务器与正式服务均可直接访问资源。
- 4 个模板使用 WASM / Canvas 2D；2 个图案模板使用浏览器透明视频解码和 WebGPU 后处理。不需要 VideoCut 桌面进程。

## 必须保留的原生几何语义

对照 `/Users/jxinfa/CLionProjects/videoCut` 当前源码：

- `sdk/text_composition/include/videocut/text_composition/TextCompositionSnapshot.h`：`TextVisualExtentPlan` 明确把稳定的控制几何和渲染范围分开，控制几何不能作为像素裁剪。
- `sdk/text/include/videocut/text/TextLayout.h`：`authoredControlBounds` / `controlBounds` 是编辑操作几何，动画后的 `letterBounds`、`visualExtent` 独立。
- `sdk/text_composition/src/TextCompositionSnapshot.cpp`：材料、背景、动画 envelope、装饰都参与可见范围计算，同时尊重模板 authored `visual_extent` 策略。
- `sdk/skia_runtime/src/text/SkiaTextRenderLane.cpp`：控制框由中性的 authored appearance 计算，逐字动画和后处理 padding 不改变操作框。

WASM 0.4.1 新增返回 `controlBounds`、`visualExtent`，均为输出像素坐标；保留原有 cropped RGBA + `originX/Y` 契约。网页操作框读取 `controlBounds`，不再扫描最终像素反推。渲染 Canvas 独立铺满作品画布，CPU 按 origin 写入，GPU 在输出画布上合成装饰和后处理。只有作品最终画布边缘裁切；操作框不参与裁剪、滤镜区域或纹理尺寸的计算。

## 验证

- 正式应用 `npm run build` 通过。
- 主应用 18 项测试通过：含 JSON 与分割保真、无效模板/版本拒绝、完整资源闭包、实际 npm 隔离安装及 WASM MIME/字体/模板资源读取、原有基础编辑与原生导出回归。
- WASM 15 项测试通过。新增几何测试：`anim-lua-letter-transform`，文字“测试”，640×360，0.35 秒与 1.8 秒控制框一致；后一个采样有 10,819 个 alpha > 20 的像素在控制框之外，确认没有按操作框裁剪。
- 正式页面实测：6 个缩略图加载；添加柔光模板，文字从“绽放”改为“星光”；时间轴播放完成多帧渲染、结束时隐藏；seek、添加第二个纹理花字、撤销/重做；通过同一会话链接恢复模板与改字；16:9 和 9:16 画布显示正常。
- 实测纹理动画飞出操作框并在画布边缘正常裁切。截图：`.local/qa/text-control-overflow.png`。
- 编辑会话 JSON 数据往返已通过模型测试；页面下载按钮已触发，但内置浏览器自动化未返回下载文件，本次不将浏览器文件下载/重新上传列为通过。

## 明确边界

当前预览最长边 640 像素，并非最终分辨率输出。上一轮单模板性能结果不能等同于多模板同时播放的整帧预算；本次未重新做完整性能分位数基准。

MP4 与 `.vcut` 已接通，详细路径与验证记录见下节。这里只接入已移植的 6 个模板，不代表全部 Metal/SDF、全屏装饰拓扑或 native GPU render-group 的等价移植。模板级整体变换目前由网页合成层承担；极端缩放、出画后回入的边界行为和最终分辨率效果仍需专项原生对照。

## 复杂模板导出接通

### MP4

- `src/editor/template-export.ts` 复用生产预览的 `createTemplatePlayer`，独立创建作品分辨率的渲染器。时间按输出帧率和片段源偏移计算，不依赖播放帧率，不放大 640 像素预览。
- `packages/server/template-export.mjs` 固定作品版本，通过 SSE 将任务交给当前作品的网页。多窗口只允许一个领取；按片段、帧序上传 RGBA，校验长度和顺序，等待编码器写入后再收下一帧。WebGPU 读回数据解除预乘后再提交。
- FFmpeg 先将透明文字写入临时 FFV1 / BGRA 文件，再按时间轴应用位置、缩放、旋转和透明度，合成原视频与音频，生成 H.264 / AAC MP4。操作框不参与裁剪。
- 完成后才发布最终文件。取消、缺帧、页面无响应或渲染失败会结束任务并清理中间文件。文字生成阶段可取消，最终 FFmpeg 合成阶段等待完成。MP4 无需 VideoCut 桌面进程或原生格式桥接器。
- Node / MCP 导出复杂模板时，也需要打开同一会话的网页；无页面时立即给出明确错误。两个 GPU 预设需要 WebGPU。画布限制：单边 ≤4096，总像素 ≤8388608。

### 可编辑 .vcut

- `packages/server/native-templates.mjs` 与预览共用配方，完整重写字体、纹理和嵌套透明视频的资源引用；资源按 SHA-256 去重，写入 `assets/text/`，采用 SDK `ManagedProjectCopy` 定位。
- `native/videocut_bridge.cpp` 使用原生模板解析、动画 IR 解码、内容绑定、文档验证和发布机制，保留材质、逐字动画、装饰、后处理、片段源范围及整体变换。原生文字仍然可编辑。
- 原生格式桥接器增加 `capabilities` 校验；旧桥接器会提示更新。依赖的字体及媒体随包保存，许可证一并携带。原有用户导入的视频/音频仍遵循外部素材引用规则。
- 在临时目录中完成原生发布与重开校验后才复制到最终目录；`web-session.json` 保存导出时的固定版本。

### 本轮实际验证

- 正式页面导出 `studio-glow`：1920×1080、30 fps、5 秒、150 帧，FFmpeg 全片解码无错误，1.5 秒截图确认汉字、金色光带及柔光存在。结果见 `.local/qa/text-export/`。
- 六模板串接导出：640×360、30 fps、3 秒、90 帧，包含纯色底视频和 AAC 音轨；每个文字片段从源 1.5 秒开始，其中纹理模板额外叠加位移、缩放、旋转和透明度。全片解码通过，六个采样确认图案、发光、径向模糊、飞出画布、翻转和乱码动画均存在。本机这次端到端约 5.3 秒，仅代表这一短片和分辨率。
- 六模板 `.vcut` 使用真实桌面格式库保存、重开；15 个依赖资源逐个验证内容摘要和大小，复制到新目录并删除原测试目录后再次验证通过。检查包含文字内容、模板身份、动画层、装饰和后处理。
- 自动回归覆盖：全局帧对齐、裁剪后的源时钟、透明 alpha 与片段透明度、导出版本冻结、双窗口领取、重复/乱序帧、损坏数据、取消与临时文件清理、资源搬迁后重开。实际浏览器导出验证独立于这些协议测试。
- 最终版本在正式页面再次导出六模板 1080p 短片：1920×1080、30 fps、3 秒、90 帧、H.264 / AAC。逐帧解码后，每个模板的 15 帧都有 15 个不同的像素摘要；音轨 PCM RMS 非零。取消按钮已在真实页面验证，取消后能再次成功导出。核验数据见 `.local/qa/text-export/matrix-hd-verification.json`，成片截图见 `.local/qa/text-export/six-templates-1080p.png`。
- 本轮 WASM 15 项回归同样通过。
- 尚未执行 Flutter 桌面 GUI 对本轮 `.vcut` 的视觉验收；原生重开校验不等同于桌面渲染逐像素一致。未扩展到当前六模板以外的效果。
