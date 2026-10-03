cmake_minimum_required(VERSION 3.12)
project(VideoCutText LANGUAGES CXX)

add_library(VideoCutText STATIC
    src/CanonicalTextPropertyCodec.cpp
    src/EffectRuntimeClock.cpp
    src/RichTextDocument.cpp
    src/QtTextSelectorRandom.cpp
    src/TextAnimation.cpp
    src/TextEffectEvaluator.cpp
    src/TextEffectEllipticLayout.cpp
    src/TextEffectProgramIR.cpp
    src/TextLayerAppearance.cpp
    src/TextProperty.cpp)
add_library(videocut::text ALIAS VideoCutText)

set_target_properties(VideoCutText PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(VideoCutText PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(VideoCutText PUBLIC VideoCutFrame)
target_link_libraries(VideoCutText PRIVATE
    VideoCutBase
    nlohmann_json::nlohmann_json)
