cmake_minimum_required(VERSION 3.10)
project(bdreader-ncnn-upscaler VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)

find_package(Vulkan REQUIRED)
find_package(WebP REQUIRED)

# Build the bundled ncnn if no system provider is available
set(NCNN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ncnn")
set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "Disable ncnn benchmarks" FORCE)
set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "Disable ncnn examples" FORCE)
set(NCNN_BUILD_TOOLS OFF CACHE BOOL "Disable ncnn tools" FORCE)
set(NCNN_BUILD_TESTS OFF CACHE BOOL "Disable ncnn tests" FORCE)
set(NCNN_VULKAN ON CACHE BOOL "Enable Vulkan" FORCE)
add_subdirectory(${NCNN_DIR} ${CMAKE_BINARY_DIR}/ncnn)
target_compile_definitions(ncnn PRIVATE NCNN_DISABLE_GPU_INFO_LOG=1)

add_library(cxxopts INTERFACE)
target_include_directories(cxxopts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

file(GLOB_RECURSE BDREADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
)

add_executable(bdreader-ncnn-upscaler ${BDREADER_SOURCES})

if(ipo_supported)
    set_property(TARGET bdreader-ncnn-upscaler PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

target_include_directories(bdreader-ncnn-upscaler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${NCNN_DIR}/include
    ${NCNN_DIR}/src
)

target_link_libraries(bdreader-ncnn-upscaler
    PRIVATE
        ncnn
        Vulkan::Vulkan
        WebP::webp
        cxxopts
)

install(TARGETS bdreader-ncnn-upscaler RUNTIME DESTINATION bin)
