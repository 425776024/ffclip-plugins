// Pinned Whisper Base revision and SHA256 metadata from the upstream repository.
export const WHISPER_BASE = Object.freeze({
  id: 'whisper-base',
  label: 'Base',
  repository: 'onnx-community/whisper-base_timestamped',
  revision: '608c49e61301901684bc36cac8f74b95ff6b5a8e',
  files: {
    'config.json': {
      size: 2243,
      sha256: 'f4d0608f7d918166da7edb3e188de5ef1bfe70d9802e785d271fd88111e9cf4b'
    },
    'generation_config.json': {
      size: 3832,
      sha256: '61070cf8de25b1e9256e8e102ded49d8d24a8369ed36ef84fdf21549e68125a0'
    },
    'preprocessor_config.json': {
      size: 339,
      sha256: 'a6a76d28c93edb273669eb9e0b0636a2bddbb1272c3261e47b7ca6dfdbac1b8d'
    },
    'tokenizer.json': {
      size: 2480466,
      sha256: '27fc476bfe7f17299480be2273fc0608e4d5a99aba2ab5dec5374b4482d1a566'
    },
    'tokenizer_config.json': {
      size: 282682,
      sha256: '2e036e4dbacfdeb7242c7d4ec4149f4a16e86026048f94d1637e3a8ee9c6a573'
    },
    'onnx/encoder_model.onnx': {
      size: 82451730,
      sha256: '7fcea817bb2be4d86729b521e5a7fcbec28fa743edfed67e882b33ff15852540'
    },
    'onnx/decoder_model_merged_q4.onnx': {
      size: 123738327,
      sha256: 'fc1902ce2e42c69b2346d8e2a98898c60c01da1e6a64ae90f41d22350ac7db13'
    }
  }
});
