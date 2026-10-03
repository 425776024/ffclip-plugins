// Hugging Face repository metadata, pinned 2026-10-01. Weights are never bundled.
export const FASTVLM = {
  id: 'fastvlm-0.5b',
  repository: 'onnx-community/FastVLM-0.5B-ONNX',
  revision: 'ca35eb9373f8a8761df0855fca19dea330f2407a',
  license: 'apple-amlr',
  dtype: { embed_tokens: 'fp16', vision_encoder: 'q4', decoder_model_merged: 'q4' },
  files: {
    'config.json': {
      size: 1328,
      sha256: 'abfaa79d962f99140c831fe28981f8658f3f450c2427b91e10c11fe6c518467d'
    },
    'generation_config.json': {
      size: 121,
      sha256: '6358e22636e45c878c2a494d46407fb5a6f3900f58d5d0c511f7e78ace50efcb'
    },
    'preprocessor_config.json': {
      size: 466,
      sha256: '431dcc491245f86385007309d1298fb1e9e0a717ef14153cd058a3355ad265df'
    },
    'processor_config.json': {
      size: 133,
      sha256: '55133a95dcfd23cb0f76b3c4deb9d4691791000af27a4d03d17e041d760dab19'
    },
    'tokenizer_config.json': {
      size: 1529,
      sha256: '8c10f3c929b1c1dae648b048c80796ab6cba569230660be6938fb22f77c8a351'
    },
    'tokenizer.json': {
      size: 11413284,
      sha256: '8bb55926dbf36523cd143a5805102a9a516df89f7010313d182e1e710d94fb15'
    },
    'special_tokens_map.json': {
      size: 367,
      sha256: 'f4f79e08d97f4d1c87f8d89264f525c8789da3b73b3bb55d1e12f692f41a7b1b'
    },
    LICENSE: {
      size: 5814,
      sha256: 'c1211a85bc9fab9e4bcf4b3711d2e100967bc3244b6b48404ce609553660ceae'
    },
    'onnx/embed_tokens_fp16.onnx': {
      size: 271810890,
      sha256: '9f0654c7da55099ef73662fb00d04dc65169947ac08c75197a6b9a59632ee521'
    },
    'onnx/vision_encoder_q4.onnx': {
      size: 505205898,
      sha256: '596db77eade9ef2373f13a7142cef52c64ee18effcf7555a82e89d657be9ffff'
    },
    'onnx/decoder_model_merged_q4.onnx': {
      size: 317445767,
      sha256: 'd4efcd943ec6bc3e0ddbcd2022330a563243ec9a648fb05a616b201d5a8c43a5'
    }
  }
};
