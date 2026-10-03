# Product-owned shaders are embedded at build time; no runtime source files.
set(videocut_text_shader_sources
    src/text/shaders/material/MaterialModulate.sksl
    src/text/shaders/material/MaterialParticleCircle.sksl
    src/text/shaders/material/MaterialGaussian.sksl
    src/text/shaders/material/MaterialTransition.sksl
    src/text/shaders/material/MaterialDirectionalBoxBlur.sksl
    src/text/shaders/material/MaterialNoiseDissolve.sksl
    src/text/shaders/material/MaterialBallistic.sksl
    src/text/shaders/material/MaterialAxisBlur.sksl
    src/text/shaders/material/MaterialSpiralWarp.sksl
    src/text/shaders/material/MaterialFlux.sksl
    src/text/shaders/material/MaterialGlow.sksl
    src/text/shaders/material/MaterialMultimesh.sksl
    src/text/shaders/material/MaterialLineColumn.sksl
    src/text/shaders/material/MaterialMaskMotion.sksl
    src/text/shaders/material/MaterialFaceNoise.sksl
    src/text/shaders/material/MaterialDualOffset.sksl
    src/text/shaders/material/MaterialShapedRamp.sksl
    src/text/shaders/material/MaterialWave.sksl
    src/text/shaders/material/MaterialAlphaOutline.sksl
    src/text/shaders/metal/MetalMaterialTurbulence.metal
    src/text/shaders/material/MaterialFlowWarp.sksl
    src/text/shaders/material/MaterialDeepGlowScreen.sksl
    src/text/shaders/material/MaterialProjectionComposite.sksl
    src/text/shaders/material/MaterialParticleScatter.sksl
    src/text/shaders/material/MaterialCylinder.sksl
    src/text/shaders/material/MaterialUnitedShadow.sksl
    src/text/shaders/material/MaterialClosed.sksl
    src/text/shaders/material/MaterialMaskedCutLine.sksl
    src/text/shaders/material/MaterialCutLineReveal.sksl
    src/text/shaders/material/MaterialPackedGaussian.sksl
    src/text/shaders/material/MaterialDualPackedGlow.sksl
    src/text/shaders/material/MaterialEncodedAlphaBlur.sksl
    src/text/shaders/material/MaterialSdfNormalSweep.sksl
    src/text/shaders/material/MaterialLightSweepGlow.sksl
    src/text/shaders/material/MaterialRadialDecayHsv.sksl
    src/text/shaders/material/MaterialFixedNineTap.sksl
    src/text/shaders/material/MaterialHsvOriginalOver.sksl
    src/text/shaders/post/PostRadianceThreshold.sksl
    src/text/shaders/post/PostRadianceErode.sksl
    src/text/shaders/post/PostLinearBlit.sksl
    src/text/shaders/post/PostRadianceDirectionalBlur.sksl
    src/text/shaders/post/PostRadianceBlend.sksl
    src/text/shaders/post/PostSoftGlowBlend.sksl
    src/text/shaders/post/PostGaussianBlurSeparable.sksl
    src/text/shaders/post/PostDeepGlowPreprocess.sksl
    src/text/shaders/post/PostDeepGlowDownscale.sksl
    src/text/shaders/post/PostDeepGlowBlur.sksl
    src/text/shaders/post/PostDeepGlowComposite.sksl
    src/text/shaders/post/PostDeepGlowPostprocess.sksl
    src/text/shaders/post/PostWaveWarp.sksl
    src/text/shaders/post/PostSGlowThreshold.sksl
    src/text/shaders/post/PostSGlowGaussian.sksl
    src/text/shaders/post/PostSGlowComposite.sksl
    src/text/shaders/post/PostChromaticAberration.sksl
    src/text/shaders/post/PostMultiShadow.sksl
    src/text/shaders/post/PostSoftGlowSeparable.sksl
    src/text/shaders/post/PostAlphaOutline.sksl
    src/text/shaders/post/PostTurbulenceNoise.sksl
    src/text/shaders/post/PostTurbulenceDisplacement.sksl
    src/text/shaders/post/PostGodRayThreshold.sksl
    src/text/shaders/post/PostGodRayGaussian.sksl
    src/text/shaders/post/PostGodRayComposite.sksl
    src/text/shaders/post/PostLinearWipe.sksl
    src/text/shaders/post/PostSimpleChokerDownscale.sksl
    src/text/shaders/post/PostSimpleChokerMatte.sksl
    src/text/shaders/post/PostSimpleChokerBlend.sksl
    src/text/shaders/post/PostOpticsCompensationDistort.sksl
    src/text/shaders/post/PostOpticsCompensationAntiAliasing.sksl
    src/text/shaders/post/PostProjectionCopyScale.sksl
    src/text/shaders/post/PostSemanticPostEffect.sksl
    src/text/shaders/gradient/LinearGradient.sksl
    src/text/shaders/gradient/RadialGradient.sksl
    src/text/shaders/metal/MetalQtTextDirectionalBlurs.metal
    src/text/shaders/metal/MetalQtTextFollowerComposite.metal
    src/text/shaders/metal/MetalQtTextLetterFragment.metal
    src/text/shaders/metal/MetalQtTextLetterVertex.metal
    src/text/shaders/metal/MetalQtTextRenderGroupCompositeFragment.metal
    src/text/shaders/metal/MetalQtTextRenderGroupCompositeVertex.metal
    src/text/shaders/metal/MetalQtTextSoftGlow.metal
    src/text/shaders/metal/MetalQtTextTrail.metal
    src/text/shaders/metal/MetalQtTextTurbulenceDisplacement.metal
    src/text/shaders/metal/MetalQtTextTurbulenceNoise.metal
    src/text/shaders/metal/MetalQtTextTurbulenceVertex.metal
    src/text/shaders/metal/MetalTextSdfMaterial.metal
    src/text/shaders/metal/MetalDistortChroma.metal
    src/text/shaders/metal/MetalDustFullscreenVertex.metal
    src/text/shaders/metal/MetalDustNoiseFragment.metal
    src/text/shaders/metal/MetalDustMaskNoiseFragment.metal
    src/text/shaders/metal/MetalDustParticleVertex.metal
    src/text/shaders/metal/MetalDustParticleFragment.metal
    src/text/shaders/metal/MetalEngineCopy.metal
    src/text/shaders/metal/MetalGaussian.metal
    src/text/shaders/metal/MetalRadialBlur.metal
    src/text/shaders/metal/MetalShake.metal
    src/text/shaders/metal/MetalTurbulence.metal
    src/text/shaders/metal/MetalTextSdf.metal
    src/text/shaders/metal/MetalTextVatMesh.metal
)

foreach(shader_relative IN LISTS videocut_text_shader_sources)
    set(shader_path "${CMAKE_CURRENT_LIST_DIR}/../${shader_relative}")
    cmake_path(NORMAL_PATH shader_path)
    get_filename_component(shader_name "${shader_relative}" NAME_WE)
    file(READ "${shader_path}" shader_source)
    string(FIND "${shader_source}" ")vcut_shader\"" shader_delimiter_position)
    if(NOT shader_delimiter_position EQUAL -1)
        message(FATAL_ERROR "Shader contains the reserved C++ delimiter: ${shader_relative}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${shader_path}")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/TextShaderSource.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/text/shaders/${shader_name}.h"
        @ONLY)
endforeach()

set_source_files_properties(${videocut_text_shader_sources} PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(VideoCutSkiaRuntime PRIVATE ${videocut_text_shader_sources})
target_include_directories(VideoCutSkiaRuntime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
