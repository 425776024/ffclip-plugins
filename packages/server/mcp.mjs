import {
  PROPERTY_DESCRIPTORS,
  propertyInputSchema,
  descriptorInputSchema,
  TEXT_TEMPLATES,
  TEMPLATE_PARTS,
  TEMPLATE_COMPOSITION_RULES,
  EFFECT_TEMPLATES,
  TRANSITION_TEMPLATES,
  findItem,
  getPropertyDescriptor,
  isPropertyApplicable,
  sampleProperty,
  ticks,
  seconds
} from '../core/project.mjs';
import { createProject } from '../core/project.mjs';
import { createInterface } from 'node:readline';
import { readFileSync } from 'node:fs';
import { VideoCutClient } from '../client/index.mjs';
const { name: packageName, version: packageVersion } = JSON.parse(
  readFileSync(new URL('../../package.json', import.meta.url), 'utf8')
);

// Line-delimited MCP JSON-RPC over stdio. stdout is reserved for protocol messages.
export async function serveMcp(url, close) {
  const client = new VideoCutClient(url);
  await client.connect();
  const object = (properties) => ({ type: 'object', properties, additionalProperties: false });
  const string = { type: 'string' },
    number = { type: 'number' },
    boolean = { type: 'boolean' };
  const operation = (action, properties, required = []) => ({
    ...object({ action: { const: action }, ...properties }),
    required: ['action', ...required]
  });
  const item = { itemId: string, expandLinked: boolean };
  const ids = { itemIds: { type: 'array', items: string, minItems: 1 }, expandLinked: boolean };
  const parameters = { type: 'object', additionalProperties: { type: ['number', 'string'] } };
  const htmlContent = {
    ...object({
      html: { type: 'string', maxLength: 1048576 },
      width: { type: 'integer', minimum: 1, maximum: 4096 },
      height: { type: 'integer', minimum: 1, maximum: 4096 },
      duration: { type: 'integer', minimum: 1, maximum: 10368000000 },
      transparent: boolean,
      variables: {
        type: 'object',
        maxProperties: 100,
        additionalProperties: { type: ['string', 'number', 'boolean'] }
      }
    }),
    required: ['html', 'width', 'height', 'duration', 'transparent']
  };
  const textTemplate = {
    ...object({
      id: { type: 'string', pattern: '^[a-zA-Z0-9._-]{1,120}$' },
      version: { const: 1 },
      style: object({
        color: propertyInputSchema('text.color'),
        fontSize: propertyInputSchema('text.fontSize')
      }),
      recipe: {
        ...object(
          Object.fromEntries(
            Object.entries(TEMPLATE_PARTS).map(([key, values]) => [key, { enum: values }])
          )
        ),
        required: ['base']
      }
    }),
    required: ['id', 'version']
  };
  const propertySchema = Object.fromEntries(
    Object.keys(PROPERTY_DESCRIPTORS).map((key) => [key, propertyInputSchema(key)])
  );
  const templateParameters = (template) =>
    object(
      Object.fromEntries(
        Object.entries(template.parameters).map(([key, descriptor]) => [
          key,
          descriptorInputSchema({ label: key, ...descriptor })
        ])
      )
    );
  const subtitleSegments = {
    type: 'array',
    minItems: 1,
    maxItems: 10000,
    items: {
      ...object({
        text: { type: 'string', minLength: 1, maxLength: 1000 },
        start: { type: 'number', minimum: 0 },
        end: { type: 'number', maximum: 86400 }
      }),
      required: ['text', 'start', 'end']
    }
  };
  const operations = [
    operation('add_subtitles', { segments: subtitleSegments, name: string }, ['segments']),
    operation(
      'add_text',
      {
        content: propertySchema['text.content'],
        startSeconds: number,
        durationSeconds: number,
        fontSize: propertySchema['text.fontSize'],
        color: propertySchema['text.color'],
        template: textTemplate
      },
      []
    ),
    operation(
      'add_html_clip',
      {
        html: htmlContent,
        name: string,
        start: { type: 'integer' },
        length: { type: 'integer' },
        trackId: string
      },
      ['html']
    ),
    operation('set_html_clip', { itemId: string, html: htmlContent, name: string }, [
      'itemId',
      'html'
    ]),
    operation('undo', {}),
    operation('redo', {}),
    operation('configure_project', {
      name: string,
      width: number,
      height: number,
      fps: number,
      frameRate: object({ numerator: number, denominator: number })
    }),
    operation(
      'trim_clip',
      {
        ...item,
        sourceInSeconds: propertyInputSchema('time.sourceIn', { seconds: true }),
        durationSeconds: propertyInputSchema('time.duration', { seconds: true })
      },
      ['itemId', 'sourceInSeconds', 'durationSeconds']
    ),
    operation(
      'move_clip',
      {
        ...item,
        startSeconds: propertyInputSchema('time.start', { seconds: true }),
        trackId: string
      },
      ['itemId', 'startSeconds']
    ),
    operation(
      'split_clip',
      { ...item, atSeconds: propertyInputSchema('time.start', { seconds: true }) },
      ['itemId', 'atSeconds']
    ),
    operation('remove_clip', item, ['itemId']),
    operation(
      'set_transform',
      {
        ...item,
        ...Object.fromEntries(
          [
            'positionX',
            'positionY',
            'scaleX',
            'scaleY',
            'rotationDegrees',
            'opacity',
            'anchorX',
            'anchorY'
          ].map((key) => [key, propertySchema[`visual.${key}`]])
        ),
        crop: object(
          Object.fromEntries(
            ['left', 'top', 'right', 'bottom'].map((key) => [
              key,
              propertySchema[`visual.crop.${key}`]
            ])
          )
        ),
        fitPolicy: propertySchema['visual.fitPolicy'],
        blendMode: propertySchema['visual.blendMode'],
        flipHorizontal: boolean,
        flipVertical: boolean
      },
      ['itemId']
    ),
    operation(
      'set_audio',
      {
        ...item,
        ...Object.fromEntries(
          ['gainLinear', 'muted', 'fadeIn', 'fadeOut'].map((key) => [
            key,
            propertySchema[`audio.${key}`]
          ])
        )
      },
      ['itemId']
    ),
    operation(
      'set_text',
      {
        ...item,
        template: { anyOf: [textTemplate, { type: 'null' }] },
        layoutWidth: { anyOf: [{ type: 'number', minimum: 1, maximum: 65536 }, { type: 'null' }] },
        templateStyle: object({
          color: { anyOf: [propertyInputSchema('text.color'), { type: 'null' }] },
          fontSize: { anyOf: [propertyInputSchema('text.fontSize'), { type: 'null' }] }
        }),
        ...Object.fromEntries(
          ['content', 'fontSize', 'color'].map((key) => [key, propertySchema[`text.${key}`]])
        )
      },
      ['itemId']
    ),
    operation('reorder_track', { ...item, index: { type: 'integer' } }, ['itemId', 'index']),
    operation(
      'trim_range',
      {
        ...item,
        beginSeconds: propertyInputSchema('time.start', { seconds: true }),
        endSeconds: propertyInputSchema('time.start', { seconds: true })
      },
      ['itemId', 'beginSeconds', 'endSeconds']
    ),
    operation(
      'move_clips',
      { ...ids, deltaSeconds: number, trackId: string, expandLinked: boolean },
      ['itemIds', 'deltaSeconds']
    ),
    operation('duplicate_clips', { ...ids, offsetSeconds: number }, ['itemIds']),
    ...['group_clips', 'link_clips', 'delete_clips'].map((action) =>
      operation(action, ids, ['itemIds'])
    ),
    ...['ungroup_clips', 'unlink_clips'].map((action) =>
      operation(action, { groupId: string }, ['groupId'])
    ),
    operation('ripple_delete', { ...ids, scope: { enum: ['selected', 'all', 'syncLocked'] } }, [
      'itemIds'
    ]),
    operation('set_speed', { ...item, rate: propertySchema['time.speed'], ripple: boolean }, [
      'itemId',
      'rate'
    ]),
    operation(
      'set_track',
      {
        trackId: string,
        visible: boolean,
        muted: boolean,
        locked: boolean,
        syncLocked: boolean,
        name: string
      },
      ['trackId']
    ),
    ...Object.entries(PROPERTY_DESCRIPTORS).map(([property]) =>
      operation(
        'set_property',
        { ...item, property: { const: property }, value: propertySchema[property] },
        ['itemId', 'property', 'value']
      )
    ),
    ...Object.entries(PROPERTY_DESCRIPTORS)
      .filter(([, descriptor]) => descriptor.keyframe)
      .map(([property]) =>
        operation(
          'set_keyframe',
          {
            ...item,
            property: { const: property },
            value: propertySchema[property],
            timeSeconds: propertyInputSchema('time.start', { seconds: true }),
            interpolation: { enum: ['linear', 'hold', 'easeIn', 'easeOut', 'easeInOut'] }
          },
          ['itemId', 'property', 'timeSeconds', 'value']
        )
      ),
    operation(
      'set_keyframe',
      {
        ...item,
        property: {
          type: 'string',
          pattern: '^effects\\..+\\.amount$',
          description: 'LUT instance intensity; use get_properties for this item.'
        },
        value: descriptorInputSchema(
          EFFECT_TEMPLATES.find((template) => template.id === 'lut').parameters.amount
        ),
        timeSeconds: propertyInputSchema('time.start', { seconds: true }),
        interpolation: { enum: ['linear', 'hold', 'easeIn', 'easeOut', 'easeInOut'] }
      },
      ['itemId', 'property', 'timeSeconds', 'value']
    ),
    operation('remove_keyframe', { ...item, property: string, keyframeId: string }, [
      'itemId',
      'property',
      'keyframeId'
    ]),
    ...EFFECT_TEMPLATES.map((template) =>
      operation(
        'add_effect',
        { ...item, templateId: { const: template.id }, parameters: templateParameters(template) },
        ['itemId', 'templateId']
      )
    ),
    operation('update_effect', { ...item, effectId: string, enabled: boolean, parameters }, [
      'itemId',
      'effectId'
    ]),
    operation('remove_effect', { ...item, effectId: string }, ['itemId', 'effectId']),
    operation('reorder_effects', { ...item, effectIds: { type: 'array', items: string } }, [
      'itemId',
      'effectIds'
    ]),
    ...TRANSITION_TEMPLATES.map((template) =>
      operation(
        'add_transition',
        {
          fromItemId: string,
          toItemId: string,
          templateId: { const: template.id },
          durationSeconds: number,
          parameters: templateParameters(template)
        },
        ['fromItemId', 'toItemId', 'templateId']
      )
    ),
    operation('remove_transition', { transitionId: string }, ['transitionId']),
    operation('update_transition', { transitionId: string, durationSeconds: number, parameters }, [
      'transitionId'
    ])
  ];
  const tools = [
    {
      name: 'initialize_demo',
      description:
        'Initialize the bundled 18-second editable starter project with HTML animation, native styled titles, packaged voiceover and timed captions. No media selection, model download or synthesis is needed. Creates a separate session; open its returned previewUrl automatically with the host browser tools and keep that same session for further edits. locale selects Chinese or English content.',
      inputSchema: object({
        locale: { enum: ['zh', 'en'] },
        name: { type: 'string', minLength: 1, maxLength: 200 }
      })
    },
    {
      name: 'open_project',
      description:
        'Open a portable .vcutweb or native Project Format 1 .vcut DIRECTORY in a new live editing session. .vcutweb retains HTML source/variables, editable recipes, media and frozen template resources, and can be moved or reopened after service restart.',
      inputSchema: { ...object({ path: string }), required: ['path'] }
    },
    {
      name: 'get_properties',
      description:
        'Read shared property descriptors, supported effect/transition parameters and current values. Local time is in seconds.',
      inputSchema: {
        ...object({ id: string, itemId: string, timeSeconds: number }),
        required: ['id']
      }
    },
    {
      name: 'get_render_status',
      description: 'Read current or most recent export progress, failure and final file.',
      inputSchema: { ...object({ id: string }), required: ['id'] }
    },
    {
      name: 'cancel_render',
      description: 'Cancel the active export and clean temporary output.',
      inputSchema: { ...object({ id: string }), required: ['id'] }
    },
    {
      name: 'transcribe_speech',
      description:
        'Recognize local audio/video with fixed Whisper Base in the open preview browser, using WebGPU with WASM fallback. The first job automatically downloads and SHA256-verifies Base (~209 MB) into persistent local storage; later jobs reuse it. No model option or separate installation step. Supply the latest session version and exactly one itemId (trimmed clip, with timeline/rate alignment) or assetId (whole source). insert defaults true and adds editable captions on one text track in a single undo step; insert:false returns timed text only. Keep previewUrl open and poll get_asr_status. On version conflict, timed segments are retained for edit_timeline/add_subtitles with the fresh version. Audio stays local. Source audio is limited to 60 minutes per job.',
      inputSchema: {
        ...object({
          id: string,
          version: { type: 'integer', minimum: 0 },
          itemId: string,
          assetId: string,
          language: { enum: ['auto', 'zh', 'en', 'ja', 'ko', 'fr', 'de', 'es', 'ru'] },
          backend: { enum: ['auto', 'webgpu', 'wasm'] },
          insert: boolean,
          startSeconds: { type: 'number', minimum: 0, maximum: 86400 }
        }),
        required: ['id', 'version'],
        oneOf: [
          { required: ['itemId'], not: { required: ['assetId'] } },
          { required: ['assetId'], not: { required: ['itemId'] } }
        ]
      }
    },
    {
      name: 'get_asr_status',
      description:
        'Read current ASR job including automatic download progress, recognition state, actual backend, text, timeline-second segments and inserted itemIds. Empty speech completes with no captions. A conflict retains text without changing the project.',
      inputSchema: { ...object({ id: string }), required: ['id'] }
    },
    {
      name: 'cancel_asr',
      description:
        'Cancel this session ASR job, stop browser inference and its automatic model download if no other job needs it. Verified cached files remain reusable.',
      inputSchema: { ...object({ id: string }), required: ['id'] }
    },
    {
      name: 'get_asr_model_status',
      description:
        'Read the fixed Whisper Base persistent cache status, size, revision and current download progress. Does not download or select models.',
      inputSchema: object({})
    },
    {
      name: 'get_vision_model_status',
      description:
        'Read FastVLM consent, verified local model installation and byte progress. Never downloads. Enablement belongs to the initialization dialog.',
      inputSchema: object({})
    },
    {
      name: 'request_vision_setup',
      description:
        'Show the initialization consent/download dialog in open previews; returns setupUrl. Open that URL if no preview is open. This does not approve or download the model. Respect a prior decline; request again only when the user asks to enable vision.',
      inputSchema: object({})
    },
    {
      name: 'describe_image',
      description:
        'Describe a local image from an authorized absolute path according to prompt (language, focus and output format). Optional id selects a session; otherwise uses the latest open preview. User consent and installed FastVLM are required; never downloads. Waits up to waitSeconds (default 30, max 60). Completed result.text is the description; pending results include sessionId/id for get_vision_status. Read-only.',
      inputSchema: {
        ...object({
          path: string,
          prompt: { type: 'string', minLength: 1, maxLength: 4000 },
          id: string,
          maxNewTokens: { type: 'integer', minimum: 1, maximum: 512 },
          backend: { enum: ['auto', 'webgpu', 'wasm'] },
          waitSeconds: { type: 'number', minimum: 0, maximum: 60, default: 30 }
        }),
        required: ['path', 'prompt']
      }
    },
    {
      name: 'describe_video',
      description:
        'Describe local video intervals from an authorized absolute path according to prompt. Automatically divides the selected source range into intervals, samples ordered frames per interval and describes each storyboard. Completed result contains timestamped text, segments with startSeconds/endSeconds/text/samples, and sampling density. Defaults: 5s target intervals, max 12 intervals, 3 frames each; long ranges widen intervals to cover the range. At most 60 minutes, 32 intervals and 4 frames each. Optional id selects a preview; otherwise latest open preview. User consent/installed FastVLM/open preview required. Waits up to 30s by default; poll pending jobs by sessionId/id. Visual samples only; no audio or editing.',
      inputSchema: {
        ...object({
          path: string,
          prompt: { type: 'string', minLength: 1, maxLength: 4000 },
          id: string,
          beginSeconds: { type: 'number', minimum: 0 },
          endSeconds: { type: 'number', exclusiveMinimum: 0 },
          segmentSeconds: { type: 'number', minimum: 1, maximum: 600, default: 5 },
          maxSegments: { type: 'integer', minimum: 1, maximum: 32, default: 12 },
          framesPerSegment: { type: 'integer', minimum: 1, maximum: 4, default: 3 },
          maxNewTokens: { type: 'integer', minimum: 1, maximum: 512 },
          backend: { enum: ['auto', 'webgpu', 'wasm'] },
          waitSeconds: { type: 'number', minimum: 0, maximum: 60, default: 30 }
        }),
        required: ['path', 'prompt']
      }
    },
    {
      name: 'analyze_media',
      description:
        'Read-only local FastVLM image/video understanding. Supply exactly one authorized absolute path, assetId or itemId and the current session version. Open previewUrl and keep it open. Model must be enabled by the user and fully installed. Video is midpoint sampled, not continuous video understanding; returns per-frame descriptions and actual source/timeline seconds. beginSeconds/endSeconds are source seconds. No audio analysis or timeline edits. Poll get_vision_status.',
      inputSchema: {
        ...object({
          id: string,
          version: { type: 'integer', minimum: 0 },
          path: string,
          assetId: string,
          itemId: string,
          prompt: { type: 'string', minLength: 1, maxLength: 4000 },
          beginSeconds: { type: 'number', minimum: 0 },
          endSeconds: { type: 'number', exclusiveMinimum: 0 },
          maxFrames: { type: 'integer', minimum: 1, maximum: 32 },
          maxNewTokens: { type: 'integer', minimum: 1, maximum: 512 },
          backend: { enum: ['auto', 'webgpu', 'wasm'] }
        }),
        required: ['id', 'version'],
        oneOf: [
          {
            required: ['path'],
            not: { anyOf: [{ required: ['assetId'] }, { required: ['itemId'] }] }
          },
          {
            required: ['assetId'],
            not: { anyOf: [{ required: ['path'] }, { required: ['itemId'] }] }
          },
          {
            required: ['itemId'],
            not: { anyOf: [{ required: ['path'] }, { required: ['assetId'] }] }
          }
        ]
      }
    },
    {
      name: 'get_vision_status',
      description:
        'Read vision state, progress, text, timestamped segments/frames or error. id is the sessionId; optional jobId reads that exact task (recent 16 retained).',
      inputSchema: { ...object({ id: string, jobId: string }), required: ['id'] }
    },
    {
      name: 'cancel_vision',
      description:
        'Cancel a vision task and terminate its browser worker. id is the sessionId; optional jobId cancels that exact task without affecting a newer one.',
      inputSchema: { ...object({ id: string, jobId: string }), required: ['id'] }
    },
    {
      name: 'cancel_vision_model_install',
      description:
        'Cancel the vision model download, preserving verified files and the saved consent.',
      inputSchema: object({})
    },
    {
      name: 'list_voices',
      description:
        'List Kokoro Chinese/English voices, installed local model variants and voice files. Speech inference runs in the open browser via WebGPU/WASM; no Python or cloud synthesis.',
      inputSchema: object({})
    },
    {
      name: 'install_tts_model',
      description:
        'Explicitly download a pinned Kokoro model and selected voices into persistent local storage. This requires internet for installation only. Returns immediately; poll get_tts_model_status. Default fp32 and zf_001. Existing verified files are reused.',
      inputSchema: object({
        dtype: { enum: ['fp32'] },
        voices: { type: 'array', minItems: 1, maxItems: 103, items: string }
      })
    },
    {
      name: 'get_tts_model_status',
      description:
        'Read local Kokoro model installation progress or failure. Does not download anything.',
      inputSchema: object({})
    },
    {
      name: 'cancel_tts_model_install',
      description:
        'Cancel the current Kokoro model download and remove partial files; preserve verified models.',
      inputSchema: object({})
    },
    {
      name: 'synthesize_speech',
      description:
        'Queue local Chinese/English speech synthesis in the session browser and optionally insert a normal WAV audio clip (insert defaults true). Open previewUrl and keep it open. Requires an explicitly installed model/voice; never downloads automatically. Returns a job immediately: poll get_tts_status. Supply the latest session version. On conflict the generated asset is retained; import its path with add_media after reviewing the current session. startSeconds is seconds.',
      inputSchema: {
        ...object({
          id: string,
          version: { type: 'integer', minimum: 0 },
          text: { type: 'string', minLength: 1, maxLength: 8000 },
          voice: string,
          speed: { type: 'number', minimum: 0.5, maximum: 2 },
          backend: { enum: ['auto', 'webgpu', 'wasm'] },
          dtype: { enum: ['fp32'] },
          startSeconds: { type: 'number', minimum: 0, maximum: 86400 },
          trackId: string,
          insert: boolean
        }),
        required: ['id', 'version', 'text']
      }
    },
    {
      name: 'get_tts_status',
      description:
        'Read queued/running/completed/conflict/cancelled/error speech job, actual browser backend and the generated WAV asset/path. Completion confirms file generation, not audible playback.',
      inputSchema: { ...object({ id: string }), required: ['id'] }
    },
    {
      name: 'cancel_tts',
      description:
        'Cancel this session speech task, terminate browser inference and discard unfinished output.',
      inputSchema: { ...object({ id: string }), required: ['id'] }
    },
    {
      name: 'create_session',
      description:
        'Create a blank VideoCut project with an optional readable name; returns the internal session id, a name-based live preview URL and projectPath (null until saved).',
      inputSchema: object({ name: { type: 'string', minLength: 1, maxLength: 200 } })
    },
    {
      name: 'list_sessions',
      description:
        'List live VideoCut cuts by readable name, preview URL and actual saved .vcutweb or .vcut directory (projectPath). Use this to find a cut the user wants to continue, then pass its internal id to editing tools. Never choose ambiguously between same-named cuts.',
      inputSchema: object({})
    },
    {
      name: 'get_session',
      description: 'Read current project and optimistic concurrency version.',
      inputSchema: { ...object({ id: string }), required: ['id'] }
    },
    {
      name: 'list_files',
      description: 'Browse an authorized local directory without copying media.',
      inputSchema: object({ path: string })
    },
    {
      name: 'add_media',
      description: 'Probe a local file and append it to the timeline; preview updates immediately.',
      inputSchema: {
        ...object({ id: string, path: string, trackId: string }),
        required: ['id', 'path']
      }
    },
    {
      name: 'list_motion_templates',
      description:
        'Read native flower template recipes, authorable components and their composition constraints, plus the HTML/GSAP/tick contract. Added backdrop/animation components require flower-style-03 or flower-style-38; other bases retain their original graph. Use before creating custom templates.',
      inputSchema: object({})
    },
    {
      name: 'add_html_clip',
      description:
        'Agent authoring/import entry point for editable animation templates on ordinary video tracks. Supply inline html payload OR authorized local path (local CSS/JS/images freeze into the clip). The editor exposes text/color/number/font values and primitive variables; expose script-controlled settings in html.variables. There is no user-facing HTML source or import form. GSAP is built in, tick receives seconds; start/length/duration use integer 120000 Hz ticks. Audio/video remain regular timeline clips. New tracks are topmost.',
      inputSchema: {
        ...object({
          id: string,
          html: htmlContent,
          path: string,
          name: string,
          start: { type: 'integer' },
          length: { type: 'integer' },
          trackId: string,
          width: { type: 'integer' },
          height: { type: 'integer' },
          duration: { type: 'integer' },
          transparent: boolean,
          variables: htmlContent.properties.variables
        }),
        required: ['id'],
        oneOf: [
          { required: ['html'], not: { required: ['path'] } },
          { required: ['path'], not: { required: ['html'] } }
        ]
      }
    },
    {
      name: 'add_text',
      description:
        'Insert editable text on a new text track. start and length are integer 120000 Hz ticks.',
      inputSchema: {
        ...object({
          id: string,
          content: propertySchema['text.content'],
          start: { type: 'integer' },
          length: { type: 'integer' },
          fontSize: propertySchema['text.fontSize'],
          color: propertySchema['text.color'],
          template: textTemplate
        }),
        required: ['id', 'content']
      }
    },
    {
      name: 'edit_timeline',
      description:
        'Atomically edit, undo or redo using the same shared history as the UI. Fields ending Seconds are seconds; set_property uses descriptor units (time properties are 120000 Hz ticks, speed is a multiplier). Linked AV trim/speed/split stay synchronized; expandLinked:false edits one item. Undo/redo must be the only operation. Track index 0 is the TOP visible layer. Supply the latest session version; conflicts require rereading. Returns ids of split right-hand clips.',
      inputSchema: {
        ...object({
          id: string,
          version: { type: 'integer' },
          operations: { type: 'array', minItems: 1, maxItems: 100, items: { oneOf: operations } }
        }),
        required: ['id', 'version', 'operations']
      }
    },
    {
      name: 'preview_control',
      description:
        'Send play, pause or seek to already-open browser previews. Open previewUrl first. Delivery is not playback confirmation: use get_preview_status and verify reported command sequence and advancing media time. Browsers may require one user play click for audio.',
      inputSchema: {
        ...object({ id: string, action: { enum: ['play', 'pause', 'seek'] }, timeSeconds: number }),
        required: ['id', 'action']
      }
    },
    {
      name: 'get_preview_status',
      description:
        'Read recent browser-reported playback state, rendered project version, decoded frame count and media playhead. Empty clients means no preview has reported in the last 15 seconds. This does not prove audible speaker output.',
      inputSchema: { ...object({ id: string }), required: ['id'] }
    },
    {
      name: 'update_session',
      description:
        'Replace authored project using the version from get_session. Times are integer 120000 Hz ticks. A conflict requires rereading before retry.',
      inputSchema: {
        ...object({ id: string, version: { type: 'integer' }, project: { type: 'object' } }),
        required: ['id', 'version', 'project']
      }
    },
    {
      name: 'save_project',
      description:
        'Save a complete portable .vcutweb DIRECTORY, including media, editable HTML/variables, flower recipes, effects, keys and frozen motion resources. Supply the latest session version and an authorized output directory. No native bridge is required. Reopen with open_project.',
      inputSchema: {
        ...object({ id: string, version: { type: 'integer' }, directory: string }),
        required: ['id', 'version']
      }
    },
    {
      name: 'export_project',
      description:
        'Write and verify a native .vcut package through the configured VideoCut bridge.',
      inputSchema: {
        ...object({ id: string, version: { type: 'integer' }, directory: string }),
        required: ['id', 'version']
      }
    },
    {
      name: 'render_video',
      description:
        'Render a frozen project version through its open browser using the same compositor as preview. Keep the preview page open. Choose mp4 or webm; unavailable browser encoders can use existing local FFmpeg.',
      inputSchema: {
        ...object({
          id: string,
          version: { type: 'integer' },
          directory: string,
          format: { enum: ['mp4', 'webm'] }
        }),
        required: ['id', 'version']
      }
    }
  ];
  const lines = createInterface({ input: process.stdin, crlfDelay: Infinity });

  async function handleLine(line) {
    let message;
    try {
      if (Buffer.byteLength(line) > 8 * 1024 * 1024) throw new Error('Message too large');
      message = JSON.parse(line);
    } catch {
      process.stdout.write(
        JSON.stringify({
          jsonrpc: '2.0',
          id: null,
          error: { code: -32700, message: 'Parse error' }
        }) + '\n'
      );
      return;
    }
    if (!message || typeof message !== 'object' || Array.isArray(message)) return;
    if (message.id === undefined) return;
    let result, error;
    if (message.method === 'initialize')
      result = {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: packageName, version: packageVersion }
      };
    else if (message.method === 'ping') result = {};
    else if (message.method === 'tools/list') result = { tools };
    else if (message.method === 'tools/call') {
      const a = message.params?.arguments || {};
      try {
        let value;
        switch (message.params?.name) {
          case 'list_motion_templates':
            value = {
              native: TEXT_TEMPLATES,
              parts: TEMPLATE_PARTS,
              nativeComposition: TEMPLATE_COMPOSITION_RULES,
              html: {
                type: 'html-clip',
                gsap: '3.15.0',
                clock: 120000,
                tick: 'window.tick(seconds, {width,height,duration,variables})',
                timelines: 'window.__timelines = {name: gsap.timeline({paused:true})}',
                variables: 'window.__videocutVariables (alias window.variables)',
                ready: 'window.__videocutReady = Promise',
                maxBytes: 1048576,
                maxSide: 4096,
                maxPixels: 8388608,
                media: 'Audio/video on timeline tracks',
                renderer: client.info.htmlClips,
                nativeSave: false
              }
            };
            break;
          case 'add_html_clip': {
            if (Boolean(a.html) === Boolean(a.path))
              throw new Error('请选择 html 内容或本地 path 中的一种');
            const imported = a.path
              ? await client.importHtml(a.path, {
                  width: a.width,
                  height: a.height,
                  duration: a.duration,
                  transparent: a.transparent,
                  variables: a.variables
                })
              : { html: a.html, name: a.name };
            const session = await client.getSession(a.id);
            value = await client.editSession(
              session.id,
              [
                {
                  action: 'add_html_clip',
                  html: imported.html,
                  name: a.name || imported.name,
                  start: a.start,
                  length: a.length,
                  trackId: a.trackId
                }
              ],
              session.version
            );
            break;
          }
          case 'open_project':
            value = await client.openProject(a.path);
            break;
          case 'get_properties': {
            const s = await client.getSession(a.id);
            const selected = a.itemId ? findItem(s.project, a.itemId).item : null;
            const paths = selected
              ? [
                  ...Object.keys(PROPERTY_DESCRIPTORS),
                  ...(selected.clip.effects || []).flatMap((effect) =>
                    Object.keys(
                      EFFECT_TEMPLATES.find((t) => t.id === effect.templateId)?.parameters || {}
                    ).map((parameter) => `effects.${effect.id}.${parameter}`)
                  )
                ].filter((path) => isPropertyApplicable(s.project, selected, path))
              : Object.keys(PROPERTY_DESCRIPTORS);
            value = {
              version: s.version,
              descriptors: selected
                ? Object.fromEntries(
                    paths.map((path) => [path, getPropertyDescriptor(selected, path)])
                  )
                : PROPERTY_DESCRIPTORS,
              effects: EFFECT_TEMPLATES,
              transitions: TRANSITION_TEMPLATES,
              values: selected
                ? Object.fromEntries(
                    paths.map((p) => [p, sampleProperty(selected, p, ticks(a.timeSeconds || 0))])
                  )
                : null
            };
            break;
          }
          case 'get_render_status':
            value = await client.renderStatus(a.id);
            break;
          case 'cancel_render': {
            const status = await client.renderStatus(a.id);
            value = await client.request(
              `/sessions/${a.id}/render-job?job=${status.id}&action=cancel`,
              { method: 'POST' }
            );
            break;
          }
          case 'initialize_demo':
            value = await client.initializeDemo(a);
            break;
          case 'create_session':
            value = await client.createSession(createProject(a.name));
            break;
          case 'list_sessions':
            value = await client.listSessions();
            break;
          case 'transcribe_speech': {
            const { id, ...options } = a;
            value = await client.transcribeSpeech(id, options);
            break;
          }
          case 'get_asr_status':
            value = await client.asrStatus(a.id);
            break;
          case 'cancel_asr':
            value = await client.cancelAsr(a.id);
            break;
          case 'get_vision_model_status':
            value = await client.visionModelStatus();
            break;
          case 'request_vision_setup':
            value = await client.requestVisionSetup();
            break;
          case 'describe_image':
          case 'describe_video': {
            const { path, prompt, waitSeconds = 30, ...options } = a;
            if (!Number.isFinite(waitSeconds) || waitSeconds < 0 || waitSeconds > 60)
              throw new Error('waitSeconds must be between 0 and 60');
            const describe =
              message.params.name === 'describe_image'
                ? client.describeImage
                : client.describeVideo;
            value = await describe.call(client, path, prompt, {
              ...options,
              timeoutMs: waitSeconds * 1000
            });
            break;
          }
          case 'analyze_media': {
            const { id, ...options } = a;
            value = await client.analyzeMedia(id, options);
            break;
          }
          case 'get_vision_status':
            value = await client.visionStatus(a.id, a.jobId);
            break;
          case 'cancel_vision':
            value = await client.cancelVision(a.id, a.jobId);
            break;
          case 'cancel_vision_model_install':
            value = await client.request('/vision/model/install', { method: 'DELETE' });
            break;
          case 'get_asr_model_status':
            value = await client.asrModelStatus();
            break;
          case 'list_voices':
            value = await client.listTtsVoices();
            break;
          case 'install_tts_model':
            value = await client.installTtsModel({ dtype: a.dtype, voices: a.voices });
            break;
          case 'get_tts_model_status':
            value = await client.ttsModelStatus();
            break;
          case 'cancel_tts_model_install':
            value = await client.cancelTtsModelInstall();
            break;
          case 'synthesize_speech': {
            const { id, ...options } = a;
            value = await client.synthesizeSpeech(id, options);
            break;
          }
          case 'get_tts_status':
            value = await client.ttsStatus(a.id);
            break;
          case 'cancel_tts':
            value = await client.cancelTts(a.id);
            break;
          case 'get_session':
            value = await client.getSession(a.id);
            break;
          case 'list_files':
            value = await client.listFiles(a.path);
            break;
          case 'add_media': {
            const asset = await client.importMedia(a.path);
            const s = await client.getSession(a.id);
            value = await client.editSession(
              s.id,
              [{ action: 'add_asset', asset, trackId: a.trackId }],
              s.version
            );
            break;
          }
          case 'add_text': {
            const s = await client.getSession(a.id);
            value = await client.editSession(
              s.id,
              [
                {
                  action: 'add_text',
                  ...a,
                  startSeconds: a.start === undefined ? 0 : seconds(a.start),
                  durationSeconds: a.length === undefined ? 5 : seconds(a.length)
                }
              ],
              s.version
            );
            break;
          }
          case 'edit_timeline': {
            value = await client.editSession(a.id, a.operations, a.version);
            break;
          }
          case 'preview_control':
            value = await client.controlPreview(a.id, a.action, a.timeSeconds);
            break;
          case 'get_preview_status':
            value = await client.previewStatus(a.id);
            break;
          case 'update_session':
            value = await client.updateSession(a.id, a.project, a.version);
            break;
          case 'save_project':
            value = await client.saveProject(a.id, a.version, a.directory);
            break;
          case 'export_project':
            value = await client.exportProject(a.id, a.version, a.directory);
            break;
          case 'render_video':
            value = await client.renderVideo(a.id, a.version, a.directory, a.format);
            break;
          default:
            throw new Error('Unknown tool');
        }
        result = { content: [{ type: 'text', text: JSON.stringify(value) }] };
      } catch (e) {
        const details = {
          ...(e.details || {}),
          error: e.message,
          ...(e.status ? { status: e.status } : {})
        };
        result = {
          isError: true,
          structuredContent: details,
          content: [{ type: 'text', text: JSON.stringify(details) }]
        };
      }
    } else error = { code: -32601, message: 'Method not found' };
    process.stdout.write(
      JSON.stringify({ jsonrpc: '2.0', id: message.id, ...(error ? { error } : { result }) }) + '\n'
    );
  }
  // A render request lives until completion; independent status/cancel requests must
  // remain serviceable on the same stdio connection. Each reply is one JSON write.
  const pending = new Set();
  for await (const line of lines) {
    if (pending.size >= 64) {
      let id = null;
      try {
        id = JSON.parse(line).id ?? null;
      } catch {}
      process.stdout.write(
        JSON.stringify({
          jsonrpc: '2.0',
          id,
          error: { code: -32000, message: 'Too many concurrent requests' }
        }) + '\n'
      );
      continue;
    }
    const task = handleLine(line).catch(() => {});
    pending.add(task);
    task.finally(() => pending.delete(task));
  }
  await close();
}
