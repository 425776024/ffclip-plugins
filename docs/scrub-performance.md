# 原尺寸混合拖针优化验证 · 2026-10-01

同一四字原生花字、1920×1080 透明 HTML/GSAP、视频混合项目，全 1920×1080 输出，8 秒连续左右拖针基准从 13.23 FPS 提升到 36.31 FPS；第一次复测为 37.06 FPS。最终一轮后 4 秒为 47.5 FPS。保留同一个 512 MiB GPU 预算，没有自动降清或缩小 HTML、视频输入像素。

这是本机 Codex 内置浏览器的真实 worker 出帧测量，输入为 60Hz 合成 PointerEvent，不能直接当作显示器呈现延迟或所有工程的保证。重花字连续新时刻播放仍约 20.72 FPS，未缓存的原生 CPU 绘制仍可能出现卡顿。拖针 P95 约 95.8ms、最大出帧间隙约 204ms，没有达到全部冷帧稳定 30 FPS。

## 实现

- 只在完全透明、无 colorFilter / existing imageFilter、SourceOver 的原生材质层跳过无效 Blur/Offset 分配；非零阴影和其他混合模式保留原逻辑。
- 历史原生花字纹理保存已经完成绘制的像素子区域；绘制前在 GPU 上清空并通过整数 copy 还原到原始完整画布，继续使用原 shader 和原采样坐标。直接修改 UV 的方案存在 1 LSB 差异，未使用。
- 在预算内优先保留重绘昂贵的花字输入，其他缓存仍按 LRU 淘汰。当前工作集的 pins 不受影响；没有活跃花字时恢复普通优先级。
- RGBA 紧致行使用单次独立内存复制，保留原 API 像素所有权；带 padding 的行仍正确逐行复制。
- 播放按工程原帧率的帧边界取样；暂停和松手后仍使用准确 ticks，音频时钟不改变。支持非整数 tick 的帧边界。

## 实际验证

- 浏览器旧/新 WASM：17 对完整 1080p RGBA 直接逐字节相等，最大误差 0；包含花字 38/03、组合、倒退和非零阴影控制。字体使用本机 Arial / PingFang SC。
- 独立 GPU 成品像素测试：11 项缩放、旋转、镜像、裁剪、模糊、glow、LUT / opacity、转场等逐字节相等。12 帧小区域测试的历史纹理占用从 124,416,000 降至 40,336,896 字节；单帧需要额外临时全幅纹理，不代表每个模板都有同样节省比例。
- 最终混合拖针：原生花字命中 229 / 291 帧；松手后准确定位到 264000 ticks，输出保持 1920×1080。所有 HTML 输入仍为原始 1920×1080。
- 新编译插件中实际向右、向左、再向右拖动，分别定位 528000 / 240000 / 528000 ticks，完整画质比例始终为 1。
- 正式构建、173 项根测试和 npm 包校验通过；插件 bundle 已重建。
- 新插件实际浏览器编码导出：6 秒、180 帧、1920×1080 H.264 30 FPS，AAC 双声道 48kHz。重新读取流信息，并解码检查 1.5 / 4.4 秒画面。编码有损成品不作为原始 RGBA 等价证明。

原始报告位于 `.local/html-evidence/opt-summary.json`、`opt-final-scrub-playback-full-1080p.json`、`opt-native-quality-browser.json`、`opt-full-hd-export-readback.json`。纯原生隔离计量和 20 帧像素对照位于 `.local/qa/wasm-native-opt/comparison.json`，不替代编辑器端到端 FPS。

当前可运行产物是 `.local/plugin-bundle/videocut-local`；已安装目录没有在本次工作中自动替换。

## 有限提前绘制与 GPU 花字原型 · 后续优化

预览新增两个独立原生花字 worker，准备最多六个后续工程帧。优先当前帧、相同 source key 共享任务；预测窗口向前移动时丢弃旧的待执行后台任务。已运行的 WASM 不抢占，跳转、修改和尺寸变化使旧 generation 完成结果失效。RGBA 快照最多 64 MiB / 64 项，另有各 worker 的 WASM 堆、资源和原生缓存；这不是总进程内存上限。现有 GPU 预算仍为 512 MiB。导出继续使用顺序参考渲染器。

所有提前绘制都使用原模板、系统字体、材质、画布尺寸和精确源时间；子 worker 仅生成拥有独立存储的 RGBA，不重复绘制中间 canvas。父渲染器按原 origin 还原完整 canvas，再走原 GPU 合成。覆盖 flower-style-03 / 38 及它们的配方；其他模板使用原路径。线程不可用时恢复顺序渲染，模板输入错误继续显式报告。

同一当前二进制、原始四字重花字工程、本机 Codex 浏览器，停止其他 GPU/编译测试后测得：

| 测量 | 关闭预渲染 | 开启预渲染 |
| --- | ---: | ---: |
| 新 renderer 首帧完成后，立即播放 5 秒 | 19.0 FPS | 20.8 FPS |
| 新 renderer 首帧完成后，暂停 600ms，再播放 5 秒 | 19.4 FPS | 23.2 FPS |
| 8 秒、60Hz 合成 PointerEvent 左右拖针 | 24.09 FPS | 28.08 FPS |

这组分别保留 renderer 初始化、首帧、暂停等待与正式 5000ms 采样窗口。不是显示器 FPS，不代表全部冷帧达到 30 FPS。此前 36.31 FPS 是前一轮测量，不能与本表的当前 worker 生命周期、调度和系统状态直接混为一次 A/B。未停止其他 GPU 测试的探索数据另存，不作为上表依据。暂停预取后的 P95 由 132.4ms 降到 90.1ms；复杂动画初段仍有停顿。原尺寸与原始输入保持不变，松手精确定位到 264000 ticks。

实际浏览器逐像素对照：3 个原生源 RGBA（包括倒退）与 3 个完整 WebGPU 混合帧，各 8,294,400 字节全部相等，最大误差 0。原生 recorder 还移除了字符动画分支中未使用的整段排版构造，独立 20 帧 RGBA / bounds 对照相等。

本轮报告：`.local/html-evidence/next-prefetch-pixels.json`、`next-prefetch-uncontended-{off,on,off-ready,ready,off-scrub,scrub}.json`。新原生 WASM 对照在 `.local/qa/wasm-native-next/`。

完整材质的 Skia Ganesh / WebGL GPU 原型已在 Apple M4 Pro 的 Chromium ANGLE Metal 上实际绘制，包含纹理描边、底板、柔阴影和 Lua 字符动画。它与当前 GPU 合成成品花字纹理是两个不同阶段。GPU 原型保留原分辨率并显著缩短单花字样本绘制时间，但当前边缘、纹理采样与柔阴影有真实像素差异，因此没有进入默认渲染或导出。实验报告及成对图片在 `.local/qa/wasm-gpu-next/`；独立 GPU 绘制时间不能当作整个编辑器的实时播放 FPS。

顺序正式验证通过：`npm run build`、根目录 203 项测试、126 个 npm 发布文件校验、插件 bundle 构建。构建和包测试曾因同时修改生成产物发生冲突；重新按构建 → 测试 → 包校验 → bundle 的顺序执行后全部通过。最新编译版预览使用生成 bundle，已安装插件目录仍未自动替换。

最新编译 bundle 的实际原生指针向右、向左、再向右拖动定位 528000 / 240000 / 528000 ticks，尺寸 1920×1080、质量比例 1。演示使用同配方的两字排版，和上表四字性能工程分别记录。实际浏览器编码 MP4 导出完成并重新读取：6 秒、180 帧、H.264 1920×1080 30fps，AAC 双声道 48kHz；已解码查看 1.5 / 4.4 秒帧。报告 `next-export.json` / `next-export-readback.json`，界面截图 `next-final-editor.png`。有损 MP4 不是 RGBA 无损对照依据。

GPU 原型最终独立无争用测量（含读回，单花字、15 个新时刻）：03 为 CPU 40.7ms → GPU 10.2ms，38 为 29.0ms → 10.4ms，03+tile+letter 为 24.8ms → 9.75ms，38+nine+letter 为 105.5ms → 16.9ms。软件 coverage 候选的 28 组 GPU hash 与原始 Ganesh 相同，没有改善当前 filter / texture / blend 差异；未进入生产。可审查的最小实验源码和构建/fixture脚本保存于 `packages/text-wasm/experiments/ganesh/`，大依赖、独立构建产物与原始报告仍在 `.local/qa/wasm-gpu-next/`。
