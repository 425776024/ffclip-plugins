# 本地插件集成

参考 ChatCut 的 [agent-plugin](https://github.com/ChatCut-Inc/agent-plugin) 将编辑服务与宿主适配分开的方向。本项目首先提供本地服务、npm 客户端和 MCP stdio，让 Agent 修改与人工预览共享同一份内存会话。没有复制其远程服务或假定宿主支持 localhost。

## Codex 本地插件

插件模板在 `plugins/videocut-local`：`.codex-plugin/plugin.json`、`.mcp.json`、`skills/video-editing/SKILL.md`。模板默认调用 PATH 中已安装的 `videocut-web`。本地打包脚本将模板转为可独立运行的安装目录：

```sh
npm run build:plugin -- --output "$HOME/plugins/videocut-local" \
  --root /你的素材目录 --root /另一个素材目录 \
  --native-bridge /绝对路径/videocut-bridge
codex plugin add videocut-local@personal
codex plugin list --marketplace personal --json
```

`--root` 可重复；显式传入的素材目录会写进配置，分发到其他机器时需修改为接收方的目录。省略时只授权插件安装目录，不记录构建机器的工作目录。`.vcut` 桥接器可选；普通剪辑/MP4 导出无需原生工程。`--native-bridge` 只接受预编译 Mach-O/ELF/PE 二进制，复制到插件的 `runtime/native/`；带桥接器的包需匹配目标操作系统和架构。本机已通过 Codex 的 `plugin-creator` 脚手架在 `~/.agents/plugins/marketplace.json` 注册 personal 条目，指向 `~/plugins/videocut-local`。新机器需先使用 Codex 的插件创建流程注册自己的本地条目，再执行安装命令；上面的命令不隐式修改用户 marketplace。

安装目录仅包含 `runtime/dist/` 中合并压缩后的页面、CLI、服务、客户端与 API 类型声明，以及包元数据和必要插件配置。不依赖源码 checkout 或运行时下载 npm 依赖。生成的 `.mcp.json` 使用 `command: "node"`、`cwd: "."` 和相对 CLI 路径；Codex 将工作目录解析为安装后的插件目录，因此可搬动或复制到缓存目录。需要 Node 22+，配置的素材目录必须可访问。语音合成需要打开支持 WASM 的浏览器；FFmpeg/FFprobe 仅用于显式启用的媒体回退路径。

npm tarball 不包含构建脚本。需要自行构建时，可从 [公开插件源码仓库](https://github.com/425776024/ffclip-plugins) 获取源码；接收者也可直接安装 npm 包或发布页中的插件安装包。公开仓库由当前插件源码导出，不包含官网源码、私有 Git 历史、本地素材或凭据。插件打包脚本只复制明确列出的构建文件；更新时使用全新临时目录组装后替换旧生成目录，避免历史源码或 source map 残留。未知的已有目录会被拒绝覆盖。

更新本地源码后重新运行打包命令，然后使用 Codex `plugin-creator` 的 `update_plugin_cachebuster.py` 更新安装目录清单版本，再 `codex plugin add videocut-local@personal`，避免命中旧缓存。新对话会加载更新后的 skill 和工具；当前对话不会热加载新工具。

可直接向安装后的 Codex 提出：

> 导入授权目录里的视频、音乐和图片，裁掉视频开头两秒，把中间一段删掉，加上中文标题和右上角图片；打开实时预览并播放。

本轮通过实际 `codex app-server` 的 `mcpServerStatus/list` 确认插件身份和当时的 11 个工具被识别（历史验证；当前工具清单以 tools/list 为准）。随后用 `tests/helpers/plugin-host.mjs` 从**已安装缓存中的 MCP 配置**启动宿主模拟器，真实发送 JSON-RPC 调用并观察 Codex 内置浏览器。没有启动第二个模型对话，也没有把当前对话伪装成已热加载新工具。

## MCP 配置示例

在支持标准 stdio MCP 的本地宿主中使用以下配置（路径替换为实际安装位置）：

```json
{
  "mcpServers": {
    "videocut": {
      "command": "node",
      "args": [
        "/绝对路径/videocut/dist/bin/videocut.mjs",
        "--mcp",
        "--port",
        "0",
        "--root",
        "/你的素材目录",
        "--native-bridge",
        "/绝对路径/videocut-bridge"
      ]
    }
  }
}
```

发布后可改为安装包的 `videocut-web` 命令。Codex 安装目录中的 `.mcp.json` 使用相对路径；WorkBuddy 等本地宿主可以通过自定义 MCP 连接器接入同一个 stdio 服务。若宿主不解析插件目录中的 `cwd`，使用下面的绝对路径配置，路径替换为实际安装目录和素材目录：

```json
{
  "mcpServers": {
    "videocut": {
      "type": "stdio",
      "command": "node",
      "args": [
        "/绝对路径/videocut-local/runtime/dist/bin/videocut.mjs",
        "--mcp",
        "--port",
        "0",
        "--root",
        "/你的素材目录",
        "--tts-model-dir",
        "/你的模型缓存目录"
      ]
    }
  }
}
```

在 WorkBuddy 的连接器设置中添加 stdio MCP；宿主自身的界面与字段以其[官方连接器说明](https://open.workbuddy.cn/docs/connector)为准。这里提供的是通用 MCP 适配，不是将 Codex 的插件 manifest 当作 WorkBuddy 安装包。所有 TTS 工具都通过 `tools/list` 发现，无需宿主访问网页的内部 API。云宿主无法直接读取用户的 localhost，需要单独的远程 MCP/认证设计或宿主支持的本地桥，不应直接暴露本地文件服务。

## 外部宿主调用语音合成

运行环境为 Node 22+ 与支持 WASM 的本地浏览器。WebGPU 可加速；无需 Python、pip 或远程 TTS 账号。模型下载和推理是独立动作：

1. `list_voices` 获取固定模型版本、音色与本机安装状态。
2. 需要下载时显式调用 `install_tts_model({dtype:"fp32",voices:["zf_001"]})`。首次约 339 MB 模型加一个约 522 KB 音色；使用 `get_tts_model_status` 轮询，`cancel_tts_model_install` 取消。模型会校验并缓存，可跨 MCP 重启复用。
3. `create_session` 或使用已有会话，打开返回的 `previewUrl`；读取 `get_session` 的最新 `version`。
4. 调用 `synthesize_speech({id,version,text:"你好，欢迎使用 VideoCut。",voice:"zf_001",speed:1,backend:"auto",startSeconds:0})`。返回任务，不阻塞其他 MCP 调用，也不自动安装模型。
5. 轮询 `get_tts_status({id})`。`completed` 返回实际 backend、普通 WAV `asset` 和插入结果。`cancel_tts({id})` 中止任务；页面关闭或执行器失联也会终止未完成任务。
6. 合成期间版本改变会返回 `conflict` 并保留生成文件。审阅当前会话，再通过 `add_media` 插入一次。`insert:false` 可只生成文件，不修改作品。

网页“音频 → 语音合成”使用同一协议。多窗口只有一个执行器领取任务；模型和 JS/WASM runtime 都经同源本地服务读取。完成安装后，推理不发送文案到网络。WAV 生成不代表已听到扬声器声音，字幕词级时间也需要独立对齐。

当前开放已实测的 FP32 模型，支持自动选择、WebGPU 和 WASM。当前固定版本的 FP16/INT8 导出在实测中返回了非有限音频，因此不提供这些精度的安装或合成选项。模型下载始终由显式安装操作触发。

本地调用已实现。英文音素前端使用 MIT HeadTTS JavaScript 规则和 Apache-2.0 Misaki 英美词典，不再分发旧 phonemizer/eSpeak 引擎。精确来源、哈希、修改和许可证随包附在 `tts-runtime/ENGLISH-SOURCE.json`、`NOTICE.txt` 与 `DISTRIBUTION.txt`。词典使用默认读音，未匹配词使用规则回退；不包含上下文词性模型。

协议支持版本 `2024-11-05`，stdio 每行一条 JSON-RPC。初始化返回工具能力。`tools/call` 的结果包含 JSON 文本；工具业务错误返回 `isError: true`。退出 stdin 会关闭本地服务。见 [MCP stdio](https://modelcontextprotocol.io/specification/2024-11-05/basic/transports)。

## 推荐调用顺序

1. `create_session({name:"我的剪辑作品"})`：得到内部 `id`、`version`、空白 `project`、按作品名称生成的 `previewUrl` 和 `projectPath`。预览地址形如 `/projects/我的剪辑作品`，同名作品加数字后缀。继续已有作品时调用 `list_sessions`，按名称、预览地址或 `.vcutweb` / `.vcut` 目录查找；多个匹配需要明确选定作品。
2. `list_files`：只列出授权目录内的子目录和可识别媒体。
3. 打开 `previewUrl`，再 `add_media` / `add_text`：添加片段并取得新快照，已打开的浏览器通过 SSE 同步更新。
4. `get_session` → `edit_timeline`：传入刚读取的 version 和 operations，一批操作全部成功才提交。冲突后重新读取，不能强制覆盖。高级调用者也可用 `update_session` 替换完整模型。
5. `preview_control` → `get_preview_status`：播放、暂停或定位后，检查浏览器回报的 `version`、`commandSequence`、实际媒体时间和视频帧数。`deliveredTo` 仅代表命令已送达。状态保留 15 秒，首次有声播放受浏览器策略限制时需要点击一次播放。
6. 用户需要带走结果时 `export_project` / `render_video`：传入已审阅版本，写入新的唯一目录/文件，不覆盖原作品。

`add_text` 支持 `content`、`start`、`length`、`fontSize`、`color`。该工具和完整作品模型的时间使用整数 ticks，一秒 = 120000 ticks；颜色为 `#RRGGBB`。`edit_timeline` 中以 `Seconds` 结尾的字段及预览的 `timeSeconds` 则使用普通秒数。

`edit_timeline` 支持 `configure_project`、`trim_clip`、`split_clip`、`move_clip`、`remove_clip`、`set_transform`、`set_audio`、`set_text`、`reorder_track`。分割返回右侧新片段 ID；裁剪选择源入点和持续时间，保留时间轴起点；第 0 条轨道位于最上层，图片叠加需放在视频上方。

```json
{
  "id": "create_session 返回的 id",
  "version": 3,
  "operations": [
    {
      "action": "trim_clip",
      "itemId": "返回的片段 id",
      "sourceInSeconds": 2,
      "durationSeconds": 8
    },
    { "action": "split_clip", "itemId": "返回的片段 id", "atSeconds": 4 }
  ]
}
```

## Node 服务 API

```js
import { startServer } from 'videocut-local/server';
const server = await startServer({ port: 0, roots: ['/你的素材目录'] });
console.log(server.url);
await server.close();
```

客户端从同源 `/api/bootstrap` 取得 token，后续请求使用 Bearer token；媒体和 SSE 使用 URL token，以支持原生 HTML 媒体元素。不要记录或分享带 token 的内部媒体 URL。分享预览使用按作品名称生成的 `previewUrl`，其中不包含内部 session id 或 token，且只能由本机访问。作品重命名后返回新地址，旧名称地址在同一服务生命周期内仍定位原作品；旧 `/?session=…` 地址也继续支持，页面连接后自动更新为名称地址。

`client.listSessions()` 返回每个当前作品的名称、内部 `id`、版本、预览地址和 `projectPath`；`client.resolveSession(new URL(previewUrl).pathname)` 解析名称地址并取得快照。内部 `id` 继续用于编辑事务、媒体与事件协议。`projectPath` 只在实际打开或保存 `.vcutweb` / `.vcut` 后返回本地绝对目录，未保存作品为 null。服务重启后可用 `open_project({path:projectPath})` 恢复已保存作品；名称地址本身不承担磁盘保存。

会话 JSON 是 Web 编辑 API 的数据传输格式，不是原生磁盘格式。当前 `.vcut` 是桌面 Project Format 1 目录，其中快照与索引采用二进制存储，必须通过桥接器保存/打开；不会写 web-session.json。视频导出需要保持作品网页打开，统一画布渲染后由浏览器编码或复用原有 FFmpeg。npm 包不包含桥接源码、构建脚本、本机静态库、私钥、素材、会话、测试产物或已编译桥接器；使用方通过 `--native-bridge` 指向单独分发的预编译程序。Codex 插件可在构建时选择包含该二进制。

## MCP 语音识别与字幕

ASR 仅提供 MCP 与 Skill 入口，固定使用 [Whisper Base](https://huggingface.co/onnx-community/whisper-base_timestamped)，不在编辑器展示识别或模型设置。首次识别自动下载固定 revision 的约 209 MB 文件，验证大小、SHA256 后写入持久缓存。缓存路径通过 CLI `--asr-model-dir` 或 `VIDEOCUT_ASR_MODEL_DIR` 配置。JS/WASM runtime 随安装包提供，模型不随包分发。音频不发往外网，识别在打开的本地预览页 Worker 中执行。

1. 打开现有会话 `previewUrl`，调用 `get_session` 获取最新版本。
2. 调用 `transcribe_speech({id,version,itemId,language:"zh"})`；所选音视频片段须有音轨。也可用 `assetId` 识别整个素材。两者只填一个，没有模型参数。
3. 调用 `get_asr_status({id})` 查看下载、解码、识别、完成或错误。默认 `backend:"auto"` 优先 WebGPU，失败时重跑 WASM。`cancel_asr({id})` 可取消任务和无其他使用者的模型下载。`get_asr_model_status({})` 只查看缓存，不下载。
4. 默认 `insert:true` 在一条文字轨生成普通字幕片段，一次撤销可移除整批。`insert:false` 只返回 `text` 和 `segments`，不修改作品。
5. 片段字幕按 source 裁剪、timeline begin 和恒定速率映射；素材字幕默认从 0 秒开始，可设 `startSeconds`。返回的 `segments:[{text,start,end}]` 均是作品时间秒数。
6. 若识别期间作品版本变化，返回 `conflict` 并保留结果；读取最新版本，确认时间位置，再用 `edit_timeline({id,version,operations:[{action:"add_subtitles",segments}]})` 插入。

无浏览器时返回 `BROWSER_REQUIRED` 与预览地址。关闭最后一页会取消未完成识别。源音频单任务最多 60 分钟；倒放片段应改用原素材。静音结果为空，不生成字幕轨。时间戳来自模型，不根据字数估算。

## 本地视觉理解

`visual-understanding` Skill 与以下 MCP 工具使用同一模型管理和任务接口：

| 工具                          | 用途                                                    |
| ----------------------------- | ------------------------------------------------------- |
| `get_vision_model_status`     | 读取保存的启用选择、已校验模型及逐文件字节进度；不下载  |
| `request_vision_setup`        | 在打开的预览页显示初始化对话框；无页面时返回 `setupUrl` |
| `describe_image`              | 输入 `path`、`prompt`，返回按要求描述的 `text`           |
| `describe_video`              | 自动采样，返回带源起止时间的区间描述与实际帧时间        |
| `analyze_media`               | 对授权图片或采样视频帧进行只读分析                      |
| `get_vision_status`           | 读取任务进度、文本、区间／逐帧描述及实际时间戳          |
| `cancel_vision`               | 取消任务并终止浏览器 Worker                             |
| `cancel_vision_model_install` | 取消下载，保留已校验文件和启用选择                      |

初始化时由用户决定是否引入视觉理解。未同意时，查询、分析及请求显示对话框均不下载模型；同意后才从固定 revision 下载约 1.1 GB 的 FastVLM 文件，检查大小和 SHA256。拒绝会持久保存。编辑页只展示初始化确认及下载状态，没有视觉分析面板或常驻入口。`--vision-model-dir` / `VIDEOCUT_VISION_MODEL_DIR` 配置持久缓存。模型许可是 Apple AMLR；许可文件与模型一并校验，确认对话框提供原始许可链接。

```js
const status = await client.visionModelStatus();
// 未初始化时打开 session.previewUrl，由用户在对话框决定；不要通过 API 代替用户同意。
// 用户明确要求重新启用或重试时：await client.requestVisionSetup();
const image = await client.describeImage(
  '/authorized/media/photo.png',
  '用中文描述主体和背景，返回 JSON，字段为 subject 和 background。',
  { maxNewTokens: 256 }
);
if (image.state === 'completed') console.log(image.text);

const video = await client.describeVideo(
  '/authorized/media/video.mp4',
  '用中文描述人物动作与场景变化，每段不超过两句话。',
  { beginSeconds: 10, endSeconds: 30, segmentSeconds: 5,
    maxSegments: 12, framesPerSegment: 3, timeoutMs: 30000 }
);
// 未完成时，使用精确任务 ID 继续等或查询，避免读取另一个任务。
const result = ['queued', 'running'].includes(video.state)
  ? await client.waitVision(video.sessionId, video.id, { timeoutMs: 60000 }) : video;
if (result.state === 'completed') {
  console.log(result.text); // [10.000–15.000s] 人物走过街道。...
  console.log(result.segments); // [{startSeconds,endSeconds,text,samples}, ...]
}
// await client.visionStatus(video.sessionId, video.id);
// await client.cancelVision(video.sessionId, video.id);
```

这两个便捷函数只需绝对路径和非空 prompt，不需作品版本；可通过 `options.id` 指定会话，省略时使用最近活跃的已打开预览。HTTP 对应 `POST /api/vision/describe-image`、`POST /api/vision/describe-video`，JSON 包含 `path`、`prompt` 及采样选项；HTTP 返回 `202` 任务，等待由客户端完成。MCP 对应 `describe_image` / `describe_video`，等待参数是 `waitSeconds`（默认 30、范围 0–60）；JS 使用 `timeoutMs`（默认 30000、范围 0–60000）。`0` 立即返回任务，等待超时返回 `waitTimedOut:true`，任务继续执行。MCP 查询和取消使用 `{id: result.sessionId, jobId: result.id}`，每会话保留最近 16 个任务，已释放的任务返回 `VISION_JOB_NOT_FOUND`。JS `signal: AbortSignal` 终止等待并取消该任务。

`prompt` 最多 4000 字符，指定关注内容、语言和输出格式；图片 `text` 或每个区间的 `segments[].text` 保留模型文本。请求 JSON 时仍须校验模型输出，接口不保证模型生成的 JSON 合法。`maxNewTokens` 默认 192、范围 1–512，分别限制每张图片或每个区间的输出。视频顶层 `text` 将区间描述拼成带源时间的文本：

```text
[10.000–15.000s] 人物走过街道，背景是沿街商铺。
[15.000–20.000s] 画面转为近景，人物面向镜头。
```

视频默认覆盖完整原素材，可选 `beginSeconds` / `endSeconds` 限定源时间范围（每次最多 60 分钟）。`segmentSeconds` 为目标区间秒数（默认 5，范围 1–600），`maxSegments` 为区间上限（默认 12，范围 1–32），`framesPerSegment` 为每区间中点采样帧数（默认 3，范围 1–4）。按所选范围均分区间，区间上限不足时扩大区间以覆盖全范围；`sampling.intervalSeconds`、`totalSamples`、`coarsened` 报告实际密度。每个区间把有时间标签的采样画面按顺序组合后送入图像模型，返回 `startSeconds`、`endSeconds`、`text`、`samples`；样本包含请求时间 `requestedSeconds`、实际源帧时间 `sourceSeconds`、帧持续时长 `durationSeconds`。这些边界是采样区间，并非模型检测到的剪切点。

需要素材／片段 ID 或逐帧描述时，使用下层接口：

```js
const session = await client.getSession(sessionId);
const job = await client.analyzeMedia(session.id, {
  version: session.version,
  path: '/authorized/media/video.mp4', // 或 assetId / itemId，三选一
  beginSeconds: 10,
  endSeconds: 30,
  maxFrames: 8,
  prompt: 'Describe visible subjects, actions and scene changes.',
  backend: 'auto'
});
const frames = await client.visionStatus(session.id, job.id); // 轮询至 completed / error / cancelled
```

保持 `previewUrl` 打开，推理在本地浏览器 Worker 执行，资源不从 CDN 或云推理服务加载。`VISION_CONSENT_REQUIRED`、`VISION_MODEL_REQUIRED`、`BROWSER_REQUIRED` 返回恢复信息。任务在关闭预览、关闭会话或服务退出时取消。素材路径仍受启动时 `--root` 约束；采样期间原文件被替换时返回 `VISION_SOURCE_CHANGED`。分析不修改作品版本。

`analyze_media` 图像返回一帧描述；视频默认均匀抽取八个中点，最多 32 帧、60 分钟。`beginSeconds` / `endSeconds` 是原素材时间，片段请求默认使用裁剪后的源区间。每帧结果包括 `requestedSeconds`、实际解码的 `sourceSeconds`、`durationSeconds` 和 `text`；片段结果另含按位置与速度映射的 `timelineSeconds`，针对任务保存的作品 `version`。编辑期间发生的新版本不会改变已有结果的对应关系。区间和逐帧描述均依据采样画面，不读取音频，也不证明采样点之间的连续动作或所有剪切点；声音理解使用 `transcribe_speech`。

## 完整 Web 工程

0.2.0 新增 `save_project({id,version,directory})` / `client.saveProject(id,version,directory)`，保存独立 `.vcutweb` 目录。`open_project` / `client.openProject` / CLI `--project` 支持两个格式。Web 保存无需原生桥，包含本地媒体、完整 HTML 源码与变量、花字 recipe、效果/关键帧、轨道关系和冻结的模板资源。包内媒体使用相对路径及 SHA256 清单；移动整个目录或重启服务后仍能编辑和导出，原始素材可删除。每次保存生成新目录；现有目录不覆盖。

`.vcutweb` 与原生 Format 1 独立。新版原生桥能力 `customTextRecipes:1` 支持自定义花字往返，描述嵌入规范文档的 template origin，并校验文档指纹和模板摘要，不附加 web-session.json。HTML 需要 Web 保存。系统字体不打包；明确选择的字体缺失时需要在目标机器安装。
