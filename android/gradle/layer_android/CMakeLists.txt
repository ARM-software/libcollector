cmake_minimum_required(VERSION 3.6.0)
project(layer_android)

set(PROJECT_DIR ${CMAKE_SOURCE_DIR}/../../../)

add_definitions(-DANDROID_STL="c++_shared")
add_definitions(-DANDROID_CPP_FEATURES="exceptions")

set(LAYER_SOURCES
    ${PROJECT_DIR}/interface.cpp
    ${PROJECT_DIR}/collectors/collector_utility.cpp
    ${PROJECT_DIR}/collectors/cputemp.cpp
    ${PROJECT_DIR}/collectors/rusage.cpp
    ${PROJECT_DIR}/collectors/streamline.cpp
    ${PROJECT_DIR}/collectors/streamline_annotate.cpp
    ${PROJECT_DIR}/collectors/memory.cpp
    ${PROJECT_DIR}/collectors/perf.cpp
    ${PROJECT_DIR}/collectors/gpufreq.cpp
    ${PROJECT_DIR}/collectors/power.cpp
    ${PROJECT_DIR}/collectors/procfs_stat.cpp
    ${PROJECT_DIR}/collectors/cpufreq.cpp
    ${PROJECT_DIR}/collectors/hwcpipe.cpp
    ${PROJECT_DIR}/collectors/mali_counters.cpp
    ${PROJECT_DIR}/collectors/ferret.cpp
    ${PROJECT_DIR}/layer/vulkan_layer.cpp
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/json_reader.cpp
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/json_value.cpp
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/json_writer.cpp)


set(TARGET_INCLUDE_DIRS
    PUBLIC  ${PROJECT_DIR}
    PRIVATE ${PROJECT_DIR}/collectors
    PRIVATE ${PROJECT_DIR}/external/jsoncpp/include
    PRIVATE ${PROJECT_DIR}/external/Vulkan-Headers/include
    PRIVATE ${PROJECT_DIR}/layer)

include_directories(${TARGET_INCLUDE_DIRS})

find_library(log-lib log)
find_library(android-lib android)

add_library(VkLayer_libcollector SHARED ${LAYER_SOURCES})
target_link_libraries(VkLayer_libcollector ${log-lib} ${android-lib})
