# 文字 GPU 效果试移植 · 2026-09-29

已在独立 `@videocut/text-wasm` 0.2.0 中实现三个试验效果，并在浏览器的 Apple WebGPU 硬件后端出图。文字轮廓由复制的 C++ SDK 经 WASM 生成，距离场与效果由 WGSL 在 GPU 执行。运行不依赖 VideoCut 应用或原生渲染进程。

## 本次范围

| 能力 | 复用来源 | 当前实现 |
| --- | --- | --- |
| SDF 距离场、纯色填充和描边 | `BuildTextSdfGpuMesh`、`MetalTextSdf.metal`、`MetalTextSdfMaterial.metal` | 保留抛物线距离算法、RG16 编码、深度取最近距离、非零 winding stencil 和内部反转；材质只取纯色填充/描边子集 |
| 高斯模糊 | `MetalGaussian.metal` | 相同权重、边缘归一化与 X/Y 两遍采样；本 API 固定关闭 gamma 转换、模糊 alpha |
| 柔光 | `MetalQtTextSoftGlow.metal` | RGB 阈值类型 0 → X 模糊 → Y 模糊和曝光 → 染色及 screen 合成；不是 DeepGlow |

原始源码仍原样保存在 `vendor/videocut/sdk/skia_runtime/src/text/shaders/metal/`，逐文件摘要在 `vendor/SOURCE.json`。WGSL 派生代码在 `src/gpu-shaders.mjs`，GPU 资源/管线管理在 `src/webgpu.mjs`。C++ 轮廓和网格导出在 `src/bridge.cpp`。

这是效果算法试移植，不是完整原生花字模板 GPU 后端。`prepareSdfMesh` 只获取模板基础排版，保留 SDK 字体 shaping、字形轮廓与网格算法，整段合成到一个画布距离场。没有重建原生逐字 atlas/material mapping；完整渐变、纹理材质、逐字动画、变换、装饰和效果执行图尚未接入此 GPU API。演示的“强度动画”修改 GPU 参数，并非原生模板的动画回放。原有 CPU 模板动画 API 继续独立工作。

## 与原生实现的有意差异

- 将原生紧密打包的 Y-up 距离网格恢复到画布位置，投影到 WebGPU 坐标；着色器顶点位置采用画布归一化坐标。
- 距离纹理使用 RGBA8 中的 RG 两通道，而完整原生 atlas 使用更紧凑的格式。对比程序也用 RGBA8，避免额外格式差异。
- 对精确解析尖点的除零分支使用极限根 0，并对浮点根号的负舍入值截断到 0。除这两处数值保护外，距离计算沿用原公式。
- 网格输入上限 64 MiB、单边上限 2048；公开模糊半径 0–128，底层循环保留原着色器 1024 的上界。SDF 的可描边范围受 `range` 限制。
- 后处理使用 RGBA8 中间 pass，和本次原生参考程序保持一致。纯色材质采用输出画布距离场和导数抗锯齿，未证明与完整原生材质逐像素一致。

## 已执行验证

1. `npm run build` 和 8 项真实 WASM 自动化测试通过。新增测试验证网格的有限值、确定性、改字后几何变化、拷贝生命周期与非法参数。
2. `npm run test:gpu:reference` 在 Apple M4 Pro 上编译并执行 `tests/gpu/metal-reference.mm`。它直接加载未修改的原生 Metal 文件，使用与 WebGPU 相同的 WASM 网格/预乘 RGBA 输入生成参考。
3. 浏览器点击“对比 Metal 参考”实际提交 GPU、等待完成、读回像素，与参考比较。`reports/gpu-validation.json` 保存硬件信息、源文件/输入/移植模块摘要、像素误差、参数变化、重复渲染确定性和 60 帧计时。
4. 浏览器实际中文改字“花字”→“你好”、柔光切换、播放和暂停均已操作并查看画面；强度动画页面统计约 60 FPS。一次改字网格重建及首次绘制约 56.6 ms，这不是稳定性能承诺。
5. `npm pack` 生成 `videocut-text-wasm-0.2.0.tgz`（约 3.6 MB），在隔离 consumer 中安装后执行真实 WASM 中文渲染和网格导出，确认 `@videocut/text-wasm/webgpu` 导出存在；两套公开接口的 TypeScript strict 检查通过。该 Node smoke 没有模拟 GPU，GPU 证据来自上面的真实浏览器验证。

同输入对比中，高斯与柔光的 RGBA8 最大通道差均为 1，即归一化颜色的 1/255；SDF 解码距离的最大归一化差约 0.00001532。完整数值和最终计时以 JSON 为准。

性能测试在 640×360、60 帧、已预热管线和缓存字形的条件下改变参数，测量 JS 提交到 GPU 队列完成的墙钟时间。它不包含首次着色器编译、WASM 初始化、字体下载或网格重建，也不等于屏幕呈现延迟。页面 FPS 是 requestAnimationFrame 回调统计，不能代替 GPU 计时。

| 效果 | 中位完成耗时 | P95 完成耗时 |
| --- | ---: | ---: |
| SDF 描边 | 0.3 ms | 0.6 ms |
| 高斯模糊 | 0.9 ms | 2.4 ms |
| 柔光 | 1.0 ms | 2.5 ms |

表格对应 JSON 中 `2026-09-29T09:47:19.457Z` 的本机运行。演示截图保存在 `gpu-preview.png`。

本次是两字、单组主要参数、单台 Apple GPU 的算法验证。相同 WASM 轮廓作为双方输入，因此未验证 FreeType 与桌面字体路径的差异，也未验证完整桌面模板、所有字体/字符、所有参数、其他浏览器/GPU、长时间播放、多图层合成或视频导出。

## 复现与下一步

```sh
cd packages/text-wasm
npm run build
npm test
npm run test:gpu:reference  # 仅开发对比需要 macOS + Metal
npm run demo
# 打开 /demo/gpu.html，点击“对比 Metal 参考”
```

发布运行只需 `dist`、业务模板/字体与 WebGPU；macOS 参考程序不打入 npm 包。

下一步接入一个真实模板的 GPU 执行路径：将模板材质/效果参数映射到当前管线，恢复逐字 atlas 与 transform，按原始时间轴执行动画，再用完整桌面输出对齐。当前三个算法与真实硬件验证为这一步提供了可复用基础。
