cmake_minimum_required(VERSION 3.12)
project(VideoCutVector LANGUAGES CXX)

add_library(VideoCutVector STATIC
    src/Sha256.cpp
    src/VectorCustomization.cpp
    src/VectorDocument.cpp
    src/VectorTimeMapping.cpp)
add_library(videocut::vector ALIAS VideoCutVector)

set_target_properties(VideoCutVector PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(VideoCutVector PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(VideoCutVector PUBLIC VideoCutFrame)
target_link_libraries(VideoCutVector PRIVATE
    VideoCutBase
    nlohmann_json::nlohmann_json)
