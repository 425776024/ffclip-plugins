// Pinned upstream metadata. Only explicit installation downloads these resources.
import { PinnedModelStore } from './model-store.mjs';
export { modelError as ttsError } from './model-store.mjs';
import { modelError as ttsError } from './model-store.mjs';
import { homedir } from 'node:os';
import { join } from 'node:path';
export const KOKORO_MODEL_ID = 'kokoro-v1.1-zh';
export const KOKORO_REPOSITORY = 'onnx-community/Kokoro-82M-v1.1-zh-ONNX';
export const KOKORO_REVISION = '6cc0f0d2ebe369a68b0df87c2b65c1af8c0ac3e3';
export const KOKORO_FILES = Object.freeze({
  'config.json': {
    size: 44,
    sha256: 'df34b4f930b23447cd4dc410fabfb42eb3f24e803e6c3f97d618fb359380a36f'
  },
  'onnx/model.onnx': {
    size: 339369442,
    sha256: '94b973941b1852754f979be5d5e20be666d5c81d9bb886b88ae1dc85c9b895ca'
  },
  'tokenizer.json': {
    size: 4944,
    sha256: '5715a60b09d5e4b9074435d68c6ccd5675b9d48b220e109fdea3cda681e23d15'
  },
  'tokenizer_config.json': {
    size: 113,
    sha256: 'be1cb066d6ef6b074b3f15e6a6dd21ac88ff3cdaedf325f0aaed686c70f75d20'
  },
  'voices/af_maple.bin': {
    size: 522240,
    sha256: 'bd5b230b916ea98c67a3a7a833a3ce43e535ce56a81503f4166ee9390b9ddeeb'
  },
  'voices/af_sol.bin': {
    size: 522240,
    sha256: '87c1d6d6a3f13f89ed54a6a67cc1ff75b926aebc3a2aac3086e6dd109a8147e6'
  },
  'voices/bf_vale.bin': {
    size: 522240,
    sha256: '2536f7922d31d96e994135ac2bb73f5a3c01326476200513c57d988822c6ca4d'
  },
  'voices/zf_001.bin': {
    size: 522240,
    sha256: '0a89ec12bb93fb9c74077924daf02568baad64e1f869389f5aaee01a386035f8'
  },
  'voices/zf_002.bin': {
    size: 522240,
    sha256: '452f96e1e3c20b14b228b5336a8d7e833b105f837d98ef53b4ddfce18eed39bf'
  },
  'voices/zf_003.bin': {
    size: 522240,
    sha256: 'a70654663d013a700f8afe42d57d1ce03ff49af8bdedbbb56c7e3da6e820b788'
  },
  'voices/zf_004.bin': {
    size: 522240,
    sha256: '81fe28be1c496ccffa73078d5c2e6d9cc5e5f91e354d6ae0f494a9c28a42064d'
  },
  'voices/zf_005.bin': {
    size: 522240,
    sha256: 'a91d2c2adecbb7c191dac4fa35e213e8d370e0c733a9bf36797f513924dcb84c'
  },
  'voices/zf_006.bin': {
    size: 522240,
    sha256: '52dee405a0e609d6d2869eef05984745db435a73aea49db7418494d5cb53bf9d'
  },
  'voices/zf_007.bin': {
    size: 522240,
    sha256: '23b6c4312208de7f195e4e78eef0ec9e498cd52907b1678ecb7a0a1996a51573'
  },
  'voices/zf_008.bin': {
    size: 522240,
    sha256: '23e20843a71fb8c5f6b984c8d4e74dd590251aeb8f620cb07a57e1485d726b0c'
  },
  'voices/zf_017.bin': {
    size: 522240,
    sha256: '9c6513e77a8efb7172a4e4ebeb48b3b010152d8cd6a0f2fa1adb353248fd91ed'
  },
  'voices/zf_018.bin': {
    size: 522240,
    sha256: 'a9ee6566c7500ce90cbd177f1d6f6cae533356e5f0e37edf3adbd0a8f2d44020'
  },
  'voices/zf_019.bin': {
    size: 522240,
    sha256: 'ab5906846335ac1227c47fd04be8b7b10a47c3ee77729296cc4c0f3f8fe79073'
  },
  'voices/zf_021.bin': {
    size: 522240,
    sha256: 'c16f939c566f786150be3e3cb6a61f7fd8d4214a26a46d3e80e1d61de06d54ec'
  },
  'voices/zf_022.bin': {
    size: 522240,
    sha256: 'c1cde40b2fd522355d2ae152d06d1033b04ad3528b7273cf25dd68f28a5563db'
  },
  'voices/zf_023.bin': {
    size: 522240,
    sha256: 'd02846dc3b8a89b4f81634b407ce8dd30d80db40fc952d54f05853a8e26a2190'
  },
  'voices/zf_024.bin': {
    size: 522240,
    sha256: '2d0f2df7b25482f6870cfbe8e83c7d1e74ed8ef8ae72df749207ea9cec7c9102'
  },
  'voices/zf_026.bin': {
    size: 522240,
    sha256: '2a2434fed129796276c61947fc65bf7dadd569468ea4edaca81f3d59802bdc32'
  },
  'voices/zf_027.bin': {
    size: 522240,
    sha256: '9301e7a0b80eedff7f314076e07f9eb74b687afb7fcb2c64ca94168469858aae'
  },
  'voices/zf_028.bin': {
    size: 522240,
    sha256: '8d15e55ad315708010c1528c633828e3179d5cbf0bfcd2db0fec27f63001eaec'
  },
  'voices/zf_032.bin': {
    size: 522240,
    sha256: '2ab0b9b1fe2f1e4cfa08b0126581df3316c12aadeadc51136c826255debd2058'
  },
  'voices/zf_036.bin': {
    size: 522240,
    sha256: 'b72873066ef9d57d4ea0605d32a82b2655010af51edbaecd6f926250d84efa4e'
  },
  'voices/zf_038.bin': {
    size: 522240,
    sha256: '569bb54ebf05c00c89eaf56218992474a8a0494d8f94a14e933ae6a5ea9f7e31'
  },
  'voices/zf_039.bin': {
    size: 522240,
    sha256: '92ad68f36d16860dcf125fdf87a4572c7ab6d7efbe6a7197d6772242a2cbbb56'
  },
  'voices/zf_040.bin': {
    size: 522240,
    sha256: 'bae25bd2c4dd717ce65effa2350b28a55a73f53a0bcc2cca5876a15e984eaa2d'
  },
  'voices/zf_042.bin': {
    size: 522240,
    sha256: '713160091b44d962ab73e3bf66f0c2f0b9677aaf0c2f0e62faa94ed451540d1e'
  },
  'voices/zf_043.bin': {
    size: 522240,
    sha256: '7d257954b651436e354712738b22e8365ed908923a5d0f154690188b2c73ea78'
  },
  'voices/zf_044.bin': {
    size: 522240,
    sha256: 'e166c8b2a680d3f51f1765cedc4ed7a29308ea40081b03347d5cd95f52119cc0'
  },
  'voices/zf_046.bin': {
    size: 522240,
    sha256: '73e8881e76670cfe803c8ab0ccc6b5daae22efdc3a67f8e0e80658a4be21d3af'
  },
  'voices/zf_047.bin': {
    size: 522240,
    sha256: '620f92a680c5a28bd8eb601738721047b73008bdf5b880f34075181afbfdcb62'
  },
  'voices/zf_048.bin': {
    size: 522240,
    sha256: '24e0aa66d1cb264e86adac5ea143f7fb0a9672a8b237ba1ad017aacc1fb0290a'
  },
  'voices/zf_049.bin': {
    size: 522240,
    sha256: '4808ba6dfc1c13af9921bb5072faf875b69bc54777281eb549592b1aa7e8a4bc'
  },
  'voices/zf_051.bin': {
    size: 522240,
    sha256: '64df55d707d3c28dec15c5c9051653b6a6cbbbf85c52e56183d3922e53415e76'
  },
  'voices/zf_059.bin': {
    size: 522240,
    sha256: 'f12fd6e34f602445173f930c4f41b4d064e1a981b39b664591ad2476fb7425cc'
  },
  'voices/zf_060.bin': {
    size: 522240,
    sha256: '429ad8ffb6fc8b68d3b77f2ccafba5be50e4ec31d25f890d739a16449ca01748'
  },
  'voices/zf_067.bin': {
    size: 522240,
    sha256: '5669caef309a3746a47588d893d58fa61c721a94ca07e431c732ea9a2b4abc6a'
  },
  'voices/zf_070.bin': {
    size: 522240,
    sha256: 'f225305611865b09dc8cb36439143c02a404299b00508e00676655bb117342e2'
  },
  'voices/zf_071.bin': {
    size: 522240,
    sha256: '454cd5aaa9458f3b028e973d19902a002eb59fc743eb94fb3c7c9d5d7a190a07'
  },
  'voices/zf_072.bin': {
    size: 522240,
    sha256: '2274b572b44723e80eee422a914c96b37ee42bf9b5ca60e7f656bede7860a1d3'
  },
  'voices/zf_073.bin': {
    size: 522240,
    sha256: 'b8ecf6945ae7bb2a0353422b7af32d3b928c5fdccf3ea553bc61227a57bf4a2c'
  },
  'voices/zf_074.bin': {
    size: 522240,
    sha256: '8e6726977c7494dfd567c580963630834c0971f66111e06c9467e6f7d912ea1e'
  },
  'voices/zf_075.bin': {
    size: 522240,
    sha256: '84e70ca9aac5178ceaeb1eae95dd77278d71e60cfacaf9af99852922b4355560'
  },
  'voices/zf_076.bin': {
    size: 522240,
    sha256: '58351a1be5aa04f874bea0e5228fc0e0047dc2ee801e915d4f59513ca60a3194'
  },
  'voices/zf_077.bin': {
    size: 522240,
    sha256: 'e2fd07f3b62e204cd6632696e2a975678e7e6aa5a9b2cb4927f94d875c113625'
  },
  'voices/zf_078.bin': {
    size: 522240,
    sha256: '82d591700a9ecb3cb14d3d05ec5aab18bd7a482d677bf6522cf96ed4c064b23b'
  },
  'voices/zf_079.bin': {
    size: 522240,
    sha256: '2df9a9fbbd39a54077a2c5073c04c7e685ce0342b34f22fc85c25a1f7a3954c7'
  },
  'voices/zf_083.bin': {
    size: 522240,
    sha256: '5557cbc381e0f2cd2a29759fdda25eca664492f1cfa3fcc241e7adeaf4164661'
  },
  'voices/zf_084.bin': {
    size: 522240,
    sha256: '2b2d4fdefa9a6c3d9f472d91600308be8b827a929955ec66afe81aca50c99968'
  },
  'voices/zf_085.bin': {
    size: 522240,
    sha256: '2451d63f576c9ffbe91acd11f2720b6e76dc75b293d7350a9620a28c23da9ef8'
  },
  'voices/zf_086.bin': {
    size: 522240,
    sha256: 'c228ba018bf56d5de1339bde10dcbea7700797b85351e4d57ee5307ac7d3ee99'
  },
  'voices/zf_087.bin': {
    size: 522240,
    sha256: 'e0347f1430b80781d910bf1499cf64d231c34b70d6e46f30f3598254d5114d44'
  },
  'voices/zf_088.bin': {
    size: 522240,
    sha256: 'ee1e80cdeaffb55e1f6c41005c4966b8979b16bb808fa451739e06953d343c13'
  },
  'voices/zf_090.bin': {
    size: 522240,
    sha256: 'fe90b3e651c3741edf6bc08a543c531c798ec53fab1fd944ab537e9257270c68'
  },
  'voices/zf_092.bin': {
    size: 522240,
    sha256: 'ffd200defd2867f79886eb0e45540d53ed0dc00ceb222c5fa692f004958d3e11'
  },
  'voices/zf_093.bin': {
    size: 522240,
    sha256: 'd736ac1738c5ba567f8e5e9bfac8d9a774e9e3a3d001b5976a5d005c9179f811'
  },
  'voices/zf_094.bin': {
    size: 522240,
    sha256: 'b37b96e3c51d12f152c9476cae42396ac38cdab76f09cc935f2fbcd8fa012f42'
  },
  'voices/zf_099.bin': {
    size: 522240,
    sha256: '9582b2ffb5027c695f6775161a8856108fa68530c6c1e29d2354d455d9eaa937'
  },
  'voices/zm_009.bin': {
    size: 522240,
    sha256: '7b74d6ed22f201e2fa28758e78ce6197082779f2b80e69ea1bf877908609514a'
  },
  'voices/zm_010.bin': {
    size: 522240,
    sha256: '73b088f7e0dc47adca4d6a642ee68843df90ff56ec2800c29d96609989d6de0a'
  },
  'voices/zm_011.bin': {
    size: 522240,
    sha256: 'ec4d7d934b9aa47e0e98e6ec802bb7b0a221be6f1b67d923d33b7082d9bbfe9f'
  },
  'voices/zm_012.bin': {
    size: 522240,
    sha256: '50d1986f71ea1a2b3ab1bfc0c95bedc59e8de45e650bc5bde87fd99ceb83cdfb'
  },
  'voices/zm_013.bin': {
    size: 522240,
    sha256: '2caa23e98910fb232a2bf6aab563666588a82bc37c99b67963ab304c5be66dff'
  },
  'voices/zm_014.bin': {
    size: 522240,
    sha256: '565aa48a99d8a196ebdd1c72f4a0e5760fed65aa95992c887385d3aad9e1d2f8'
  },
  'voices/zm_015.bin': {
    size: 522240,
    sha256: 'e695c8d72b3eb4a864f4735db7f4f93de100ff62704e619edfddbebfafb07312'
  },
  'voices/zm_016.bin': {
    size: 522240,
    sha256: '4dc408a11f1e8925ff8bca40c885e7596d08be63c2c61571234b96170f8e6d1b'
  },
  'voices/zm_020.bin': {
    size: 522240,
    sha256: '2a25ca83ddfd003c0b82d97899ff38734db8faf48e5e4076d68c69aea17705c8'
  },
  'voices/zm_025.bin': {
    size: 522240,
    sha256: 'cbb4cd1df85b5dde4cf742c61d9b93e179574935c1162d06604bc4be6a8e990d'
  },
  'voices/zm_029.bin': {
    size: 522240,
    sha256: 'ff78cb9f64d43fe6179c9479257775bea94dc31eade9362b0c630749025d8ecd'
  },
  'voices/zm_030.bin': {
    size: 522240,
    sha256: '32f8fe4d5f626dfb67adb73499690fd4f5952763746efbcadcf2daad4a3560a6'
  },
  'voices/zm_031.bin': {
    size: 522240,
    sha256: 'a5974232ed634be2ae40526101ee7c653120fdb354b1584f2bb38ed4fae4a39e'
  },
  'voices/zm_033.bin': {
    size: 522240,
    sha256: '758e64c15efad492beebf575eb96951e0500da980bf5e8a3850d078d0430dda1'
  },
  'voices/zm_034.bin': {
    size: 522240,
    sha256: 'b371bef26b75c826de15c836103da3e037a44f605ffe744d68ad485d17e370ab'
  },
  'voices/zm_035.bin': {
    size: 522240,
    sha256: '8b1c603f6e1eae300ac1a6f1199a5bb1ce9814d3e1e61f7536d21b564d680476'
  },
  'voices/zm_037.bin': {
    size: 522240,
    sha256: '535ec47cb6ed66c203f61ace51eddbc7a13ecaca2458c1d8d5a928fb3db4b315'
  },
  'voices/zm_041.bin': {
    size: 522240,
    sha256: '8b7bd5649a00c62e87a6099eb31d683664bfb0fd55bcd5211da62e95b3b78ffc'
  },
  'voices/zm_045.bin': {
    size: 522240,
    sha256: '68d5e7f3415811a076326178c2d9f5d3aa168cf27deb6ae3f0395ab97ad45225'
  },
  'voices/zm_050.bin': {
    size: 522240,
    sha256: '7869f25a5e71ea9b67a1893777e375ac411bdbfb75feff5efe25fad2fc766c8d'
  },
  'voices/zm_052.bin': {
    size: 522240,
    sha256: 'e775a71cd74c5462d5a6c3f0ff7681f04419053043ad2dcbabaa13d0c6de0b08'
  },
  'voices/zm_053.bin': {
    size: 522240,
    sha256: 'a549dcf0456b2ba1515455e0a30d111b00d24e748880e03da1965cd4ff10baf7'
  },
  'voices/zm_054.bin': {
    size: 522240,
    sha256: '4f7aeb1627fa1d406bcc1a83e4eeb8b014509dca492dec0ae7dd4d0d1cdbe8b3'
  },
  'voices/zm_055.bin': {
    size: 522240,
    sha256: '3cf77a37899053b5a7c224f51a22371dc37d30667d87e1b481e760ac0f8f0da8'
  },
  'voices/zm_056.bin': {
    size: 522240,
    sha256: 'd5f48143152f376a940182741589fb1f588387e09f1a9263760981f8702af816'
  },
  'voices/zm_057.bin': {
    size: 522240,
    sha256: '29cb23f82add956f25c7b663db2027d5a50abb7035c7cb15ee608e193a412f51'
  },
  'voices/zm_058.bin': {
    size: 522240,
    sha256: '0cafc2ef83710c4ce55b384107426cfdcb23aabf5699e106ff392f9571b4d4f3'
  },
  'voices/zm_061.bin': {
    size: 522240,
    sha256: 'd57141bd480e7a4f1a3b5d3365ce53ac04cfe6dd9d0f2d3a052213f80657046d'
  },
  'voices/zm_062.bin': {
    size: 522240,
    sha256: '171d8811bfeb198714b0d3733ed740eb56be0aa3e6d2bd7b127904116a199f93'
  },
  'voices/zm_063.bin': {
    size: 522240,
    sha256: '6fbe326e5852cd9dbac7eb7e9c9989a604e51074d251a8ff8365e050c83b2a6e'
  },
  'voices/zm_064.bin': {
    size: 522240,
    sha256: '6e712d18758f2d8654c97fb274c23b5148dbafc1027ee6539446d4c8aff308cd'
  },
  'voices/zm_065.bin': {
    size: 522240,
    sha256: '77918a32b75bf7902a6cee310cd22fb16de7ecbb9aa438a05039ebec73f9e0e8'
  },
  'voices/zm_066.bin': {
    size: 522240,
    sha256: 'fcdd1d4c8b2808c4456418c76ed2321910b3f1b5c2646365af4ee0e8c6c53c4d'
  },
  'voices/zm_068.bin': {
    size: 522240,
    sha256: 'ae265665e29656ea0d151f3315c649d892ecf053391c3c3bec5f10bf18531a58'
  },
  'voices/zm_069.bin': {
    size: 522240,
    sha256: '54ac2dc25ee5d1dfa72f777ff538faf69033705019a9e15f3138deb79a965774'
  },
  'voices/zm_080.bin': {
    size: 522240,
    sha256: 'cbbcae6bfd6c2f3875b2163fffc8851aeae1b8bad2f1006a94ea4e6f9b44ae9e'
  },
  'voices/zm_081.bin': {
    size: 522240,
    sha256: 'ffc1341f044c95ad8ef598079df9e3a85d81d093268ae8422848a9b81d386fd2'
  },
  'voices/zm_082.bin': {
    size: 522240,
    sha256: '7351d7652d97ec5fcb7632b317d7c4e062fff3cf4ed0246167d3d15e97046c01'
  },
  'voices/zm_089.bin': {
    size: 522240,
    sha256: 'f4315ceab376b4082cc9cdd8028d37f9aa7d43ad67ddb85902a247df0913ca89'
  },
  'voices/zm_091.bin': {
    size: 522240,
    sha256: '6ab4a21b702ec57161f48e350de3fa0b0bd1d13a890657398cfa21452d85a9b3'
  },
  'voices/zm_095.bin': {
    size: 522240,
    sha256: '4d4dac7245ea4c4c4740137fca229932055e376aed2b1ad7cd0e3e3859a13b75'
  },
  'voices/zm_096.bin': {
    size: 522240,
    sha256: 'e6b76d86ca459d8ce87d2a41c812319b11e9563ecf1ba80a4e86a368d6c02fbc'
  },
  'voices/zm_097.bin': {
    size: 522240,
    sha256: 'e60ec05738261c2eb13c43f7429ece6b32e0992223b0a6336a9fcdec90fb8da4'
  },
  'voices/zm_098.bin': {
    size: 522240,
    sha256: '7dd142334e863af31c7e0d0fbe491b8d54083ff5322441d550fcfc5e4111480d'
  },
  'voices/zm_100.bin': {
    size: 522240,
    sha256: '112de52f1aab3b370fd01f4e2e1d8bb37fee2725e77e14b81c802d50687e0e6e'
  }
});

export const KOKORO_DTYPES = Object.freeze({
  fp32: 'onnx/model.onnx'
});
const commonFiles = ['config.json', 'tokenizer.json', 'tokenizer_config.json'];
export function defaultTtsModelDir() {
  const cache =
    process.platform === 'darwin'
      ? join(homedir(), 'Library', 'Caches')
      : process.platform === 'win32'
        ? process.env.LOCALAPPDATA || join(homedir(), 'AppData', 'Local')
        : process.env.XDG_CACHE_HOME || join(homedir(), '.cache');
  return join(cache, 'videocut', 'tts');
}
const voiceInfo = (id) => ({
  id,
  name: `${id.startsWith('z') ? '中文' : '英语'}${id[1] === 'f' ? '女声' : '男声'} ${id}`,
  language: id.startsWith('z') ? 'zh' : 'en',
  gender: id[1] === 'f' ? 'female' : 'male'
});

/** Persistent, content-verified model resources. No caller-controlled remote URLs. */
export class KokoroModelStore extends PinnedModelStore {
  constructor({
    directory = defaultTtsModelDir(),
    fetchImpl = globalThis.fetch,
    files = KOKORO_FILES
  } = {}) {
    super({
      directory,
      fetchImpl,
      files,
      modelId: KOKORO_MODEL_ID,
      repository: KOKORO_REPOSITORY,
      revision: KOKORO_REVISION
    });
    this.voices = Object.keys(files)
      .filter((path) => /^voices\/[a-z]{2}_[a-z0-9]+\.bin$/.test(path))
      .map((path) => voiceInfo(path.slice(7, -4)));
  }
  async catalog() {
    const installed = new Set();
    for (const path of Object.keys(this.files))
      if (await this.verifiedFile(path)) installed.add(path);
    return {
      model: KOKORO_MODEL_ID,
      modelBaseUrl: `/tts-models/${KOKORO_MODEL_ID}/`,
      revision: KOKORO_REVISION,
      defaultVoice: 'zf_001',
      voices: this.voices.map((voice) => ({
        ...voice,
        installed: installed.has(`voices/${voice.id}.bin`)
      })),
      installedDtypes: Object.entries(KOKORO_DTYPES)
        .filter(([, path]) => installed.has(path) && commonFiles.every((p) => installed.has(p)))
        .map(([dtype]) => dtype),
      installedVoices: this.voices
        .filter((voice) => installed.has(`voices/${voice.id}.bin`))
        .map((voice) => voice.id),
      install: this.status()
    };
  }
  async require(dtype, voice) {
    if (!Object.hasOwn(KOKORO_DTYPES, dtype)) throw ttsError('模型精度无效');
    if (!this.voices.some((v) => v.id === voice)) throw ttsError('音色不存在');
    const paths = [...commonFiles, KOKORO_DTYPES[dtype], `voices/${voice}.bin`];
    for (const path of paths)
      if (!(await this.verifiedFile(path)))
        throw ttsError('请先安装所选模型精度和音色', 409, 'TTS_MODEL_REQUIRED');
  }
  startInstall({ dtype = 'fp32', voices = ['zf_001'] } = {}) {
    if (
      !Object.hasOwn(KOKORO_DTYPES, dtype) ||
      !Array.isArray(voices) ||
      !voices.length ||
      voices.length > 103 ||
      voices.some((id) => !this.voices.some((v) => v.id === id))
    )
      throw ttsError('模型精度或音色无效');
    if (this.controller) throw ttsError('已有模型安装任务，请等待或取消', 409);
    const paths = [
      ...commonFiles,
      KOKORO_DTYPES[dtype],
      ...new Set(voices.map((id) => `voices/${id}.bin`))
    ];
    const files = paths.map((path) => ({ path, size: this.files[path].size, downloaded: 0 }));
    this.install = {
      state: 'downloading',
      dtype,
      voices: [...new Set(voices)],
      progress: 0,
      files
    };
    const controller = (this.controller = new AbortController());
    this.pending = this.downloadAll(controller, files)
      .catch((error) => {
        this.install.state = controller.signal.aborted ? 'cancelled' : 'error';
        if (!controller.signal.aborted)
          this.install.error = String(error.message || error).slice(0, 2000);
      })
      .finally(() => {
        if (this.controller === controller) this.controller = null;
      });
    return this.status();
  }
}
