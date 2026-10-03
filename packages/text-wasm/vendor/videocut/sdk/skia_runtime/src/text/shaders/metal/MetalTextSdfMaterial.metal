
#include <metal_stdlib>
using namespace metal;

constant uint kLayerStroke = 1u;
constant uint kLayerOuterShadow = 2u;
constant uint kLayerInnerShadow = 3u;
constant uint kLayerGlow = 4u;

constant uint kMaterialSolid = 0u;
constant uint kMaterialLinearGradient = 1u;
constant uint kMaterialRadialGradient = 2u;
constant uint kMaterialTexture = 3u;

constant uint kDomainGlyph = 0u;
constant uint kDomainLine = 1u;

constant uint kSpreadRepeat = 1u;
constant uint kSpreadMirror = 2u;

constant uint kTextureCover = 0u;
constant uint kTextureContain = 1u;
constant uint kTextureTile = 3u;

constant uint kBlendSourceOver = 0u;
constant uint kBlendMultiply = 1u;
constant uint kBlendScreen = 2u;
constant uint kBlendOverlay = 3u;
constant uint kBlendAdd = 4u;
constant uint kBlendDarken = 5u;
constant uint kBlendLighten = 6u;

struct TextSdfMaterialUniforms {
  float4x4 localToPresentation;
  float4 qtLetterPositions[4];
  float4x4 qtLetterMvp;
  float4 qtLetterOffset;
  float4 qtInnerShadowUvOffset;
  float4 destinationSize;
  float4 distanceTextureSize;
  float4 glyphAtlasRect;
  float4 glyphLocalRect;
  float4 glyphMaterialRect;
  float4 lineRect;
  float4 textRect;
  float4 replacementMaskRect;
  float4 distanceAndOpacity;
  float4 layerGeometry0;
  float4 layerGeometry1;
  float4 layerGeometry2;
  float4 linearGradient;
  float4 radialAndCoordinates;
  float4 materialTransform;
  float4 textureTransform;
  float4 textureCoordinateScale;
  float4 textureAtlas;
  float4 materialColor;
  float4 underlayColor;
  float4 underlayGradient;
  float4 presentationTint;
  uint4 modes0;
  uint4 modes1;
  uint4 modes2;
  uint4 modes3;
  uint4 modes4;
  float4 gradientOffsets[4];
  float4 gradientColors[16];
};

struct TextSdfMaterialVaryings {
  float4 position [[position]];
  float2 sdfUv;
  float2 localPosition;
  float2 textureUv;
  float gradientPosition;
  float underlayGradientPosition;
};

static inline float2 rotateAboutCenter(float2 value, float radians) {
  float2 centered = value - float2(0.5f);
  float sine = sin(radians);
  float cosine = cos(radians);
  return float2(centered.x * cosine + centered.y * sine,
                centered.y * cosine - centered.x * sine) + float2(0.5f);
}

static inline float2 orientTextureCoordinate(float2 uv, uint orientation) {
  if (orientation == 1u)
    return float2(uv.y, 1.0f - uv.x);
  if (orientation == 2u)
    return float2(1.0f) - uv;
  if (orientation == 3u)
    return float2(1.0f - uv.y, uv.x);
  return uv;
}

static inline float2 glyphTextureCoordinate(
    float2 sdfUv, constant TextSdfMaterialUniforms &uniforms) {
  float2 sdfMinimum =
      uniforms.glyphAtlasRect.xy * uniforms.distanceTextureSize.zw;
  float2 sdfMaximum =
      (uniforms.glyphAtlasRect.xy + uniforms.glyphAtlasRect.zw) *
      uniforms.distanceTextureSize.zw;
  float2 uv =
      (sdfUv - ((sdfMinimum + sdfMaximum) * 0.5f)) *
      uniforms.textureCoordinateScale.xy;
  float2 atlasGrid = max(uniforms.textureAtlas.xy, float2(1.0f));
  uv *= float2(1.0f) / atlasGrid;
  float textureScale = max(uniforms.materialTransform.y, 0.00001f);
  float2 signedScale = float2(
      uniforms.modes2.x != 0u ? -textureScale : textureScale,
      uniforms.modes2.y != 0u ? -textureScale : textureScale);
  uv *= float2(1.0f) / signedScale;
  float cosine = cos(uniforms.materialTransform.z);
  float sine = sin(uniforms.materialTransform.z);
  uv = float2(uv.x * cosine + uv.y * sine,
              uv.x * -sine + uv.y * cosine);
  float cellCenterX =
      (-0.5f + 0.5f / atlasGrid.x) +
      fract(uniforms.textureAtlas.z / atlasGrid.x);
  uv.x += cellCenterX;
  uv += float2(0.5f);
  uv = orientTextureCoordinate(uv, uniforms.modes2.z);
  if (uniforms.modes4.w != 0u)
    uv.y = 1.0f - uv.y;
  return uv;
}

vertex TextSdfMaterialVaryings videoCutTextSdfMaterialVertex(
    uint vertexId [[vertex_id]],
    constant TextSdfMaterialUniforms &uniforms [[buffer(0)]]) {
  constexpr float2 corners[6] = {
      // Qt's indexed quad is (0,2,1), (0,3,2). Preserve that winding and
      // interpolation order even though culling is disabled.
      float2(0.0f, 0.0f), float2(1.0f, 1.0f),
      float2(1.0f, 0.0f), float2(0.0f, 0.0f),
      float2(0.0f, 1.0f), float2(1.0f, 1.0f)};
  constexpr uint cornerIndices[6] = {0u, 2u, 1u, 0u, 3u, 2u};
  float2 corner = corners[vertexId];
  float2 baseLocal = uniforms.glyphLocalRect.xy +
                     corner * uniforms.glyphLocalRect.zw;
  float2 local = baseLocal;
  uint layerKind = uniforms.modes0.x;
  // TextPro offsets outer material quads in authored glyph coordinates. The
  // SDF atlas conversion is used only by the inner-shadow lookup below.
  if (layerKind != kLayerInnerShadow)
    local += uniforms.layerGeometry0.xy;
  float4 qtPosition = uniforms.qtLetterPositions[cornerIndices[vertexId]];
  // Preserve the source Letter shader's polar evaluation order when the
  // authored template carries that domain.  Cartesian offsets remain the
  // ordinary editor/fallback contract.
  if (uniforms.qtLetterOffset.z != 0.0f) {
    float radius = uniforms.qtLetterOffset.x;
    float angle = uniforms.qtLetterOffset.y;
    qtPosition.xy += radius * float2(cos(angle), sin(angle));
  } else {
    qtPosition.xy += uniforms.qtLetterOffset.xy;
  }
  float4 presentation = uniforms.qtLetterMvp * qtPosition;
  // The captured Letter pass carries a legacy negative clip-space Z together
  // with a different depth-clip state. This material target has no depth
  // attachment; retain its established in-range Z while preserving Qt's
  // evidence-derived X/Y/W operation order.
  presentation.z = presentation.w * 0.5f;
  TextSdfMaterialVaryings output;
  output.position = presentation;
  // The exact TextPro distance mesh is authored in its native bottom-up
  // atlas domain.  The legacy readback path reverses those rows before a
  // top-down Skia consumer sees them; this direct Metal material path samples
  // the native target in place, so perform that same row projection in UV.
  float2 atlasCorner = float2(corner.x, 1.0f - corner.y);
  float2 atlasPixel = uniforms.glyphAtlasRect.xy +
                      atlasCorner * uniforms.glyphAtlasRect.zw;
  output.sdfUv = atlasPixel * uniforms.distanceTextureSize.zw;
  output.localPosition = baseLocal;
  // TEXT_LETTER_MAT produces vGradientTexcoord in the vertex shader and the
  // fragment shader only samples the RGBA8 LUT.  Keep the same float32
  // operation/interpolation boundary: recomputing the affine projection from
  // localPosition in the fragment is mathematically equivalent, but changes
  // LUT phase at steep byte-domain transitions.
  float4 gradientBounds =
      uniforms.modes0.z == kDomainGlyph
          ? uniforms.glyphMaterialRect
          : (uniforms.modes0.z == kDomainLine
                 ? uniforms.lineRect
                 : uniforms.textRect);
  float gradientOutset = uniforms.radialAndCoordinates.w;
  float2 gradientOrigin = gradientBounds.xy - float2(gradientOutset);
  float2 gradientExtent = max(
      gradientBounds.zw + float2(gradientOutset * 2.0f),
      float2(0.00001f));
  float2 normalizedGradient =
      (baseLocal - gradientOrigin) / gradientExtent;
  normalizedGradient =
      ((normalizedGradient - float2(0.5f)) /
       max(uniforms.materialTransform.x, 0.00001f)) +
      float2(0.5f);
  output.gradientPosition =
      dot(normalizedGradient - uniforms.linearGradient.xy,
          uniforms.linearGradient.zw);
  float2 underlayCoordinate =
      (baseLocal - uniforms.glyphLocalRect.xy) /
      max(uniforms.glyphLocalRect.zw, float2(0.00001f));
  output.underlayGradientPosition =
      dot(underlayCoordinate - uniforms.underlayGradient.xy,
          uniforms.underlayGradient.zw);
  // QtTextLetter emits vTextureTexcoord from the vertex shader. Keeping this
  // affine mapping here preserves the same interpolation and float32 rounding
  // contract instead of recomputing it independently for every fragment.
  output.textureUv = uniforms.modes1.y != 0u
                         ? glyphTextureCoordinate(output.sdfUv, uniforms)
                         : float2(0.0f);
  return output;
}

static inline float unpackDistance(float2 packedDistance) {
  return dot(packedDistance, float2(1.0f / 256.0f, 1.0f));
}

static inline float sampleNormalizedDistance(
    texture2d<float> distanceTexture, float2 uv,
    constant TextSdfMaterialUniforms &uniforms) {
  // The typed material executor owns a native RG8Unorm Metal texture, so it
  // can use the same hardware-linear sample observed in TEXT_LETTER_MAT.
  // Manual byte recovery was required only by the legacy SkRuntimeEffect
  // half4 child ABI; retaining it here changes Metal's interpolation precision
  // and produces small but systematic edge/shadow/glow intensity errors.
  constexpr sampler linearDistanceSampler(coord::normalized,
                                           address::clamp_to_edge,
                                           filter::linear);
  return unpackDistance(
      distanceTexture.sample(linearDistanceSampler, uv).rg);
}

static inline float2 localOffsetToSdfUv(
    float2 localOffset,
    constant TextSdfMaterialUniforms &uniforms) {
  float2 atlasPerLocal = uniforms.glyphAtlasRect.zw /
                         max(uniforms.glyphLocalRect.zw, float2(0.00001f));
  return float2(localOffset.x * atlasPerLocal.x,
                -localOffset.y * atlasPerLocal.y) *
         uniforms.distanceTextureSize.zw;
}

// TEXT_LETTER_MAT keeps the source and TEXT_SDF_BLUR_MAT atlases as
// independent inputs even on the captured RG8 Metal lane, where the blur
// target's format stores the source-distance pair in RG. Preserve that dual
// input ABI here: the native backend intentionally aliases both bindings for
// the captured RG8 projection, while other backends may supply the 25-tap
// blurred atlas produced by the legacy raster path.
static inline float materialDistance(
    texture2d<float> distanceTexture,
    texture2d<float> blurredDistanceTexture, float2 uv, float blurMix,
    constant TextSdfMaterialUniforms &uniforms) {
  float raw = sampleNormalizedDistance(distanceTexture, uv, uniforms);
  float blurred =
      sampleNormalizedDistance(blurredDistanceTexture, uv, uniforms);
  return mix(raw, blurred, blurMix);
}

static inline bool insideGlyphDistanceDomain(
    float2 uv, constant TextSdfMaterialUniforms &uniforms) {
  float2 atlasPosition = uv * uniforms.distanceTextureSize.xy;
  float2 atlasMinimum = uniforms.glyphAtlasRect.xy;
  float2 atlasMaximum = atlasMinimum + uniforms.glyphAtlasRect.zw;
  return all(atlasPosition >= atlasMinimum) &&
         all(atlasPosition <= atlasMaximum);
}

static inline float boundedMaterialDistance(
    texture2d<float> distanceTexture,
    texture2d<float> blurredDistanceTexture, float2 uv, float blurMix,
    constant TextSdfMaterialUniforms &uniforms) {
  return insideGlyphDistanceDomain(uv, uniforms)
             ? materialDistance(distanceTexture, blurredDistanceTexture, uv,
                                blurMix, uniforms)
             : 0.0f;
}

static inline float textProContourSmooth(
    float centerDistance,
    constant TextSdfMaterialUniforms &uniforms) {
  // This typed executor is a native Metal render pass, so its helper-lane
  // topology is the same one used by the captured TEXT_LETTER_MAT fragment.
  // The explicit four-sample reconstruction was needed only while this code
  // ran inside a Ganesh SkRuntimeEffect with the opposite render-target
  // convention.  Keeping that reconstruction after moving to native Metal
  // changes the sampled derivative at every glyph edge.
  return fwidth(centerDistance) * uniforms.distanceAndOpacity.w;
}

static inline float linearStep(float edge0, float edge1, float value) {
  return fast::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
}

static inline float spreadCoordinate(float value, uint spread) {
  if (spread == kSpreadRepeat)
    return fract(value);
  if (spread == kSpreadMirror) {
    float repeated = fract(value * 0.5f) * 2.0f;
    return repeated <= 1.0f ? repeated : 2.0f - repeated;
  }
  return clamp(value, 0.0f, 1.0f);
}

static inline float4 premultiply(float4 color) {
  return float4(color.rgb * color.a, color.a);
}

static inline float4 roundedRgba8(float4 color) {
  return floor(clamp(color, 0.0f, 1.0f) * 255.0f + float4(0.5f)) *
         (1.0f / 255.0f);
}

static inline float gradientOffset(
    constant TextSdfMaterialUniforms &uniforms, uint index) {
  return uniforms.gradientOffsets[index / 4u][index % 4u];
}

static inline float4 sampleContinuousGradient(
    float position, constant TextSdfMaterialUniforms &uniforms) {
  uint count = uniforms.modes1.w;
  float coordinate = spreadCoordinate(position, uniforms.modes0.w);
  // TEXT_LETTER_MAT keeps authored colors in straight RGBA. Coverage is
  // applied only by the final outColor * mask expression; premultiplying the
  // LUT here darkens every partially transparent gradient edge a second time.
  float4 result = uniforms.gradientColors[0];
  for (uint index = 1u; index < 16u; ++index) {
    if (index >= count)
      break;
    float rightOffset = gradientOffset(uniforms, index);
    float leftOffset = gradientOffset(uniforms, index - 1u);
    float amount = clamp((coordinate - leftOffset) /
                             max(rightOffset - leftOffset, 0.000001f),
                         0.0f, 1.0f);
    if (coordinate <= rightOffset) {
      float4 left = uniforms.gradientColors[index - 1u];
      float4 right = uniforms.gradientColors[index];
      result = mix(left, right, amount);
      break;
    }
    result = uniforms.gradientColors[index];
  }
  return result;
}

static inline float4 sampleGradient(
    float position, texture2d<float> gradientTexture,
    sampler gradientSampler,
    constant TextSdfMaterialUniforms &uniforms) {
  // TextPro uploads a real 256x1 RGBA8Unorm texture and samples it directly
  // with the vertex-produced gradient coordinate. The captured runtime MSL
  // binds this texture to its dedicated linear sampler and returns straight
  // RGBA; its CPU construction and byte boundaries must not be reconstructed
  // or premultiplied in this fragment.
  return uniforms.modes1.z != 0u
             ? gradientTexture.sample(
                   gradientSampler,
                   float2(clamp(position, 0.0f, 1.0f), 0.5f))
             : sampleContinuousGradient(position, uniforms);
}

static inline float4 coordinateRect(
    constant TextSdfMaterialUniforms &uniforms) {
  uint domain = uniforms.modes0.z;
  if (uniforms.modes0.y == kMaterialTexture && uniforms.modes1.y != 0u)
    domain = kDomainGlyph;
  if (domain == kDomainGlyph)
    return uniforms.glyphMaterialRect;
  if (domain == kDomainLine)
    return uniforms.lineRect;
  return uniforms.textRect;
}

static inline float2 materialCoordinate(
    float2 localPosition, constant TextSdfMaterialUniforms &uniforms) {
  float4 bounds = coordinateRect(uniforms);
  float outset = uniforms.radialAndCoordinates.w;
  float2 origin = bounds.xy - float2(outset);
  float2 extent = max(bounds.zw + float2(outset * 2.0f), float2(0.00001f));
  float2 coordinate = (localPosition - origin) / extent;
  float coordinateScale = uniforms.materialTransform.x;
  return ((coordinate - float2(0.5f)) /
          max(coordinateScale, 0.00001f)) + float2(0.5f);
}

static inline float4 sampleAuthoredGradient(
    float2 localPosition, float vertexLinearPosition,
    texture2d<float> gradientTexture, sampler gradientSampler,
    constant TextSdfMaterialUniforms &uniforms) {
  if (uniforms.modes0.y == kMaterialRadialGradient) {
    return sampleGradient(
        distance(localPosition, uniforms.radialAndCoordinates.xy) *
            uniforms.radialAndCoordinates.z,
        gradientTexture, gradientSampler, uniforms);
  }
  return sampleGradient(vertexLinearPosition, gradientTexture,
                        gradientSampler, uniforms);
}

static inline float4 sampleTextureMaterial(
    float2 coordinate, float2 underlayCoordinate, float2 textureUv,
    float underlayGradientPosition,
    texture2d<float> sourceTexture, texture2d<float> gradientTexture,
    sampler sourceSampler, sampler gradientSampler,
    constant TextSdfMaterialUniforms &uniforms) {
  float2 uv = coordinate;
  float2 domainSize = max(coordinateRect(uniforms).zw, float2(0.00001f));
  bool contained = true;
  uint fit = uniforms.modes1.x;
  if (uniforms.modes1.y != 0u) {
    uv = textureUv;
  } else {
    float sourceAspect = uniforms.textureTransform.z /
                         max(uniforms.textureTransform.w, 0.00001f);
    float domainAspect = domainSize.x / domainSize.y;
    if (fit == kTextureCover) {
      if (sourceAspect > domainAspect)
        uv.x = (uv.x - 0.5f) * domainAspect / sourceAspect + 0.5f;
      else
        uv.y = (uv.y - 0.5f) * sourceAspect / domainAspect + 0.5f;
    } else if (fit == kTextureContain) {
      if (sourceAspect > domainAspect)
        uv.y = (uv.y - 0.5f) * sourceAspect / domainAspect + 0.5f;
      else
        uv.x = (uv.x - 0.5f) * domainAspect / sourceAspect + 0.5f;
    }
    uv = ((uv - float2(0.5f)) /
          max(uniforms.materialTransform.y, 0.00001f)) + float2(0.5f);
    uv -= uniforms.textureTransform.xy / domainSize;
    uv = rotateAboutCenter(uv, uniforms.materialTransform.z);
    if (uniforms.modes2.x != 0u)
      uv.x = 1.0f - uv.x;
    uv = orientTextureCoordinate(uv, uniforms.modes2.z);
    if (fit == kTextureContain)
      contained = all(uv >= float2(0.0f)) && all(uv <= float2(1.0f));
    if (fit == kTextureTile)
      uv = fract(uv);
    else
      uv = clamp(uv, 0.0f, 1.0f);
    const bool invertY = (uniforms.modes2.y != 0u) !=
                         (uniforms.modes4.w != 0u);
    if (invertY)
      uv.y = 1.0f - uv.y;
  }
  // QtTextLetter keeps filtered texture and gradient arithmetic in float.
  // Narrowing here changes opaque texture bytes before coverage is applied.
  float4 textureColor = sourceTexture.sample(sourceSampler, uv);
  if (!contained)
    textureColor = float4(0.0f);
  textureColor *= uniforms.layerGeometry2.w;
  if ((uniforms.modes3.x & 1u) != 0u) {
    float4 underlay = uniforms.underlayColor;
    textureColor = uniforms.modes2.w != 0u
                       ? underlay * textureColor.a
                       : underlay * (1.0f - textureColor.a) + textureColor;
  } else if (uniforms.modes3.y != 0u) {
    float4 underlay = sampleGradient(underlayGradientPosition,
                                    gradientTexture, gradientSampler, uniforms);
    textureColor = uniforms.modes2.w != 0u
                       ? underlay * textureColor.a
                       : underlay * (1.0f - textureColor.a) + textureColor;
  }
  return textureColor;
}

static inline float textProShadowBlurMix(
    constant TextSdfMaterialUniforms &uniforms) {
  const uint layerKind = uniforms.modes0.x;
  const bool shadowVariant =
      layerKind == kLayerGlow ||
      (layerKind == kLayerOuterShadow && uniforms.modes3.w != 1u);
  if (!shadowVariant)
    return 0.0f;
  float distanceDenominator =
      max(uniforms.distanceAndOpacity.x, 1.0f) * 2.0f;
  float extraSmooth =
      (max(uniforms.layerGeometry0.w, 0.0f) +
       max(uniforms.distanceAndOpacity.y, 0.0f)) /
      distanceDenominator;
  return linearStep(0.0f, 0.1f, extraSmooth);
}

static inline float materialMask(
    float centerDistance, float originalDistance, float2 centerUv,
    float contourSmooth, float blurMix,
    texture2d<float> distanceTexture,
    texture2d<float> blurredDistanceTexture,
    constant TextSdfMaterialUniforms &uniforms) {
  uint layerKind = uniforms.modes0.x;
  float distanceRange = max(uniforms.distanceAndOpacity.x, 1.0f);
  float distanceDenominator = distanceRange * 2.0f;
  // Native feather masks also contract their contour before smoothing.
  bool signedFeather = layerKind == kLayerOuterShadow && uniforms.modes3.w == 1u;
  float spreadWidth = (signedFeather ? uniforms.layerGeometry0.z
                                    : max(uniforms.layerGeometry0.z, 0.0f)) /
                      distanceDenominator;
  float authoredBlur = max(uniforms.layerGeometry0.w, 0.0f);
  float requestedBlur = max(uniforms.distanceAndOpacity.y, 0.0f);
  float blurRadius = authoredBlur + requestedBlur;
  float startWidth = 0.0f;
  float endWidth = 0.0f;
  float extraSmooth = 0.0f;
  float smoothIntensity = 0.0f;
  uint passMode = 0u;

  if (layerKind == kLayerStroke) {
    passMode = 1u;
    float width = max(uniforms.layerGeometry1.x, 0.0f);
    if ((uniforms.modes3.x & 2u) != 0u)
      startWidth = uniforms.layerGeometry2.w * 0.5f /
                   distanceDenominator;
    else if (uniforms.layerGeometry0.z > 0.0f)
      startWidth = width * 0.5f / distanceDenominator;
    else
      startWidth = max(uniforms.layerGeometry1.y, 0.0f) * 0.5f /
                   distanceDenominator;
    endWidth = (width * 0.5f + max(uniforms.layerGeometry0.z, 0.0f)) /
               distanceDenominator;
    extraSmooth = blurRadius / distanceDenominator;
  } else if (layerKind == kLayerInnerShadow) {
    passMode = 3u;
    endWidth = spreadWidth;
    // Qt Letter captures contain two authored inner-shadow smoothing domains.
    // The layer receipt owns the scale; applying it here preserves subsequent
    // user edits without a package-id or blur-magnitude branch.
    extraSmooth = blurRadius * uniforms.qtInnerShadowUvOffset.w /
                  distanceDenominator;
  } else if (layerKind == kLayerOuterShadow || layerKind == kLayerGlow) {
    passMode = 2u;
    endWidth = spreadWidth;
    extraSmooth = blurRadius / distanceDenominator;
    smoothIntensity = blurRadius / (2.5f * distanceDenominator);
    if (layerKind == kLayerOuterShadow && uniforms.modes3.w == 1u) {
      // LegacyFeather does not compile TextPro's _SHADOW branch. It is the
      // ordinary expanded fill mask with the same extraSmooth binding.
      passMode = 0u;
      smoothIntensity = -5.0f;
    } else if (layerKind == kLayerOuterShadow &&
               uniforms.modes3.w == 2u) {
      smoothIntensity = uniforms.layerGeometry2.z;
    }
  } else if (requestedBlur > 0.0f) {
    extraSmooth = requestedBlur / distanceDenominator;
  }

  float smoothing = contourSmooth + extraSmooth;
  constexpr float boundary = 0.5f;
  constexpr float minimumSdf = 0.05f;
  float mask = 0.0f;
  if (passMode == 0u || passMode == 3u) {
    float edge = fast::clamp(boundary - endWidth, minimumSdf, 1.0f);
    float edge0 = fast::clamp(edge - smoothing, minimumSdf, 1.0f);
    float edge1 = fast::clamp(edge + smoothing, minimumSdf, 1.0f);
    mask = linearStep(edge0, edge1, centerDistance);
  } else if (passMode == 1u) {
    float inner = fast::clamp(boundary - startWidth, minimumSdf, 1.0f);
    float outer = fast::clamp(boundary - endWidth, minimumSdf, 1.0f);
    float inner0 = fast::clamp(inner - smoothing, minimumSdf, 1.0f);
    float inner1 = fast::clamp(inner + smoothing, minimumSdf, 1.0f);
    float outer0 = fast::clamp(outer - smoothing, minimumSdf, 1.0f);
    float outer1 = fast::clamp(outer + smoothing, minimumSdf, 1.0f);
    mask = linearStep(outer0, outer1, centerDistance) *
           (1.0f - linearStep(inner0, inner1, centerDistance));
  } else {
    float edge = fast::clamp(boundary - endWidth, minimumSdf, 1.0f);
    // TEXT_LETTER_MAT's _SHADOW branch intentionally allows its softened
    // edge to reach zero. Fill, outline, and inner-shadow retain u_minSDF;
    // applying it here clips the low-intensity shadow/glow fringe.
    float edge0 = fast::clamp(edge - smoothing, 0.0f, 1.0f);
    float edge1 = fast::clamp(edge + smoothing, 0.0f, 1.0f);
    mask = linearStep(edge0, edge1, centerDistance);

    float2 roundUv = float2(25.0f) * uniforms.distanceTextureSize.zw;
    float distanceUp = boundedMaterialDistance(
        distanceTexture, blurredDistanceTexture,
        centerUv + float2(0.0f, roundUv.y), blurMix, uniforms);
    float distanceBottom = boundedMaterialDistance(
        distanceTexture, blurredDistanceTexture,
        centerUv - float2(0.0f, roundUv.y), blurMix, uniforms);
    float distanceLeft = boundedMaterialDistance(
        distanceTexture, blurredDistanceTexture,
        centerUv - float2(roundUv.x, 0.0f), blurMix, uniforms);
    float distanceRight = boundedMaterialDistance(
        distanceTexture, blurredDistanceTexture,
        centerUv + float2(roundUv.x, 0.0f), blurMix, uniforms);
    float distanceRound =
        (distanceUp + distanceBottom + distanceLeft + distanceRight) * 0.25f;
    float maskRound = linearStep(edge0, edge1, distanceRound);
    float ratio = 1.0f - clamp(smoothIntensity * 1.25f, 0.0f, 1.0f);
    mask = ratio * clamp(mask, 0.0f, 1.0f) +
           (1.0f - ratio) * clamp(maskRound, 0.0f, 1.0f);

    float xVariable = 1.0f - mask;
    constexpr float xEdgeToZero = 0.99f;
    constexpr float xEdgeToOne = 0.35f;
    constexpr float xRadius = xEdgeToZero - xEdgeToOne;
    constexpr float inverseSigma = 2.5f;
    float ratioX = clamp(xVariable - xEdgeToOne, 0.0f, xRadius) *
                   inverseSigma;
    float zeroValue = exp(-xRadius * xRadius * inverseSigma * inverseSigma);
    mask = linearStep(zeroValue, 1.0f, exp(-ratioX * ratioX));
  }

  if (passMode == 3u) {
    float originMask = linearStep(boundary - contourSmooth,
                                  boundary + contourSmooth,
                                  originalDistance);
    mask *= originMask;
  }

  // The typed model's extrusion is a post-TextPro extension. Preserve it on
  // top of the recovered mask without changing the legacy zero-thickness
  // path used by the initial template set.
  float thickness = uniforms.layerGeometry1.z;
  if (layerKind == kLayerOuterShadow && thickness > 0.00001f) {
    float2 direction = float2(cos(uniforms.layerGeometry1.w),
                              sin(uniforms.layerGeometry1.w));
    for (uint step = 1u; step < 15u; ++step) {
      float amount = thickness * float(step) * (1.0f / 14.0f);
      float2 sampleUv = centerUv + localOffsetToSdfUv(
                                       -direction * amount, uniforms);
      float projected = materialDistance(
          distanceTexture, blurredDistanceTexture, sampleUv, blurMix,
          uniforms);
      float edge = clamp(boundary - endWidth, minimumSdf, 1.0f);
      float edge0 = clamp(edge - smoothing, 0.0f, 1.0f);
      float edge1 = clamp(edge + smoothing, 0.0f, 1.0f);
      mask = max(mask, linearStep(edge0, edge1, projected));
    }
  }
  return mask;
}

static inline float3 blendStraight(float3 source, float3 destination,
                                   uint blend) {
  if (blend == kBlendMultiply)
    return source * destination;
  if (blend == kBlendScreen)
    return source + destination - source * destination;
  if (blend == kBlendOverlay) {
    return select(2.0f * source * destination,
                  1.0f - 2.0f * (1.0f - source) * (1.0f - destination),
                  destination >= float3(0.5f));
  }
  if (blend == kBlendDarken)
    return min(source, destination);
  if (blend == kBlendLighten)
    return max(source, destination);
  return source;
}

static inline float4 compositeMaterial(float4 source, float4 destination,
                                       uint blend) {
  if (blend == kBlendSourceOver)
    return source + destination * (1.0f - source.a);
  if (blend == kBlendAdd)
    return min(source + destination, float4(1.0f));
  float sourceAlpha = source.a;
  float destinationAlpha = destination.a;
  float3 sourceStraight = sourceAlpha > 0.0f
                              ? source.rgb / sourceAlpha
                              : float3(0.0f);
  float3 destinationStraight = destinationAlpha > 0.0f
                                   ? destination.rgb / destinationAlpha
                                   : float3(0.0f);
  float3 blended = blendStraight(sourceStraight, destinationStraight, blend);
  float outputAlpha = sourceAlpha + destinationAlpha * (1.0f - sourceAlpha);
  float3 outputRgb =
      source.rgb * (1.0f - destinationAlpha) +
      destination.rgb * (1.0f - sourceAlpha) +
      blended * (sourceAlpha * destinationAlpha);
  return float4(outputRgb, outputAlpha);
}

fragment float4 videoCutTextSdfMaterialFragment(
    TextSdfMaterialVaryings input [[stage_in]],
    constant TextSdfMaterialUniforms &uniforms [[buffer(0)]],
    texture2d<float> distanceTexture [[texture(0)]],
    texture2d<float> sourceTexture [[texture(1)]],
    texture2d<float> destinationSnapshot [[texture(2)]],
    texture2d<float> replacementMask [[texture(3)]],
    texture2d<float> blurredDistanceTexture [[texture(4)]],
    texture2d<float> gradientTexture [[texture(5)]],
    sampler distanceSampler [[sampler(0)]],
    sampler sourceSampler [[sampler(1)]],
    sampler destinationSampler [[sampler(2)]],
    sampler gradientSampler [[sampler(3)]]) {
  // TEXT_LETTER_MAT samples the vertex-produced vSDFTexcoord directly.
  // Recomputing the same affine mapping from localPosition in the fragment
  // changes float32 interpolation/rounding at every contour and material
  // atlas phase.
  float2 originalUv = input.sdfUv;
  float2 centerLocal = input.localPosition;
  float2 centerUv = originalUv;
  if (uniforms.modes0.x == kLayerInnerShadow) {
    centerLocal -= uniforms.layerGeometry0.xy;
    centerUv += uniforms.qtInnerShadowUvOffset.z != 0.0f
                    ? -uniforms.qtInnerShadowUvOffset.xy
                    : localOffsetToSdfUv(-uniforms.layerGeometry0.xy,
                                         uniforms);
  }
  float blurMix = textProShadowBlurMix(uniforms);
  float centerDistance = materialDistance(
      distanceTexture, blurredDistanceTexture, centerUv, blurMix, uniforms);
  float originalDistance = sampleNormalizedDistance(
      distanceTexture, originalUv, uniforms);
  float contourWidth = textProContourSmooth(centerDistance, uniforms);
  float mask = materialMask(centerDistance, originalDistance, centerUv,
                            contourWidth, blurMix, distanceTexture,
                            blurredDistanceTexture, uniforms);
  float2 coordinate = materialCoordinate(input.localPosition, uniforms);
  float4 material;
  if (uniforms.modes0.y == kMaterialSolid) {
    material = roundedRgba8(uniforms.materialColor);
  } else if (uniforms.modes0.y == kMaterialLinearGradient ||
             uniforms.modes0.y == kMaterialRadialGradient) {
    material = sampleAuthoredGradient(input.localPosition,
                                      input.gradientPosition,
                                      gradientTexture, sourceSampler,
                                      uniforms);
  } else {
    float2 underlayCoordinate =
        (input.localPosition - uniforms.glyphLocalRect.xy) /
        max(uniforms.glyphLocalRect.zw, float2(0.00001f));
    material = sampleTextureMaterial(coordinate, underlayCoordinate,
                                     input.textureUv,
                                     input.underlayGradientPosition,
                                     sourceTexture, gradientTexture,
                                     sourceSampler, gradientSampler, uniforms);
  }
  if (uniforms.modes4.x != 0u) {
    float2 maskUv =
        (input.localPosition - uniforms.replacementMaskRect.xy) /
        max(uniforms.replacementMaskRect.zw, float2(0.00001f));
    maskUv = orientTextureCoordinate(clamp(maskUv, 0.0f, 1.0f),
                                     uniforms.modes4.z);
    if (uniforms.modes4.y != 0u)
      maskUv.y = 1.0f - maskUv.y;
    mask *= replacementMask.sample(sourceSampler, maskUv).a;
  }
  material *= premultiply(uniforms.presentationTint);
  float opacity = clamp(uniforms.distanceAndOpacity.z, 0.0f, 1.0f);
  // The recovered native TEXT_LETTER_MAT fragment keeps outColor and mask in
  // float through the final multiplication.  The old SkRuntimeEffect path's
  // half4 return ABI is not part of the native Metal contract and introduces
  // extra edge/material quantization before the RGBA8 target write.
  float4 source = material * (mask * opacity);
  // The captured Qt Letter SourceOver path returns premultiplied source and
  // lets the RGBA8 render target perform One/OneMinusSourceAlpha blending.
  if (uniforms.modes3.z == kBlendSourceOver)
    return source;
  float2 destinationUv =
      input.position.xy * uniforms.destinationSize.zw;
  float4 destination =
      destinationSnapshot.sample(destinationSampler, destinationUv);
  return compositeMaterial(source, destination, uniforms.modes3.z);
}
