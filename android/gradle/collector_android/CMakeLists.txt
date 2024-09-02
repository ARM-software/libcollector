cmake_minimum_required(VERSION 3.6.0)

set(PROJECT_DIR ${CMAKE_SOURCE_DIR}/../../../)

add_definitions(-DANDROID_STL="c++_shared")
add_definitions(-DANDROID_CPP_FEATURES="exceptions")

set(TARGET_INCLUDE_DIRS
    ${PROJECT_DIR}/collectors
    ${PROJECT_DIR}/external
    ${PROJECT_DIR}/external/jsoncpp/include
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/
    ${PROJECT_DIR}/external/jsoncpp/include/json
    ${PROJECT_DIR})

set(COLLECTOR_SOURCES
    ${PROJECT_DIR}/interface.cpp
    ${PROJECT_DIR}/collectors/collector_utility.cpp
    ${PROJECT_DIR}/collectors/cputemp.cpp
    ${PROJECT_DIR}/collectors/ferret.cpp
    ${PROJECT_DIR}/collectors/rusage.cpp
    ${PROJECT_DIR}/collectors/streamline.cpp
    ${PROJECT_DIR}/collectors/streamline_annotate.cpp
    ${PROJECT_DIR}/collectors/memory.cpp
    ${PROJECT_DIR}/collectors/cpufreq.cpp
    ${PROJECT_DIR}/collectors/gpufreq.cpp
    ${PROJECT_DIR}/collectors/perf.cpp
    ${PROJECT_DIR}/collectors/power.cpp
    ${PROJECT_DIR}/collectors/procfs_stat.cpp
    ${PROJECT_DIR}/collectors/hwcpipe.cpp
    ${PROJECT_DIR}/collectors/mali_counters.cpp
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/json_tool.h
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/json_reader.cpp
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/json_valueiterator.inl
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/json_value.cpp
    ${PROJECT_DIR}/external/jsoncpp/src/lib_json/json_writer.cpp)

set(BURROW_SOURCES ${PROJECT_DIR}/burrow.cpp)

include_directories(${TARGET_INCLUDE_DIRS})

add_library(app-glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

add_library(collector_android STATIC ${COLLECTOR_SOURCES})
target_link_libraries(collector_android app-glue)

find_library(log-lib log)
find_library(android-lib android)

add_executable(burrow ${BURROW_SOURCES})
target_link_libraries(burrow app-glue ${log-lib} ${android-lib} ${app-glue} collector_android)
target_link_options(burrow PRIVATE -Wl,-z,max-page-size=16384)
