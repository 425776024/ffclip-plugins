# 复杂图案花字和文字动画试移植 · 2026-09-29

独立包版本 `@videocut/text-wasm` 0.3.0。演示入口：<http://127.0.0.1:4321/demo/complex.html>，在包目录运行 `npm run demo`。运行只使用 WASM、浏览器、WebGPU、模板和字体，不依赖 VideoCut 应用或本地渲染服务。没有修改原始 SDK 仓库和 vendor 快照。

## 六个演示

| 页面名称 | 原包来源 | 移植内容 |
| --- | --- | --- |
| 图案 · 柔光入场 | `anim-studio-time-transform-softglow` | 原包透明 MP4 星光装饰、逐字变换和 Bézier 时序、原生参数采样、五遍柔光后期 |
| 图案 · 径向聚拢 | `anim-studio-dual-selector-radial` | 原包透明 MP4 动态装饰、双选择器、径向模糊 |
| 纹理花字 · 拼贴底板 | `flower-style-38` + `bubble-tile` + `anim-lua-letter-transform` | 七张纹理、平铺底板、原生程序逐字变换；组合适配 |
| 多层花字 · 弹性底板 | `flower-style-03` + `bubble-nine-slice` + `anim-lua-letter-transform` | 纹理填充、双描边、九宫格底板、原生程序逐字变换；组合适配 |
| 空间翻转 | `anim-lua-cube` | 原生程序求值、立方体投影材质 |
| 多阶段乱码打印 | `anim-lua-printer-multistage` | 原生程序、随机字符替换、多阶段显隐 |

四个演示加载原包；另两个在演示层明确组合花字、底板和动画。组合时为底板资源加入独立命名空间，防止不同包中的 `assets/asset-000.png` 互相覆盖。原始 fixtures 不变。

## 实现

- `src/composition_plan.cpp` 复用 `SampleDecorationRenderPlan`、`ResolveDecorationRasterAffine`、`SampleTextAnimationLayerEvaluation`、`ResolveQtSoftGlowContract` / `ResolveQtRadialBlurContract`，导出装饰位置、视频源时间和后期参数。原生逐字动画继续由 WASM 执行。
- `src/browser-composition.mjs` 提供公开 `@videocut/text-wasm/composition` 接口，加载原包的直接 packed-alpha 视频包装、浏览器 H.264 解码和时间定位，最多缓存 8 帧并受 32 MiB 限额约束。释放时清理视频、Blob URL 和 ImageBitmap。
- `src/composition-gpu.mjs` / `composition-shaders.mjs` 合成 alpha-left/color-right 视频、WASM 文字和后期。径向模糊公式源自 `MetalRadialBlur.metal`，采样调度源自 `QtTextRadialBlurMetalRuntimeApple.mm`。柔光执行阈值、降采样、横向模糊、纵向模糊/曝光和 screen 合成。
- `experimentalComposition` 仅接收两套已列出的单装饰/单后期模板结构。需要外部合成的模板不能直接使用 CPU `draw()`，避免背景和后期被静默丢掉。通用 Lottie、任意效果图和其他 Metal 效果不在此范围内。

## 验证与证据

真实浏览器 GPU 读回记录保存在 `complex-validation.json`，六个案例均通过。适配器为 Apple / metal-3，非 fallback。每个案例采样 0.35、1.0、1.8、2.6 秒，检查至少一个画面非空、时间变化、重复跳转确定性和中文改字后的像素变化。两套视频装饰模板另有分别关闭背景、关闭后期的像素差异检查。其余模板的对应字段为 null，不表示已执行这些开关检查。

独立 WASM 测试对两组花字分别删除底板，再验证像素确实改变，并检查资源命名空间。这验证底板实际参与渲染。编译和全部 13 项测试通过，覆盖前版基础文字/SDF 能力及新合成计划、错误拒绝、动画参数和底板。

页面实际操作并查看预览；柔光模板连续播放、暂停后时间改变；七纹理模板在时间轴按 End 后显示 5.00 秒并重新绘制。页面支持修改文字、时间轴、背景/后期开关和四个时间点的缩略图。截图保存为同目录 `complex-*.png`。构建、移植代码、模板 manifest 和 tarball 摘要保存在 `complex-build.json`。

`npm pack` 生成 `videocut-text-wasm-0.3.0.tgz`；隔离 consumer 安装该包后，真实 WASM 中文渲染、SDF 网格导出和新 composition 导入通过。三组公开 API 的 TypeScript strict 检查通过。Node 验证没有模拟浏览器 GPU；GPU 证据来自实际浏览器。

## 性能与一致性边界

下面是 640×360、默认短中文、已加载字体/管线、四个检查时刻的墙钟采样。`totalMs` 从 WASM 文字渲染开始，包含需要的视频取帧及等待 GPU 队列完成；不包含初始化、字体下载、首次管线编译或额外 readPixels。视频检查前生成了同时间的缩略图，因此多数取帧命中缓存，不能作为连续视频解码吞吐或稳定 FPS。

| 模板 | 检查时刻总耗时范围 |
| --- | ---: |
| 柔光入场 | 1.6–3.4 ms |
| 径向聚拢 | 1.7–4.4 ms |
| 七纹理花字 | 131.9–158.7 ms |
| 九宫格花字 | 9.3–16.6 ms |
| 空间翻转 | 55.4–71.9 ms |
| 多阶段乱码打印 | 3.9–6.5 ms |

七纹理和投影路径的主要成本在 WASM CPU 材质。当前复杂模板演示运行在主线程，重模板会阻塞交互；需要 Worker 和进一步 GPU 材质移植才能改善。不能称为所有复杂模板已实时。

文字仍用 WASM 栅格字形；装饰/后期使用输出画布作为合成表面，没有恢复桌面 Page render-group 的完整尺寸、原生逐字 SDF atlas 和材质执行图。浏览器视频解码与原生解码的色彩差异也未量化。因此本次验证是浏览器可播放、可改字和实际图层参与，不是完整桌面模板逐像素一致性验收。

0.2.0 的 Metal 数值对比仅覆盖当时的 SDF/高斯/柔光参数子集，不覆盖本次径向模糊、五遍柔光管线或整套复杂模板。尚未验证其他 GPU/浏览器、长文本、多图层、长时间播放及视频导出。
