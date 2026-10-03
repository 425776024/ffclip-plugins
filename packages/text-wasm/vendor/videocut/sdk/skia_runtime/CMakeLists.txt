cmake_minimum_required(VERSION 3.20)
project(VideoCutSkiaRuntime LANGUAGES CXX)

if(NOT TARGET videocut_skia::all)
    message(FATAL_ERROR
        "VideoCutSkiaRuntime requires the pinned videocut_skia::all imported target")
endif()

add_library(VideoCutSkiaRuntime STATIC
    src/codec/AnimatedWebpEncoder.cpp
    src/codec/CanonicalRgbaAdapter.cpp
    src/runtime/SkiaRuntime.cpp
    src/raster/SkiaFramePublication.cpp
    src/resources/QtPackedAlphaYuvMergeContract.cpp
    src/resources/QtPackedAlphaYuvMergeRuntimeStub.cpp
    src/resources/SkiaResourceProvider.cpp
    src/text/FontInstanceResolver.cpp
    src/text/FlowerAppearanceComposition.cpp
    src/text/SkiaGpuContextStub.cpp
    src/text/QtTextDustMetalRuntimeStub.cpp
    src/text/QtTextDistortChromaMetalRuntimeStub.cpp
    src/text/QtTextDirectionalBlursContract.cpp
    src/text/QtTextDirectionalBlursRuntimeStub.cpp
    src/text/QtTextEngineCopyMetalRuntimeStub.cpp
    src/text/QtTextGaussianMetalRuntimeStub.cpp
    src/text/QtTextGlyphOutlineRuntimeStub.cpp
    src/text/QtTextLetterMetalRuntimeStub.cpp
    src/text/QtTextRadialBlurMetalRuntimeStub.cpp
    src/text/QtTextShakeMetalRuntimeStub.cpp
    src/text/QtTextSoftGlowMetalRuntimeStub.cpp
    src/text/QtTextTrailRuntimeStub.cpp
    src/text/QtTextTurbulenceMetalRuntimeStub.cpp
    src/text/QtTextPostEffectRuntime.cpp
    src/text/QtTextProPostEffectPipeline.cpp
    src/text/TextEffectFontSizeReflow.cpp
    src/text/TextGlyphOutlineCapture.cpp
    src/text/TextGradientLutShader.cpp
    src/text/TextProLetterTransformF32.cpp
    src/text/TextSdfMeshBuilder.cpp
    src/text/SkiaTextRenderLane.cpp
    src/text/TextVisualAssets.cpp
    src/text/TextLayout.cpp
    src/text/TextEffectFrameInput.cpp
    src/text/TextGlyphMaterials.cpp
    src/text/TextComponentRecorder.cpp
    src/text/TextRenderGroups.cpp
    src/text/TextPostEffectGraph.cpp
    src/text/TextExecutionResources.cpp
    src/text/TextExecutionMaterials.cpp
    src/text/TextExecutionGraph.cpp
    src/text/TextFrameIdentity.cpp
    src/text/TextPresentation.cpp
    src/text/TextFontContext.cpp
    src/text/TextAssetAdmission.cpp
    src/text/QtTextPostEffectParameters.cpp
    src/text/QtTextPostEffectContracts.cpp
    src/text/TextRuntimeShader.cpp
    src/text/TextExecutionParameterContract.cpp
    src/vector/SkiaVectorRenderLane.cpp)
add_library(videocut::skia_runtime ALIAS VideoCutSkiaRuntime)

# Match the host's compact Debug code generation on the render hot path.
# Unoptimized byte-vector construction/destruction dominates real full-batch
# samples even when the actual effect passes already execute on Metal.
if(COMMAND videocut_enable_memory_bounded_debug_codegen)
    videocut_enable_memory_bounded_debug_codegen(VideoCutSkiaRuntime)
endif()

include(cmake/TextShaderSources.cmake)

# The pinned Skia libraries are built without RTTI. These translation units
# derive from Skia interfaces, so they must use the same ABI or their vtables
# will reference Skia typeinfo that intentionally is not exported.
if(MSVC)
    set_source_files_properties(
        src/resources/SkiaResourceProvider.cpp
        src/vector/SkiaVectorRenderLane.cpp
        PROPERTIES COMPILE_OPTIONS "/GR-")
else()
    set_source_files_properties(
        src/resources/SkiaResourceProvider.cpp
        src/vector/SkiaVectorRenderLane.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-rtti")
endif()

set_target_properties(VideoCutSkiaRuntime PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

target_include_directories(VideoCutSkiaRuntime
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(VideoCutSkiaRuntime PRIVATE
    VIDEOCUT_SKIA_REVISION="${VIDEOCUT_SKIA_REVISION}"
    VIDEOCUT_SKIA_RENDERER_PROFILE="${VIDEOCUT_SKIA_ARTIFACT_PROFILE}")
target_link_libraries(VideoCutSkiaRuntime
    PUBLIC VideoCutText VideoCutVector VideoCutMedia
    PRIVATE
        videocut_skia::all
        nlohmann_json::nlohmann_json
        VideoCutGpuFrameBridge
        VideoCutGpuExecution
        VideoCutBase)

if(APPLE)
    target_sources(VideoCutSkiaRuntime PRIVATE
        src/text/QtTextGlyphOutlineRuntimeApple.cpp)
    set_source_files_properties(
        src/text/QtTextGlyphOutlineRuntimeStub.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)
    target_link_libraries(VideoCutSkiaRuntime PRIVATE
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework Foundation")
endif()

if(APPLE AND VIDEOCUT_SKIA_ARTIFACT_PROFILE STREQUAL
        "videocut-skia-metal-v1")
    enable_language(OBJCXX)
    target_sources(VideoCutSkiaRuntime PRIVATE
        src/resources/QtPackedAlphaYuvMergeRuntimeApple.mm
        src/text/QtTextDirectionalBlursRuntimeApple.mm
        src/text/QtTextDistortChromaMetalRuntimeApple.mm
        src/text/QtTextEngineCopyMetalRuntimeApple.mm
        src/text/QtTextGaussianMetalRuntimeApple.mm
        src/text/QtTextLetterMetalRuntimeApple.mm
        src/text/QtTextRadialBlurMetalRuntimeApple.mm
        src/text/QtTextShakeMetalRuntimeApple.mm
        src/text/QtTextSoftGlowMetalRuntimeApple.mm
        src/text/QtTextTrailRuntimeApple.mm
        src/text/QtTextTurbulenceMetalRuntimeApple.mm
        src/text/QtTextDustMetalRuntimeApple.mm
        src/text/SkiaGpuContextApple.mm)
    # The pinned Ganesh Metal headers contain zeroing-weak encoder caches.
    # This bridge otherwise keeps its existing manual Objective-C ownership.
    set_source_files_properties(src/text/SkiaGpuContextApple.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-weak")
    set_source_files_properties(
        src/resources/QtPackedAlphaYuvMergeRuntimeStub.cpp
        src/text/QtTextDirectionalBlursRuntimeStub.cpp
        src/text/QtTextDistortChromaMetalRuntimeStub.cpp
        src/text/QtTextEngineCopyMetalRuntimeStub.cpp
        src/text/QtTextGaussianMetalRuntimeStub.cpp
        src/text/QtTextLetterMetalRuntimeStub.cpp
        src/text/QtTextRadialBlurMetalRuntimeStub.cpp
        src/text/QtTextShakeMetalRuntimeStub.cpp
        src/text/QtTextSoftGlowMetalRuntimeStub.cpp
        src/text/QtTextTrailRuntimeStub.cpp
        src/text/QtTextTurbulenceMetalRuntimeStub.cpp
        src/text/QtTextDustMetalRuntimeStub.cpp
        src/text/SkiaGpuContextStub.cpp
        PROPERTIES HEADER_FILE_ONLY TRUE)
    target_compile_definitions(VideoCutSkiaRuntime PRIVATE
        VIDEOCUT_SKIA_METAL_RUNTIME=1)
    target_link_libraries(VideoCutSkiaRuntime PRIVATE
        "-framework ImageIO"
        "-framework Metal")
endif()

if(APPLE AND VIDEOCUT_SKIA_ARTIFACT_PROFILE STREQUAL
        "videocut-skia-metal-v1")
endif()

if(APPLE AND VIDEOCUT_SKIA_ARTIFACT_PROFILE STREQUAL "videocut-skia-metal-v1")
endif()
