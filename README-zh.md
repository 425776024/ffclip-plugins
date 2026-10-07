[英文](README.md) · [官网](https://ffclip.com)

## 快速开始

<sub>复制给 AI 助手，安装后直接体验</sub>

```text
请从 npm 安装最新版 @ffclip-com/videocut，接入当前助手，保留已有配置，并在浏览器打开内置可编辑示例。需要 Node.js 22 或更新版本。如果新工具需要重启才能连接，先用命令行打开示例让我体验。
```

```sh
npm install -g @ffclip-com/videocut@latest && videocut-web --demo --open --port 0
```

需要 Node.js 22+ 和 Chrome 或 Edge。内置作品无需选素材、无需下载模型。打开后可直接播放，也可以对助手说：“把标题改成我的旅行，然后导出视频。”

<details>
<summary>查看编辑器：文字、滤镜、关键帧与转场</summary>

![VideoCut 0.2.13：循环展示文字预设、滤镜、关键帧和关于弹窗](docs/images/videocut-ui.gif)

在时间轴上裁剪和排列视频，调整字幕、效果与关键帧，实时预览并导出 MP4 或 WebM。

左侧“关于”包含官网、微信二维码和作者 B 站主页。编辑器会自动检查新版本，点击“自动更新”后先保存完整作品，再安装官方最新包；安装完成后重启 ffclip 服务或 MCP 连接即可使用新版本。

</details>

[使用说明](docs/usage-zh.md) · [MIT 许可证](LICENSE) · [第三方声明](THIRD_PARTY_NOTICES.md)

本项目早期浏览器音视频实现引用并改编了 [WebAV](https://github.com/WebAV-Tech/WebAV) 开源项目（MIT，Copyright © 2023 风痕），感谢作者及社区贡献者。详见 [开源声明与许可证](THIRD_PARTY_NOTICES.md)。

## 复制提示词，做出自己的作品

下面均为 VideoCut 实际导出的 MP4，可直接播放。安装连接后，把提示词发给助手即可开始创作；换掉文案、颜色和数据，就是你的版本。

### HTML 动画 · 产品开场

```text
用 VideoCut 做一个 6 秒、16:9 的 HTML 产品开场：奶油白背景、珊瑚红点缀，
标题“From idea.”和“To video.”依次上移浮现。右侧三张卡片错位展开、轻轻悬浮，
圆形徽章缓慢旋转。保留可编辑的标题和动画，打开预览并导出 MP4。
```

https://github.com/user-attachments/assets/60a8880e-c636-435c-9dcd-6c2b287854ac

[HTML 源文件](docs/examples/readme/html-intro.html)

继续改：`把标题换成“周末出发”，珊瑚红换成湖蓝色，保留卡片动画。`

### 数据动画 · 让数字动起来

```text
用 VideoCut 做一个 6 秒的 HTML 数据动画：深绿背景、浅绿柱状图，
标题“Small steps. Big momentum.”。Q1 到 Q4 的柱子依次长高，
数字从 0 增长到 24%、48%、72%、96%，左侧大数字停在 96%。明确标注为演示数据，
数值可编辑，打开预览并导出 MP4。
```

https://github.com/user-attachments/assets/7e8fb040-f794-4732-9f98-5d83c013ec94

[HTML 源文件](docs/examples/readme/html-chart.html)

继续改：`把四个数值改成 18、42、65、88，标题换成“这一年的进步”。`

### 本地配音 · 文案变成有声短片

```text
用 VideoCut 的本地中文女声 zf_001，以正常语速朗读：“给灵感一个声音。把文字变成配音，
让字幕跟随节奏。现在，开始你的创作。”制作约 10 秒的短片：米白和橄榄绿配色，标题“让灵感，
成为作品。”，圆形声波随实际音频起伏。生成字幕，按原稿校正文字和分句；配音与字幕保留独立轨道，
打开预览并导出带声音的 MP4。
```

https://github.com/user-attachments/assets/0da34d26-bc46-474d-856a-34a61aef828e

点击播放并取消静音，即可听到本地生成的中文配音。

继续改：`把配音文案换成我的产品介绍，重新生成配音和字幕，保留画面风格。`

配音和语音识别首次使用需要下载模型，之后在本地处理。示例字幕已按原稿校正。

[下载三个可编辑工程](docs/examples/readme/showcase-projects.zip) · [打开方式与示例说明](docs/examples/readme/README-zh.md)
