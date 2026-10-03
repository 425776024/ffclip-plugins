# HTML 动画与花字模板

VideoCut 的动画创作继续使用现有编辑器、媒体轨道和时间轴。新增 `html-clip` 表达 HTML、SVG、CSS 和 GSAP 图形动画；音视频仍由现有媒体元素负责。HTML 图形可以叠在视频上，和图片、文字、音频混排，共用播放头、拖拽定位、裁剪、分割、恒速、画面变换、效果、转场及撤销重做。

插件提供 [motion-templates SKILL](../plugins/videocut-local/skills/motion-templates/SKILL.md)。Agent 可根据自然语言创作动画并打开同一编辑会话，用户可以继续人工编辑。这提供了将 HTML 动画接入成熟剪辑工作流的路径；当前能力不等于全部 HyperFrames 功能或任意 HyperFrames 工程导入。

## 两种模板

| 表达                     | 用途                                               | 编辑与输出                                                             |
| ------------------------ | -------------------------------------------------- | ---------------------------------------------------------------------- |
| 原生花字 `text.template` | 组合已提供的字体材质、背景与逐字动画               | 可编辑，保存完整 `.vcutweb`，通过更新桥往返原生 `.vcut`，导出 MP4/WebM |
| `html-clip`              | 自定义 DOM/SVG/CSS/GSAP 图形、图表、标题与透明装饰 | HTML 与变量保留在完整 `.vcutweb`，逐帧合成后导出 MP4/WebM              |

`list_motion_templates` 返回已有模板、支持的原生配方部件、`nativeComposition` 组合约束和 HTML 渲染器能力。新增 `backdrop` 或 `animation` 只允许以 `flower-style-03`、`flower-style-38` 为基础；cube、printer 与 studio/external 基础包保留原始执行图，可使用原模板或只指定 `base` 的配方，不能任意追加背景或替换动画。部件已发货不代表任意组合都生效，不支持的组合会在作品校验与资源加载前明确拒绝。六个内置预设的行为不受影响。例如：

```json
{
  "id": "my-flower",
  "version": 1,
  "recipe": {
    "base": "flower-style-38",
    "backdrop": "bubble-nine-slice",
    "animation": "anim-lua-letter-transform"
  }
}
```

把该对象传给 `add_text.template` 或原始 `edit_timeline` 的 `add_text.template`。`base` 取自 `TEMPLATE_PARTS.base`；可选背景为 `bubble-tile`、`bubble-nine-slice`；可选动画为 `anim-lua-letter-transform`。这不开放任意 Lua/Shader 或尚未移植的原生模板部件。自定义配方可以编辑、导出 MP4/WebM、保存完整 `.vcutweb`，并通过能力为 `customTextRecipes:1` 的更新原生桥写入及重开 `.vcut`。

花字模板默认使用自带字体、字号与材质颜色。添加模板时省略基础文字的 `fontSize`、`color` 与字体覆盖；通过共享 `visual.scaleX`、`visual.scaleY` 和位置调整大小与排布。需要覆盖材质颜色或字号时，使用 `template.style` 的 `color` / `fontSize`；更新原生桥的 `textTemplateStyles:1` 能力保留这些样式。

## 添加 HTML 图形

`add_html_clip` MCP 工具接受现有会话 `id`，以及完整 `html` 对象或授权目录中的绝对 HTML 文件路径 `path`。本地文件导入把相对 JS、CSS 和图像资源冻结进 HTML；后续预览和导出读取同一份已保存内容。

```json
{
  "id": "实际会话 id",
  "path": "/授权目录/graphics/lower-third.html",
  "width": 1920,
  "height": 1080,
  "duration": 600000,
  "transparent": true,
  "variables": { "title": "本地创作", "subtitle": "动画与视频，共用时间轴" },
  "start": 240000,
  "length": 600000
}
```

默认创建最上方的“HTML 动画”视频轨道。指定 `trackId` 可以放入已有未锁定视频轨道；同一轨道不能重叠。需要叠加时使用不同轨道，第 0 轨是最上层。

完整模型中使用 `clip.type = "html-clip"`、空 `assetId`，并存储：

```ts
type HtmlContent = {
  html: string;
  width: number;
  height: number;
  duration: number;
  transparent: boolean;
  variables?: Record<string, string | number | boolean>;
};
```

原始 `edit_timeline` 支持 `add_html_clip { html, name?, start?, length?, trackId? }` 和 `set_html_clip { itemId, html, name? }`。后者替换一个片段的完整内容，保留源范围、位置与变速；新动画时长必须覆盖当前源出点。分割后的两个片段各自持有独立内容，修改一个不会修改另一个。

选中 HTML 片段后，“内容”属性面板直接展示解析出的文字、颜色、数值、字体和已有动画参数，不展示源码或变量 JSON。颜色可点击色块选取，并用不透明度滑杆调整透明度，也可输入 CSS 颜色。属性、名称、尺寸和背景修改在停止输入 300 毫秒后自动应用，无需点击应用按钮；中文输入法组词期间暂缓应用，无效值在面板内提示。静态样式支持内嵌样式表与带引号的行内样式；可识别的单个数值保留原单位，颜色保留透明度。应用修改只替换对应的源内容值，保留其余 HTML 和动画脚本，并沿用撤销重做。脚本动态生成的内容和无法可靠解析的样式不会生成属性控件；这些内容可由模板的动画参数开放编辑。动画模板卡片仅保留预览，悬停时在卡片内显示添加按钮，不单列模板名称或按钮行，不展示技术说明或源码／HTML 导入按钮；自定义 HTML 创作与导入通过 Agent 工具完成。

模板库共提供 10 款 1920×1080 动画，均有入场、停留和退场，可通过普通内容属性修改文案、字体和配色：

| 模板     | 实际用途               | 专属编辑内容                                   |
| -------- | ---------------------- | ---------------------------------------------- |
| 棱镜发布 | 品牌片头、发布会开场   | 标题、章节、装饰说明                           |
| 瑞士叙事 | 知识讲解、三点总结     | 三个要点及标签、展示编号                       |
| 极光数据 | 业绩汇报、增长展示     | 指标数值与单位、四个图表数值与标签、趋势说明   |
| 暮色演讲 | 演讲封面、章节转场     | 标题、签名文案、章节                           |
| 蓝图路线 | 教程流程、项目规划     | 三个阶段的标题、说明与标签                     |
| 全息产品 | 软件产品介绍、产品卖点 | 产品名称、短句、三个卖点与说明                 |
| 动感促销 | 电商上新、活动宣传     | 活动价格、原价、优惠、角标、行动文案、活动说明 |
| 前后对比 | 效率对比、服务升级说明 | 前后两组数值、标签与要点、结果文案             |
| 人物介绍 | 采访、讲师、主持人介绍 | 姓名、身份、人物缩写、介绍短句                 |
| 呼吸标题 | 视频片头、叠加章节标题 | 主副标题、标签、章节与品牌名                   |

前八款为 8 秒实色背景模板，后两款为 6 秒透明叠加模板。全息产品采用悬浮产品面板与光轨；动感促销采用弹性票券入场；前后对比采用两侧展开与分批要点揭示。人物介绍和呼吸标题沿用稳定的模板 ID，并升级为玻璃卡片、光线勾勒与呼吸停留。极光数据的曲线和末端标记由四个数值生成，支持上升与下降趋势；图表值按非负数处理，以最大值自动缩放，不是固定示意曲线。

新插入模板按编辑器语言使用中英文默认文案，已有片段内容不随语言改变。促销价格为自由文本，对比结果为示例，应替换为自己的实际信息。预览图来自每款第 3 秒的真实渲染；修改模板后运行 `npm run build:html-posters` 更新全部 10 款预览图。

## tick 与 GSAP 的统一时钟

作品时间以每秒 120000 个整数 ticks 表示。`duration` 是动画源时长；`start` 和 `length` 是轨道位置与片段长度。裁剪、分割、恒速先映射成 `sourceTime`，再调用图形渲染。原有 `sourceInSeconds`、`durationSeconds`、`atSeconds`、`startSeconds` 仍然使用秒。

运行时注入本地 GSAP，并在 authored 脚本前提供 `window.__videocutVariables`。注册暂停的 GSAP 时间线到 `window.__timelines`，每一帧都会 seek 到对应源时间。可选 `window.tick(seconds, context)` 或 `window.__videocut.tick` 在动画 seek 后运行；`context` 包含源 `width`、`height`、以秒表示的 `duration` 和 `variables`。初始化需要等待时，设置有限的 `window.__videocutReady` Promise。字体与图像解码也会在首帧前等待。

因此，播放、导出和左右拖拽播放头使用相同的源时间语义。模板必须能接受任意时间及反向跳转：不使用墙钟、增量积分、自主 `requestAnimationFrame` 或定时播放驱动视觉状态。CSS/WAAPI 动画同样被暂停并按源时间设置。详细契约与操作示例见 [HTML authoring reference](../plugins/videocut-local/skills/motion-templates/references/html-clip.md)，可复用的示例见 [GSAP 透明 lower third](../plugins/videocut-local/skills/motion-templates/assets/gsap-lower-third.html)。

## 本地资源、透明与边界

源 viewport 每边为 1–4096 整数像素，总计不超过 8388608 像素。嵌入资源后的 HTML 不超过 1 MiB UTF-8；本地导入的资源原始总量同样限 1 MiB。动画源时长为 1 tick 至 24 小时。变量最多 100 个，仅支持有界文字、有限数字和布尔值。

`transparent: true` 时，将 `html`、`body` 和满屏容器背景设为透明；局部卡片可保持不透明。渲染器输出透明 PNG，现有合成器继续应用位置、缩放、旋转、透明度、裁剪、混合、效果与转场。HTML 的透明区域露出下层视频或图像。

渲染需要本机 Chrome/Chromium；也可以设置绝对可执行路径 `VIDEOCUT_CHROMIUM`。服务使用独立 headless profile，将画面传给 Codex 的预览页面，保持编辑页与 authored HTML 分开。网络与 file URL 访问被阻止。导入支持本地相对 JS、样式链接、CSS `url(...)` 和单一图像 `src`；CSS `@import`、`srcset`、远程依赖和 JS 模块依赖图需要预先合并/打包。`audio`、`video`、`iframe`、`object`、`embed` 不支持，音视频使用原轨道。

MP4/WebM 导出逐帧把图形与媒体合成到最终画布，保留合成过程中的 alpha。输出整段视频的透明通道没有新增。`save_project` 保存完整的 `.vcutweb` 目录，包括媒体、HTML 源内容与变量、花字配方/样式、效果、关键帧和冻结的模板资源；目录可整体搬移并用 `open_project` 重开。原生 `.vcut` 支持花字配方与样式往返，HTML 仍需要 `.vcutweb` 保存。

反向定位的确定性、真实播放帧率和最终编码输出是不同验证项。预览已显示一帧不代表复杂 HTML 可以实时满帧播放；实际性能受场景、源分辨率和机器影响。交付时分别报告静态/模型检查、浏览器拖拽与播放观察、编码导出及成片读取结果。

连续拖针保留正在渲染的一帧，并合并到最新目标时间，避免每次鼠标事件都取消渲染；松手后精确定位。拖动、播放和暂停使用相同的预览分辨率，不按帧耗时自动降级。HTML 的 CSS viewport 与捕获像素保持源尺寸，视频导出也按源尺寸捕获 HTML，再进入完整作品合成。手动“完整”画质使用作品完整分辨率。优化通过并行准备、复用纹理与有界精确帧缓存进行，复杂场景首次渲染仍可能影响交互帧率。

缓存参考原生 C++ 合成器的内容身份与资源驻留策略：HTML 源内容、变量、透明度和源时间共同标识一帧；合成位置及外部特效不进入源纹理身份。回拖命中仍驻留的 GPU 纹理时，跳过 HTML 捕获、文字栅格化及视频寻帧。CPU 仅保留有界纹理元数据，并在每次命中前检查实际 GPU 驻留状态与尺寸；GPU 继续使用原有 512 MiB 共享预算。服务另保留最多 64 MiB / 256 帧的原尺寸无损 PNG 缓存。两类缓存失效或被淘汰时，按相同源时间和尺寸重新渲染。
