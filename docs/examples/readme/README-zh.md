# README 示例工程

[English](README.md) · [返回 VideoCut](../../../README-zh.md)

下载并解压 [showcase-projects.zip](showcase-projects.zip)，在 VideoCut 中选择“打开工程”，选中一个完整的 `.vcutweb` 目录：

| 工程 | 时长 | 内容 |
| --- | --- | --- |
| `html-intro.vcutweb` | 6 秒 | 可编辑 HTML 开场和标题变量 |
| `html-chart.vcutweb` | 6 秒 | 可编辑 HTML 柱状图和演示数值 |
| `voiceover.vcutweb` | 10 秒 | HTML 动画、已生成配音、三条独立文字字幕 |

工程已包含所需资源。重新打开或导出现有配音不需要下载模型；生成新配音、识别新音频时需要相应模型。字体使用系统字体，在不同平台上可能有所差异。

本目录中的 HTML 是 VideoCut 动画源文件，由渲染器按指定时间调用 `window.tick`，并非直接打开就会自动播放的网页。配音工程另外保存了声波数据、音频和字幕。

三段 MP4 均通过 VideoCut 0.2.12 以 1280 × 720、24 fps 导出，README 直接嵌入上传至 GitHub 的 MP4 视频附件。配音使用本地 Kokoro `zf_001`，语速为 1。Whisper Base 生成初始识别结果后，按原稿校正文字，并依据实际音频停顿调整字幕分句；这里展示句级字幕，不表示已验证逐字对齐。圆形声波使用实际音频的 RMS 音量数据。

原创动画设计遵循本仓库的 MIT 许可证。模型和运行库声明见 [THIRD_PARTY_NOTICES.md](../../../THIRD_PARTY_NOTICES.md)。
