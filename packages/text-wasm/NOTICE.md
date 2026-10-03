# Source and third-party notices

VideoCut source and authored template fixtures in this repository are released under the MIT license (see LICENSE). Vendored third-party code, fonts and other assets retain their original licenses and notices. The snapshot and exact copied-file hashes are recorded in vendor/SOURCE.json.

Two local WASM adaptations avoid shaping unused whole-paragraph passes and allocating transparent SourceOver glyph filters. Their original snapshot hashes, current source hashes and purposes are recorded in vendor/SOURCE.json under localAdaptations; these files are adapted SDK source rather than untouched copies.

The experimental WGSL shaders in src/gpu-shaders.mjs are derived from the copied VideoCut MetalTextSdf.metal, MetalTextSdfMaterial.metal, MetalGaussian.metal, and MetalQtTextSoftGlow.metal. Their original files remain unchanged under vendor/. Port scope and deliberate numerical guards are documented in reports/gpu-port.md. The native Metal validation program is a development tool and is not part of the npm runtime.

The radial kernel in src/composition-shaders.mjs derives from MetalRadialBlur.metal and the schedule in QtTextRadialBlurMetalRuntimeApple.mm. The complex demo reuses original package textures and packed-alpha MP4 assets. Two demo recipes combine existing flower, backdrop and animation packages; they are explicitly identified as compositions of those sources, not untouched desktop templates. Development media are excluded from npm distributions.

The compiled renderer includes Skia and enabled dependencies: FreeType, HarfBuzz, ICU, Expat, libjpeg-turbo, libpng, libwebp, Wuffs, zlib, and Skia's in-tree color conversion code. nlohmann/json 3.12.0 headers are vendored for native JSON decoding. Relevant upstream notices and license texts are retained under LICENSES/.

The development fixtures use Inter and Source Han Sans SC fonts. Their license texts are retained under LICENSES/. Fonts and native template packages are development fixtures only and are excluded from the npm distribution. A consumer supplying fonts and template resources is responsible for retaining the notices applying to those assets.

Pinned compiler and Skia revisions are in scripts/common.mjs and the built dist/build-info.json. These build tools are excluded from npm distributions.
