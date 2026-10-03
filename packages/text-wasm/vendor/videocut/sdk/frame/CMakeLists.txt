cmake_minimum_required(VERSION 3.12)

project(VideoCutFrame LANGUAGES CXX)

set(VIDEOCUT_FRAME_PUBLIC_HEADERS
    include/videocut/frame/AudioBuffer.h
    include/videocut/frame/LayerFrame.h
    include/videocut/frame/FrameDesc.h
    include/videocut/frame/FrameExport.h
    include/videocut/frame/FrameMap.h
    include/videocut/frame/FrameProvenance.h
    include/videocut/frame/MediaTime.h
    include/videocut/frame/FrameResult.h
    include/videocut/frame/FrameStorage.h
    include/videocut/frame/FrameTypes.h
    include/videocut/frame/VideoFrame.h
)

set(VIDEOCUT_FRAME_SOURCES
    src/AudioBuffer.cpp
    src/FrameDesc.cpp
    src/FrameProvenance.cpp
    src/FrameInternal.h
    src/FrameStorage.cpp
    src/VideoFrame.cpp
)

# Configure-time dependency fence. This target is intentionally a standard
# library-only leaf; platform/media conversion belongs in boundary adapters.
set(VIDEOCUT_FRAME_FORBIDDEN_INCLUDE_PATTERN
    "^[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]*(imgui|ImMat|immat|VkMat|libav|libsw|vulkan|Metal/|CoreVideo/|Flutter/)[^>\"]*[>\"]") # C8_SCAN_RULE
foreach(source_file IN LISTS VIDEOCUT_FRAME_PUBLIC_HEADERS VIDEOCUT_FRAME_SOURCES)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
        forbidden_include_lines
        REGEX "${VIDEOCUT_FRAME_FORBIDDEN_INCLUDE_PATTERN}")
    if(forbidden_include_lines)
        message(FATAL_ERROR
            "VideoCutFrame forbidden dependency in ${source_file}: "
            "${forbidden_include_lines}")
    endif()
endforeach()

add_library(VideoCutFrame STATIC
    ${VIDEOCUT_FRAME_PUBLIC_HEADERS}
    ${VIDEOCUT_FRAME_SOURCES}
)
add_library(videocut::frame ALIAS VideoCutFrame)

set_target_properties(VideoCutFrame PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)

target_compile_definitions(VideoCutFrame PUBLIC VIDEOCUT_FRAME_STATIC=1)

target_include_directories(VideoCutFrame PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(VideoCutFrame PRIVATE /W4 /permissive-)
else()
    target_compile_options(VideoCutFrame PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
    )
endif()

get_directory_property(hasParent PARENT_DIRECTORY)
if(hasParent)
    set(VIDEOCUT_FRAME_LIBRARIES VideoCutFrame PARENT_SCOPE)
    set(VIDEOCUT_FRAME_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include" PARENT_SCOPE)
endif()
