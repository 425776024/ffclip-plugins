# 文字 WASM 性能优化 · 0.4.0 · 2026-09-29

在相同 640×360、默认两字、相同模板和动画参数下，浏览器实测七纹理花字与空间翻转的预热后 P95 均已低于 50 ms。没有降低分辨率、移除材质/阴影、预渲染整段视频或扩充整帧缓存。

## 哪里慢

对复制的 `SkiaTextRenderLane::Render` 做了临时分段计时：规划（含布局和动画）、底板记录、字形材质记录、装饰、执行图、最终栅格绘制、帧发布。诊断数据在 `performance-profile.json`；正式构建不含这些打印。

预热后的原标量版本，最终栅格绘制占约 97–99%：七纹理案例的规划约 0.3–0.5 ms、字形记录约 1.3–2.1 ms；空间翻转规划与执行图通常各不足 1 ms。Node 的诊断绝对耗时与浏览器不同，因此不将其作为网页性能承诺。

七纹理模板不只是七张 PNG。它有多层纹理描边、嵌套阴影描边和大半径模糊，最终逐像素采样、混合与模糊占主要成本。原实现已缓存字体和解码纹理；瓶颈不是每帧下载资源。空间翻转的 CPU 投影 shader 也在最终栅格阶段计算。

## 改动

1. SDK 与 Skia 编译启用 `-msimd128`。只对 Skia 的 `SkOpts.cpp` 使用 Emscripten 的 `-msse4.1` 转译，选择四像素栅格流水线。输出依然是 WASM SIMD，不要求用户 CPU 是 x86。运行时读取实际 `raster_pipeline_highp_stride`，公开为 `engine.capabilities.rasterPipelineLanes`，测试要求值为 4。
2. 只在 `SkBlurEngine.cpp` 去掉默认 `SKVX_DISABLE_SIMD`，启用 SkVx 已有的 WASM 向量运算，优化 Gaussian/box 模糊的通道累加。
3. 编译器 wrapper 仅调整这两个热点文件的编译参数，未修改原生 SDK、vendor 或 Skia 源码。构建使用独立 `skia-wasm-simd` 目录；wrapper 摘要变化时只使这两个对象文件失效，避免 Ninja 忽略 wrapper 内容变化。

第一步单独完成时，七纹理花字 P50/P95 为 63.4/80.0 ms；启用模糊向量运算后才进入本次的 50 ms 目标。

Emscripten SSE→SIMD128 支持说明：<https://emscripten.org/docs/porting/simd>。本包实际固定使用 Emscripten 4.0.9；上述结论来自本地编译与实测，而非文档推断。0.4.0 需要运行环境支持 WASM SIMD，没有捆绑标量自动回退。

## 浏览器 A/B 结果

| 模板 | 优化前 P50 / P95 | 优化后 P50 / P95 | 优化后最大值 | P50 加速 |
| --- | ---: | ---: | ---: | ---: |
| 七纹理＋平铺底板 | 133.4 / 156.4 ms | **40.6 / 47.1 ms** | 47.6 ms | 3.29× |
| 空间翻转 | 58.8 / 59.8 ms | **15.2 / 16.3 ms** | 16.8 ms | 3.87× |

以 `performance-validation.json` 的原始数据为准。每个引擎先渲染 12 个不同时间点预热，再测量 24 个不同时间点，交替 A→B / B→A 顺序。序列超过原有 8 帧缓存容量，时间点也不同于预热序列。计时从 WASM 文字渲染开始，直到浏览器 GPU 队列完成，不包含额外像素读回和比较。不能将这些数值当作屏幕呈现延迟。

首个渲染请求单独记录：七纹理 **105.8 ms**，空间翻转 **36.0 ms**；不包含引擎/字体下载和模板安装。七纹理首帧仍超过 50 ms。因此“50 ms 内”仅指本次默认两字、640×360 的预热后采样，不保证冷启动、长文字、更高分辨率、其他浏览器/硬件、系统负载或所有时间点。

## 画面和功能

- 两个重模板 24 帧逐帧 GPU 读回，与原 0.3.0 标量 WASM 对比；另测“你好”和“文字动画”两组改字。原包、字体、纹理和后期代码一致。
- 浏览器七纹理比较的最大通道差为 4/255，单帧最大平均绝对差约 0.00351/255；空间翻转最大差为 1/255。少量像素存在 SIMD 数值/舍入差异，不能称逐像素完全一致。
- `raster-simd-validation.json` 补查六个演示、两组中文、四个时间点，共 48 帧。比较预乘 RGBA；两套外部合成模板同时要求原生装饰/后期参数计划完全一致。该 Node 检查不覆盖浏览器视频解码与 GPU 后期。
- 两项比较预先使用相同阈值：平均通道差 ≤0.1/255，通道差 >2 的比例 ≤0.5%，实际数值全部保留。48 帧 CPU 比较均通过。
- 构建、14 项 WASM 测试通过，包括 vendor 摘要、中文编辑、确定性、动画、底板和 SIMD 流水线宽度。截图 `performance-preview.png` 展示真实浏览器画面与表格。
- 0.4.0 tarball 已在隔离 consumer 安装验证：真实中文栅格绘制、SDF 网格、composition 导入、运行时 SIMD 宽度与三组 API 的 TypeScript strict 检查通过。`performance-build.json` 记录最终构建和包的摘要；确认其 WASM 与浏览器报告一致。复杂模板页也已加载新引擎并实际播放七纹理动画。

## 复现

在包目录运行：

```sh
npm run build
npm test
npm run test:performance:prepare -- /absolute/path/to/videocut-text-wasm-0.3.0.tgz
npm run test:performance:raster
npm run demo
```

打开 <http://127.0.0.1:4321/demo/performance.html>，点击“运行性能与画面对比”。基线必须配对使用旧版 JS 胶水和 WASM，不能将不同构建的胶水与 WASM 混用。此页面是开发验证工具，性能报告会在操作完成后保存到本地。
