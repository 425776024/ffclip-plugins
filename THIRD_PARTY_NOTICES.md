# Third-party notices

The MIT license applies to the original VideoCut and ffvideo code and authored templates in this repository. It does not replace licenses for third-party code, fonts, models or media.

- Text renderer: [notices](packages/text-wasm/NOTICE.md), [license texts](packages/text-wasm/LICENSES/), [source snapshot](packages/text-wasm/vendor/SOURCE.json). The frozen WASM runtime includes the same dependencies and notices.
- Browser speech: [notices](packages/tts/NOTICE.txt), [distribution inventory](packages/tts/DISTRIBUTION.txt), licenses under `packages/tts/vendor/licenses/`.
- Speech recognition and visual understanding: [notices](packages/asr/NOTICE.txt). Model weights are downloaded separately and retain their own terms.
- Mediabunny: MPL-2.0; GSAP: Standard GSAP license. Build scripts retain notices in generated distributions. These dependencies are not relicensed under MIT.
- Development fonts: license texts under `packages/text-wasm/LICENSES/`.
- ffvideo retrieved media: retain per-asset author, source and license records, including export credits.

The official website source in the private `ffclip/` directory is excluded from this repository and from this MIT grant.

## WebAV acknowledgment / WebAV 开源声明

VideoCut's early browser video implementation incorporated and adapted the [WebAV](https://github.com/WebAV-Tech/WebAV) open-source project, including its browser audio/video clipping, composition and related utilities. We thank WebAV author 风痕 and its contributors. The historical vendored source is retained in this repository's Git history at `src/renderer/src/WebAV/`.

VideoCut 早期浏览器音视频实现引用并改编了 WebAV 的片段处理、合成与相关工具代码，感谢作者风痕及社区贡献者。历史引用源码保留在本仓库 Git 历史的 `src/renderer/src/WebAV/` 目录中。

WebAV is licensed under the MIT License, Copyright (c) 2023 风痕. Its original copyright and permission notice are preserved in [licenses/WebAV-MIT.txt](licenses/WebAV-MIT.txt) and included with distributions. Current browser media decoding and encoding use Mediabunny, whose MPL-2.0 notices are retained separately.

## PAGX

Tencent libpag PAGX (Apache-2.0) supplies the WASM graphic renderer and optional native HTML importer. The official PAGX skill is bundled unchanged. Runtime version, pinned converter source and adapter changes are recorded in `packages/pagx/NOTICE.txt`; distributed notices are in `dist/web/pagx-runtime/`. The renderer produces graphic frames; VideoCut handles video encoding independently. No PAG Enterprise SDK is included. `@xmldom/xmldom` 0.9.12 (MIT) parses editable XML; its license is included in `dist/licenses/xmldom-MIT.txt`.
