project(collector)

cmake_minimum_required(VERSION 3.1)

set(SRC_ROOT ".")

add_definitions(-std=c++11 -Wall -fno-strict-aliasing -ggdb -Wno-deprecated-declarations)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
set(COLLECTOR_INCLUDES
    PUBLIC ${SRC_ROOT}
    PRIVATE ${SRC_ROOT}/collectors
    PUBLIC ${SRC_ROOT}/external
    PRIVATE ${SRC_ROOT}/external/jsoncpp/include
)
set(COLLECTOR_SRC
        ${SRC_ROOT}/interface.cpp
        ${SRC_ROOT}/collectors/collector_utility.cpp
        ${SRC_ROOT}/collectors/cputemp.cpp
        ${SRC_ROOT}/collectors/rusage.cpp
        ${SRC_ROOT}/collectors/streamline.cpp
        ${SRC_ROOT}/collectors/streamline_annotate.cpp
        ${SRC_ROOT}/collectors/memory.cpp
        ${SRC_ROOT}/collectors/perf.cpp
        ${SRC_ROOT}/collectors/gpufreq.cpp
        ${SRC_ROOT}/collectors/power.cpp
        ${SRC_ROOT}/collectors/procfs_stat.cpp
        ${SRC_ROOT}/collectors/cpufreq.cpp
        ${SRC_ROOT}/collectors/hwcpipe.cpp
        ${SRC_ROOT}/collectors/mali_counters.cpp
        ${SRC_ROOT}/collectors/ferret.cpp
        ${SRC_ROOT}/external/jsoncpp/src/lib_json/json_reader.cpp
        ${SRC_ROOT}/external/jsoncpp/src/lib_json/json_value.cpp
        ${SRC_ROOT}/external/jsoncpp/src/lib_json/json_writer.cpp
)

# --- library ---

add_library(collector SHARED ${COLLECTOR_SRC})
target_include_directories(collector ${COLLECTOR_INCLUDES})
target_link_libraries(collector rt)
set_target_properties(collector PROPERTIES LINK_FLAGS "-pthread" COMPILE_FLAGS "-pthread")

add_library(libcollector STATIC ${COLLECTOR_SRC})
target_include_directories(libcollector ${COLLECTOR_INCLUDES})
target_link_libraries(libcollector rt)
set_target_properties(libcollector PROPERTIES LINK_FLAGS "-pthread" COMPILE_FLAGS "-pthread")

# --- vulkan layer ---

add_library(VkLayer_libcollector SHARED ${COLLECTOR_SRC} ${SRC_ROOT}/layer/vulkan_layer.cpp)
target_include_directories(VkLayer_libcollector ${COLLECTOR_INCLUDES})
add_custom_command(TARGET VkLayer_libcollector POST_BUILD COMMAND mkdir -p implicit_layer.d)
add_custom_command(TARGET VkLayer_libcollector POST_BUILD COMMAND mv ${CMAKE_CURRENT_BINARY_DIR}/libVkLayer_libcollector.so implicit_layer.d/libVkLayer_libcollector.so)
add_custom_command(TARGET VkLayer_libcollector POST_BUILD COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/layer/VkLayer_libcollector.json implicit_layer.d/VkLayer_libcollector.json)

# --- test app ---

add_executable(test_app ${SRC_ROOT}/test.cpp)
target_link_libraries(test_app libcollector)
set_target_properties(test_app PROPERTIES LINK_FLAGS "-pthread" COMPILE_FLAGS "-pthread")
target_include_directories(test_app ${COLLECTOR_INCLUDES})

# --- burrow ---

add_executable(burrow ${SRC_ROOT}/burrow.cpp)
target_link_libraries(burrow libcollector)
set_target_properties(burrow PROPERTIES LINK_FLAGS "-pthread" COMPILE_FLAGS "-pthread")
target_include_directories(burrow ${COLLECTOR_INCLUDES})
