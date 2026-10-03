# @videocut/text-wasm

独立的 VideoCut 文字 WebAssembly 模块，直接编译复制进来的 C++ 文字、动画、文字合成及 Skia 绘制代码。运行时只需要浏览器或 Node.js、`.mjs` / `.wasm`、模板和字体；不需要安装、启动 VideoCut，也不通过本地渲染服务通信。

默认实现是 `wasm-raster-v1`：Skia CPU 栅格后端。0.2.0 增加独立的 `webgpu-effects-experimental-v1`，试移植 SDF 距离场/纯色描边、高斯模糊、柔光。它们复用原生算法，但实验 GPU API 尚未执行完整模板材质与动画图，也尚未接入主网页编辑器。

0.4.0 启用 WASM SIMD128：Skia 四通道栅格流水线及 SkVx 模糊向量运算。运行环境需要支持 WebAssembly SIMD；没有自动降级到旧标量二进制。`engine.capabilities.rasterPipelineLanes` 返回实际流水线宽度（当前为 4）。模板、分辨率、纹理和动画参数不变。性能和优化前后像素差异见 `reports/performance.md`。

## 本地运行

本工作区已经完成编译，可以直接运行：

```sh
npm --prefix packages/text-wasm run demo
# 打开 http://127.0.0.1:4321/
npm --prefix packages/text-wasm test
cd packages/text-wasm
npm pack
```

新机器从源码构建，需要 Node.js 22+、Git、Python 3、CMake 3.24+、Ninja。先运行 `npm run setup` 下载固定版本 Emscripten、Skia 及依赖，再运行 `npm run build`。这两个命令在本包目录执行。首次构建需要下载依赖并编译 Skia，后续增量构建无需 desktop VideoCut 项目。

`VIDEOCUT_WASM_TOOLS` 和 `VIDEOCUT_WASM_BUILD` 可指定构建依赖/输出目录。当前工作区优先复用 `.local/text-wasm-tools` 和 `.local/text-wasm-build`；独立拷贝本包时默认使用包内 `.cache/`。已有编译环境不需要再次运行 setup。setup 不覆盖未知来源的现有目录。

## 网页调用

将 npm tarball 安装到目标项目，部署 `.wasm` 为 `application/wasm`。下面使用 Vite 的 `?url` 导入；其他构建工具可以直接传部署后的绝对 wasm URL。

```js
import { createTextEngine, loadTextTemplate } from '@videocut/text-wasm';
import wasmUrl from '@videocut/text-wasm/videocut-text.wasm?url';

const engine = await createTextEngine({ wasmUrl });
const renderer = engine.createRenderer();
const bytes = async (url) => new Uint8Array(await (await fetch(url)).arrayBuffer());

// 注册模板中的字体 asset_id；浏览器不会读取系统字体。
renderer.registerAsset('builtin.font.inter.variable.v1', 'font/ttf',
  await bytes('/fonts/Inter.ttf'));
renderer.registerAsset('builtin.font.reference-cjk.source-han-sans-sc-medium.v1', 'font/otf',
  await bytes('/fonts/SourceHanSansSC-Medium.otf'));

const template = await loadTextTemplate('/templates/flower-style-09/manifest.json');
await renderer.loadTemplate(template, {
  bindings: { content: '你好' },
  allowRasterFallback: true,
  // 为缺少 CJK fallback 的原始模板显式补充字体，不改磁盘上的模板文件。
  fallbackFonts: [{
    id: 'builtin.font.reference-cjk.source-han-sans-sc-medium.v1',
    family: 'Source Han Sans SC'
  }]
});
renderer.draw(canvas, { timeUs: 1_500_000 });
renderer.setText('花字');
renderer.draw(canvas, { timeUs: 1_500_000 });

// 动画使用时间驱动，同一个实例连续传入递增或跳转的 timeUs。
const frame = renderer.render({ timeUs: 2_000_000, width: 960, height: 540 });
// frame.data: 独立拷贝的紧密排列 RGBA8，透明通道为 straight alpha。
// frame.width/height 为裁剪后的文字图尺寸；放置到 originX/originY。
// 原始字体大小、换行规则来自模板；长文字不一定自动缩小。
renderer.dispose();
engine.dispose();
```

每个 renderer 加载一个模板；切换包时释放旧实例、创建新实例。`setText` 默认绑定 `content`，多槽模板用 `setBindings`。字体和模板资源先注册后加载。帧像素是 JS 自有副本，后续渲染、内存增长及 dispose 不会使旧帧失效。

Node.js 可以直接 `createTextEngine()`，自动从相邻路径加载 WASM。加载模板时需要传绝对 HTTP URL 或自定义 `fetch`。Worker 中同样可以使用 render，并将帧数据传给主线程。CPU 渲染是同步调用，对复杂/外部模板建议放在可终止的专用 Worker 内；本版本没有异步渲染取消 API。模块不依赖 SharedArrayBuffer 或跨源隔离。

## 实验 WebGPU 效果

演示：`http://127.0.0.1:4321/demo/gpu.html`。支持改中文、调整描边/模糊/柔光强度、播放效果强度动画，页面底部有三种实际 GPU 输出的并排预览。浏览器需要 WebGPU 和安全上下文（HTTPS 或 localhost）；不可用时明确报错。

在前面已完成字体注册、模板加载的 renderer 上使用以下 API，最后再 dispose：

```js
import { createTextGpuEffects } from '@videocut/text-wasm/webgpu';

// 使用尚未创建 2D/WebGL 上下文的新 canvas。
const gpu = await createTextGpuEffects(gpuCanvas);
gpu.setMesh(renderer.prepareSdfMesh({ width: 640, height: 360, range: 30 }));
gpu.render({ effect: 'soft-glow', radius: 12, sigma: 6, exposure: 1.5 });

// 参数动画只需继续 render；文字变更后重新生成并上传 mesh。
renderer.setText('你好');
gpu.setMesh(renderer.prepareSdfMesh({ width: 640, height: 360, range: 30 }));
gpu.render({ effect: 'sdf', strokeWidth: 12, fill: [0.92, 0.08, 0.22, 1] });

await gpu.completed(); // 验证/截图时等待；普通播放无需每帧阻塞等待。
const pixels = await gpu.readPixels(); // 完整画布的 premultiplied RGBA8。
gpu.dispose();
```

`prepareSdfMesh` 使用复制的 SDK 排版、字形轮廓与 SDF 网格算法，当前只导出基础排版轮廓，不应用逐字时间动画、原始复杂材质或后处理图。纯色填充与描边参数由 `gpu.render` 提供。它将整段文字合成一个距离场，而不是完整原生逐字 atlas。调节效果参数不重新排版；改字需要重新构建轮廓。`range` 限制距离场能表达的描边范围，超出范围的描边不能靠增大 `strokeWidth` 恢复。

也可 `gpu.setSource({width, height, data})` 上传同一张 premultiplied RGBA8 图测试高斯/柔光；CPU `renderer.render()` 返回 straight alpha，必须先乘 alpha，并按 origin 放到目标画布后上传。最大 GPU 画布 2048×2048，半径 0–128；这些是实验 API 的范围，不代表此范围内的全部参数均已与原生验证。`submissionMs` 仅为 CPU 提交时间，`completed()` 才等待 GPU 队列完成。

模块运行时仍只有 WASM、JS、模板、字体和浏览器 GPU，不需要 VideoCut 或本地渲染服务。macOS Metal 程序仅用于开发时生成对比参考图，不属于 npm 运行依赖。

## 复用范围与边界

0.3.0 的复杂模板演示在 `/demo/complex.html`：两套带透明视频装饰的原始模板、两组纹理花字/底板/逐字动画组合、立方体投影和多阶段乱码打印。前两套通过 `@videocut/text-wasm/composition` 执行，原生 C++ 采样动画、装饰 affine/视频时钟和后期参数；浏览器解码原包 MP4，WebGPU 还原左右打包的 alpha/RGB、合成、执行柔光或径向模糊。未启动任何原生渲染进程。

```js
import { createBrowserTextComposition } from '@videocut/text-wasm/composition';
const player = await createBrowserTextComposition(engine, freshCanvas, bundle, {
  width: 640, height: 360,
  bindings: { content: '绽放' },
  fonts: [{ id: fontAssetId, mediaType: 'font/otf', bytes: fontBytes }],
  fallbackFonts: [{ id: fontAssetId, family: 'Source Han Sans SC' }],
  experimentalComposition: true
});
await player.render({ timeUs: 1_500_000 });
player.setText('你好');
await player.render({ timeUs: 1_500_000 });
player.dispose();
```

实际调用必须提供模板引用的全部主字体和 fallback；例如演示同时注册 Inter 与 Source Han Sans。`experimentalComposition: true` 只接收 `anim-studio-time-transform-softglow` / `anim-studio-dual-selector-radial` 的当前受支持结构。普通模板可用同一合成 API，关闭该选项。操作按顺序 await，不在渲染中途改字或 dispose。`readPixels()` 为预乘 RGBA8；视频帧缓存最多 8 帧且按 32 MiB 预算约束。

这些复杂模板仍使用 WASM 栅格字形；后期参数由原生 contract 按输出画布尺寸计算，未恢复桌面 Page render-group 的完整尺寸和 SDF 材质。因此是能播放、能改字的实验移植，不能宣称完整原生像素一致。CPU 纹理/投影模板有明显帧耗时，演示显示实际总耗时，不保证全部模板实时。详情见 `reports/complex-port.md`。

- 复制的模块：`text`、`text_composition`、`frame`、`vector`、`skia_runtime`，以及 SHA256/Float16/packed alpha 小型辅助依赖。没有链接 Flutter、桌面编辑器或 FFmpeg。
- C++ 桥接层提供实例管理、资源解析、完整模板解码、原生动画求值、文字替换、帧绘制及诊断；JS 层提供模板下载、路径与摘要校验、资源注册和 Canvas 绘制。
- 模板包保留 `manifest.json`、`composition.json`、`animation.ir.json`、`effect.program.json` 及 assets。不是把原生模板重写成 CSS。
- SDF 模板默认报错，调用者必须显式选择 `allowRasterFallback` 才走 SDK 的栅格字形路径。返回的 warnings 会标记该差异。
- 默认 CPU renderer 不支持 Metal 专用图节点、原生后处理、部分动画资源和 composition-owned 装饰；上文实验合成 API 仅接通列出的两套装饰/后期结构。失败返回错误，不会把成功加载当成成功绘制。
- 原生执行图中一部分 CPU SkSL 效果虽可运行，但可能很慢。演示将抽样报错/超时的模板置灰；“可选”仅代表一次抽样有像素，不表示完整兼容。
- GPU 实验模块实现了 SDF 距离场算法和部分材质/后处理，但默认 CPU renderer 的兼容范围未扩大。未实现完整桌面 SDF 模板执行路径、复杂效果全量移植、视频编码导出、自动字体下载、系统字体回退。视频资源只在实验合成 API 的受支持 packed-alpha 装饰中解码。
- 模板 admission digest 是本模块对配置及已注册资源计算的实例标识，不是桌面 catalog 的 package digest，不能直接当作桌面工程身份回写。
- 最多 32 个实例；单资源 64 MiB、单实例注册资源 256 MiB、单幅图最多 8M 像素且单边不超过 4096；WASM 线性内存上限 1 GiB。dispose 释放对象和缓存，但已增长的 WASM 线性内存不会缩小；长期应用可复用 engine，或终止专用 Worker 回收整个引擎。

## 验证

性能对比：在包目录执行 `npm run test:performance:prepare -- /path/to/videocut-text-wasm-0.3.0.tgz`，再打开 `/demo/performance.html`。它使用匹配的旧版 JS/WASM 和当前构建，在真实浏览器中交替测量、读回像素，结果保存到 `reports/performance-validation.json`。基线是开发输入，不包含在发行包中。

见 `reports/validation.md` 和 `reports/template-audit.json`。自动化测试执行真实 WASM，包括中文改字、透明像素、确定性跳转、动画变化、实例释放、失败诊断、输入边界与复制源码摘要。

GPU 试移植的范围、实际 Metal/WebGPU 像素对比与性能边界见 `reports/gpu-port.md` / `reports/gpu-validation.json`。在 macOS 上运行 `npm run test:gpu:reference` 生成未修改的原生 Metal 着色器参考，再打开 GPU 演示点击“对比 Metal 参考”执行真实浏览器 GPU 验证并保存报告。这验证相同输入上的效果算法，不等于完整桌面模板画面对齐。

`npm run audit` 对复制的 122 个模板分别在 Node Worker 中采样一帧（640×360、1.5 秒、单次 CPU 预算 3 秒），结果写入 `.cache/template-audit.json`。这是诊断脚本，不等于视觉一致性验收。正式保存的 reports 是这次运行的快照，不会被脚本隐式覆盖。

## 源码与发布

`vendor/SOURCE.json` 记录来源、原仓库提交以及复制文件的实际 SHA256。原仓库可能有未提交修改，因此文件摘要比提交号更精确。vendor 保持原样，WASM 适配放在 `src/` 和顶层 CMake。模板动画 JSON 解码器从原生 editor product 文件抽取；源路径与摘要同样记录在 SOURCE.json。

Skia 固定 `933b272d039add454e6491efafd2db84a18a92dd`，Emscripten 固定 `4.0.9`。发行包仅包含 dist、说明和许可证；源码、演示字体、模板及构建工具不会打进 npm 包。模板和字体应由调用应用自行部署。`dist/build-info.json` 记录 WASM 大小和摘要。

当前包采用 `MIT` 许可证，未单独发布到 npm。第三方声明见 `NOTICE.md` / `LICENSES/`。
