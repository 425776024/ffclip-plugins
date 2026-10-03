cmake_minimum_required(VERSION 3.12)
project(VideoCutTextComposition LANGUAGES CXX)

add_library(VideoCutTextComposition STATIC
    src/CanonicalTextCompositionCodec.cpp
    src/DecorationRenderPlan.cpp
    src/TextCompositionDocument.cpp
    src/TextCompositionSnapshot.cpp
    src/TextEditing.cpp
    src/TextInvalidation.cpp
    src/TextTemplatePatch.cpp)
add_library(videocut::text_composition ALIAS VideoCutTextComposition)

set_target_properties(VideoCutTextComposition PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(VideoCutTextComposition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(VideoCutTextComposition PUBLIC
    VideoCutText
    VideoCutVector)
target_link_libraries(VideoCutTextComposition PRIVATE
    VideoCutBase
    nlohmann_json::nlohmann_json)
