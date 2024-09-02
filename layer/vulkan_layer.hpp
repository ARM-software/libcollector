#pragma once
#ifndef VULKAN_LAYER_HPP
#define VULKAN_LAYER_HPP

#include <mutex>
#include <string>
#include <ctime>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <unordered_map>
#include "json/value.h"
#include "json/reader.h"
#include "json/writer.h"

#include "vulkan/vk_layer.h"
#include "interface.hpp"
#include "collectors/collector_utility.hpp"

// VK_LAYER_EXPORT got removed from vulkan/vk_layer.h
#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT
#endif

struct InstanceDispatchTable;
struct vkCollectorContext;

struct InstanceDispatchTable {
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance nextDestroyInstance;
    PFN_vkEnumerateDeviceExtensionProperties nextEnumerateDeviceExtensionProperties;
};

struct vkCollectorContext {
    vkCollectorContext();
    ~vkCollectorContext();

    static std::mutex id_mutex;
    static uint32_t next_id;
    uint32_t id;

    void parse_config(const std::string& config_path);
    void initialize_collectors();
    void process_frame();
    void postprocess_results();

    uint32_t frames_processed;
    uint32_t current_frame;
    uint32_t current_subframe;
    uint32_t num_subframes_per_frame;

    uint32_t start_frame;
    uint32_t end_frame;

    bool collectors_started;
    bool collectors_finished;
    bool postprocessed;
    bool init_failed;

    Collection* collectors;

    std::chrono::time_point<std::chrono::high_resolution_clock> run_start_wall_timer;
    double run_duration_wall_timer;

    std::clock_t run_start_cpu_timer;
    double run_duration_cpu_timer;

    // Dispatch table
    PFN_vkGetDeviceProcAddr gdpa;

    PFN_vkDestroyDevice nextDestroyDevice;
    PFN_vkGetDeviceQueue nextGetDeviceQueue;
    PFN_vkQueueSubmit nextQueueSubmit;
    PFN_vkQueuePresentKHR nextQueuePresentKHR;
};

std::string get_config_path();
bool read_config();

#endif
