# 2026-09-29–30 架构升级验收

本记录针对当前 `packages/core/media/render/server/client` 和 `src/editor`。历史 `docs/verification.md` 中的 FFmpeg 合成、11 个 MCP 工具等属于旧实现，不作为本轮结论。

## 运行结果

- `npm test`：95 项通过、0 失败、0 跳过。包含原生 Format 1 往返、批次顺序与失败回滚、关键帧切片、媒体属性 owner、关系选择闭包、缓存回收/取消、实际 npm 安装、MCP、导出协议与缺少本地编码程序的回归。
- Chrome 154 真实渲染矩阵：40 项通过。覆盖 3 个特效、4 个转场、6 个使用本机系统字体的复杂文字配方、6 个混合模式、立体声、三种编码路径，以及首帧写出后取消、过期帧丢弃、GPU 丢失后新设备恢复、字体读取失败后同渲染器重试和初始化期间关闭。640×360 预览与成品重新解码帧平均 RGB 绝对误差约 0.05–1.84/255。
- 浏览器媒体矩阵：10 组通过。29.97/VFR 采样与整数 PTS 参考一致，连续和回退定位、在途合并、取消、并发预算、跨会话源身份复用、Worker 组件及 DPR 2 均有实际运行证据。
- 一小时双声道波形：720 个脉冲完整保留，包括末端；冷分析 6.31 秒，缓存查询 83.7 毫秒。完成后 PCM 缓存为 0，数值波形约 15.98 MiB。具体环境、内存计量边界与原始结果见 [媒体性能](media-performance.md)。这不是旧版本与新版本的同画质 A/B 结论。
- 1000 片段时间轴与连续媒体游标：最终 120 秒长源解码 3596 帧全部 PTS 正确，滚动时最多挂载 80 个片段，取帧中位 0.3 ms、P95 0.4 ms、最大 5 ms；停止后约 215 ms 恢复真实可见高清图。测试 JS 堆峰值约 61.94 MiB，不等于完整进程内存或固定 FPS。
- Chrome 实际 UI：7 个效果/转场库真实预览卡片、添加柔光并改强度 0.65、擦除方向 up、多选文字混合值、双片段实际拖动一次提交/一次撤销均通过。另有真实 Timeline/Preview 组件的 Escape/外部替换/单次撤销 6 项集成测试，输入为合成事件，未冒充物理键盘中断。
- 工具栏响应式：1280 与 720 px 宽度下，工具栏和操作组 `scrollWidth === clientWidth`；按钮文字完整。窄面板扩展操作排到第二行，不使用横向滚动容器。
- 最终安装版画布：横屏 640×360、方形 360×360、竖屏 360×640，均实际导出带系统字体与声音的 90 帧作品，并独立检查成品尺寸。

## 实际安装版与无 FFmpeg 环境

通过 `npm pack` 构建 tarball，最终安装到独立 `.local/architecture-final`，从安装目录启动 CLI，服务进程使用空 `PATH` 且未设置 `FFMPEG` / `FFPROBE`。使用安装包的客户端和网页：

1. 纯 JavaScript 导入 H.264/AAC 视频和 WAV 双声道音频。
2. 编辑 3 秒作品，包含裁剪与分割、独立擦除转场、LUT、复杂文字模板、位置关键帧及音频淡化。
3. 纯浏览器编码 MP4/H.264/AAC 和 WebM/VP9/Opus，均为 640×360、90 个视频帧。视频编码和音频编码均未使用外部程序。
4. 调用 SDK 桥接器保存实际 `.vcut` 目录，再打开并比对 placement、visual、audio、retime、automation、effects、transitions；保持版本 1，无 `web-session.json`，不复制系统字体文件。
5. 用开发机器已有的 FFprobe/FFmpeg 独立检查成品的流、完整解码与反相双声道。校验程序不属于产品导出流程或使用者安装要求。

安装包不包含 FFmpeg/FFprobe、NodeAV、AAC WASM 编码器或平台专用原生桥接器，不自动下载这些程序。内置字体已删除；压缩包 7,534,355 字节（约 7.53 MB，101 个文件，零 runtime npm dependencies），主要包含文字 WASM 和模板图案资源。格式桥接器仍需单独配置，视频剪辑和浏览器导出不需要它。

系统字体使用本机 Arial 与 PingFang SC。六个模板及基础中英文文字的原生保存/重开已验证字体身份，npm 文件清单拒绝 TTF/OTF/TTC/WOFF 等字体文件。

AAC 校准导出经过五个非周期 PCM 标记检查，采样偏移均为 0；Opus 五个标记相对首个解码样本偏移为 0，容器毫秒量化仍会带来不足 1 ms 的绝对起点差异。AAC 解码补样和 WebM 不支持 DiscardPadding 的尾部补样已单独记录，没有据此声称样本级精确裁尾。

浏览器再次导入实际成品时，AAC 五点偏移为 0，Opus 为 0.5 ms；外部 Ogg 为 0，外部 WebM 最大 1 ms。局部音频解码加入有界预滚，并按容器的 CodecDelay/preskip 处理时间，避免首段失真和负 PTS 错移整段素材。


## 最终追加验收

- 60 秒高清作品：1920×1080、30000/1001、1799 帧 H.264 + 48 kHz 双声道 AAC，包含文字模板、LUT、柔光和擦除转场。纯浏览器编码用时 11.706 秒，文件 65,328,459 字节；6 个重新解码的实际 PTS 与同时间同尺寸合成帧平均 RGB 误差 0.566–0.875/255。已有 FFprobe/FFmpeg 独立确认音视频均 60 秒且完整解码成功，它们只用于开发验收。源视频为 640×360，故不能将此测试描述成原生 1080p 输入细节保真测试。
- 固定画质性能：改造前冻结的 npm 包与当前实现，固定 1280×720、相同图像/系统文字/三特效、四轮交错离屏渲染。最终画面逐字节一致；图层移动中位耗时由 2.4–2.9 ms 降为 0.7–0.9 ms。冷首帧区间重叠，不宣称普遍冷启动加速；仅重复帧计时的 0 ms 受精度限制，不代表零成本。方法、失败测量与边界见 [渲染性能](render-performance.md)。
- 基础兼容：在 Chrome 中隐藏 WebGPU 能力，Canvas2D 基础视频/图片/系统字/变换/混合真实预览及 WebM 导出回解码通过，复杂效果/转场/文字模板明确拒绝，共 5/5；这不是 Safari/Edge 实机验收。
- 编辑性能：同一 1000 clips 作品的 50 次单属性编辑、四轮交错测量，中位约 14→9 ms；单次正反历史补丁 331 字节，原整份快照 686,448 字节。此处是命令 CPU/历史存储指标，不冒充渲染速度。
- 最终安装版 MCP：15 个工具；同会话 HTTP 编辑→MCP undo→HTTP redo 一致，无浏览器返回带 `previewUrl` 的 `BROWSER_REQUIRED`，409 冲突保留版本/快照。MCP 导出阻塞请求期间仍能查询、ping 和取消。最终 MP4/WebM 均 90 帧，立体声反相相关系数 -1，无临时 PCM/mux/partial 残留。
- 59.94：新增一小时后帧位置和半帧吸附边界的专门断言；最终测试 95/95。
- 实际安装版预览：CUA 点击播放 120 秒视频与系统文字，70 秒中途采样及自然结束均无 UI 错误，本次播放期间无控制台错误。1280×720 作品在当前视口以 884×497 预览，自动质量系数保持 1；不把两个时间点的状态采样称为逐帧无丢帧验证。证据为 `installed-preview-long-final.json`，其中另存的旧导航日志不属于本次播放。

新增本地证据：`render-results/fixed-quality-ab-final.json`、`hd60-final.json`、`hd60-independent-validation.json`、`editor-component-final.json`、`ui-library-final.json`、`mixed-properties-final.png`、`effect-library-final.png`、`installed-acceptance-final.json`、`installed-output-final-validation.json`。源代码和安装包没有因为性能测试的画布读取问题而修改渲染算法；失败测量保留在同目录中。

## 证据与边界

本地证据在 `.local/architecture-qa/`，不进入发布包：

- `render-results/browser-report-complete-1790696658989.json` 及对应预览、成品、解码 PNG。
- `media-results/browser-report.json`、`timeline-1000.json/png`、`ui-interactions.json`。
- `installed-acceptance-final.json`、`installed-output-final/`、`installed-output-final-validation.json`、`system-fonts-final.png`、`toolbar-narrow.png`。
- `canvas-variants-final.json`、`toolbar-narrow-final.png`：最终安装版画布与响应式验证。
- `render-results/aac-sync-validation.json`、`opus-sync-validation.json`、`media-reimport-validation.json`：导出编码延迟及浏览器重新导入。

按用户要求不进行桌面 GUI 对照。未执行其他操作系统或 Edge/Safari 实机矩阵、4K 长视频持续性能和人工扬声器听音验收。GPU/JS 缓存数字不代表完整进程内存。SDR、恒速变调、原生效果允许的参数范围和无法无损映射时拒绝导入等边界见 [架构记录](architecture-upgrade.md)。

没有发布 npm、上传插件、提交或推送 Git。
