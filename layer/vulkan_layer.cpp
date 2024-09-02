#include "layer/vulkan_layer.hpp"

static std::mutex context_mutex;
static std::unordered_map<void*, InstanceDispatchTable*> instance_dispatch_map;
static std::unordered_map<VkDevice, vkCollectorContext*> device_to_context_map;
static std::unordered_map<VkQueue, vkCollectorContext*> queue_to_context_map;

static bool gconfig_initialized = false;
static Json::Value glibcollector_config;

std::mutex vkCollectorContext::id_mutex;
uint32_t vkCollectorContext::next_id = 0;

template <typename dispatchable_type>
void *get_key(dispatchable_type inst) {
    return *reinterpret_cast<void **>(inst);
}

#define GET_PROC_ADDR(func)       \
    if (!strcmp(funcName, #func)) \
        return (PFN_vkVoidFunction)&lc_##func;

std::string get_config_path() {
    std::string config_path = "libcollector_config.json";

    #ifdef ANDROID
        // TODO(tomped01): Find a better way on android
        config_path = "/sdcard/Download/libcollector_config.json";
    #else
        const char* env_path = std::getenv("VK_LIBCOLLECTOR_CONFIG_PATH");
        if (env_path) {
            config_path = std::string(env_path);
        }
    #endif

    DBG_LOG("LIBCOLLECTOR LAYER: Reading config from file: %s \n", config_path.c_str());

    return config_path;
}

bool read_config() {
    context_mutex.lock();
    std::string config_json_raw;

    const std::string config_path = get_config_path();

    FILE* file_handle = fopen(config_path.c_str(), "r");
    if (file_handle) {
        fseek(file_handle, 0, SEEK_END);
        size_t len = ftell(file_handle);
        fseek(file_handle, 0, SEEK_SET);
        std::string contents(len + 1, '\0');
        size_t r = 0;
        int err = 0;
        do
        {
            r += fread(&contents[0], 1, len, file_handle);
            err = ferror(file_handle);
        } while (!feof(file_handle) && r != len && (err == EWOULDBLOCK || err == EINTR || err == EAGAIN));
        fclose(file_handle);
        config_json_raw = contents;
    } else {
        DBG_LOG("LIBCOLLECTOR LAYER ERROR: Failed to open libcollector JSON config file: %s \n", config_path.c_str());
        context_mutex.unlock();
        return false;
    }

    Json::Reader reader;

    if (!reader.parse(config_json_raw, glibcollector_config)) {
        DBG_LOG("LIBCOLLECTOR LAYER JSON parse error: %s, failed to enable libcollector instrumentation\n", reader.getFormattedErrorMessages().c_str());
        context_mutex.unlock();
        return false;
    } else {
        DBG_LOG("LIBCOLLECTOR LAYER: Successfully parsed libcollector JSON!\n");
    }

    if (glibcollector_config.get("result_file_basename", "").asString() == "") {
        #ifdef ANDROID
            glibcollector_config["result_file_basename"] = "/sdcard/Download/results.json";
        #else
            glibcollector_config["result_file_basename"] = "results.json";
        #endif
    }

    if (glibcollector_config.get("num_subframes_per_frame", 0).asInt() == 0) {
        glibcollector_config["num_subframes_per_frame"] = 1;
    }

    if (glibcollector_config.get("frame_trigger_function", "").asString() == "") {
        glibcollector_config["frame_trigger_function"] = "vkQueuePresentKHR";
    }

    context_mutex.unlock();
    return true;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL lc_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance
) {
    VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
    }

    if (layerCreateInfo == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

    VkResult ret = createFunc(pCreateInfo, pAllocator, pInstance);

    InstanceDispatchTable* new_table = new InstanceDispatchTable();
    new_table->gipa = (PFN_vkGetInstanceProcAddr)gipa(*pInstance, "vkGetInstanceProcAddr");
    new_table->nextDestroyInstance = (PFN_vkDestroyInstance)gipa(*pInstance, "vkDestroyInstance");
    new_table->nextEnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)gipa(*pInstance, "vkEnumerateDeviceExtensionProperties");

    context_mutex.lock();
    instance_dispatch_map[get_key(*pInstance)] = new_table;

    context_mutex.unlock();

    return ret;
}


VK_LAYER_EXPORT void VKAPI_CALL lc_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator
) {
    context_mutex.lock();
    InstanceDispatchTable* table = instance_dispatch_map[get_key(instance)];
    PFN_vkDestroyInstance nextDestroyInstance = table->nextDestroyInstance;
    instance_dispatch_map.erase(get_key(instance));
    context_mutex.unlock();

    delete table;

    nextDestroyInstance(instance, pAllocator);
}


VK_LAYER_EXPORT VkResult VKAPI_CALL lc_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice
) {
    VkLayerDeviceCreateInfo* layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;

    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
    }

    if (layerCreateInfo == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

    VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);

    vkCollectorContext* new_context = new vkCollectorContext();
    if (!new_context) {
        DBG_LOG("LIBCOLLECTOR ERROR: Failed to allocate vkCollectorContext\n");
    }

    new_context->gdpa = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");

    new_context->nextDestroyDevice = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
    new_context->nextQueueSubmit = (PFN_vkQueueSubmit)gdpa(*pDevice, "vkQueueSubmit");
    new_context->nextGetDeviceQueue = (PFN_vkGetDeviceQueue)gdpa(*pDevice, "vkGetDeviceQueue");
    new_context->nextQueuePresentKHR = (PFN_vkQueuePresentKHR)gdpa(*pDevice, "vkQueuePresentKHR");

    new_context->initialize_collectors();

    // Store context
    context_mutex.lock();
    device_to_context_map[*pDevice] = new_context;
    context_mutex.unlock();

    return ret;
}


VK_LAYER_EXPORT void VKAPI_CALL lc_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator
) {
    context_mutex.lock();
    vkCollectorContext* context = device_to_context_map[device];
    PFN_vkDestroyDevice nextDestroyDevice = context->nextDestroyDevice;
    device_to_context_map.erase(device);
    context_mutex.unlock();

    if (!context->postprocessed) {
        context->postprocess_results();
    }

    delete context;

    nextDestroyDevice(device, pAllocator);
}


VK_LAYER_EXPORT void VKAPI_CALL lc_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue
) {
    context_mutex.lock();
    vkCollectorContext* context = device_to_context_map[device];
    PFN_vkGetDeviceQueue nextGetDeviceQueue = context->nextGetDeviceQueue;
    context_mutex.unlock();

    nextGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);

    context_mutex.lock();
    queue_to_context_map[*pQueue] = context;
    context_mutex.unlock();
}


// Frame functions

VK_LAYER_EXPORT VkResult VKAPI_CALL lc_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence
) {
    context_mutex.lock();
    vkCollectorContext* context = queue_to_context_map[queue];
    context->process_frame();
    PFN_vkQueueSubmit nextQueueSubmit = context->nextQueueSubmit;
    context_mutex.unlock();

    return nextQueueSubmit(queue, submitCount, pSubmits, fence);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL lc_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo
) {
    context_mutex.lock();
    vkCollectorContext* context = queue_to_context_map[queue];
    context->process_frame();
    PFN_vkQueuePresentKHR nextQueuePresentKHR = context->nextQueuePresentKHR;
    context_mutex.unlock();

    return nextQueuePresentKHR(queue, pPresentInfo);
}


// Enum-instance functions

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount,
    VkLayerProperties* pProperties
) {
    if (pProperties == NULL) {
        *pPropertyCount = 1;
    } else {
        const char* layer_name = "VK_LAYER_ARM_libcollector";
        strncpy(pProperties[0].layerName, layer_name, strlen(layer_name) + 1);
        pProperties[0].specVersion = VK_MAKE_API_VERSION(0, 1, 1, 2);
        pProperties[0].implementationVersion = 2;
        const char* layer_description = "ARM libcollector layer implementation.";
        strncpy(pProperties[0].description, layer_description, strlen(layer_description) + 1);
    }

    return VK_SUCCESS;
}


VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice,
    uint32_t *pPropertyCount,
    VkLayerProperties *pProperties
) {
    return vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties
) {
    if (pLayerName == NULL) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    } else if (!strcmp(pLayerName, "VK_LAYER_ARM_libcollector")) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    return VK_ERROR_LAYER_NOT_PRESENT;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                         const char *pLayerName,
                                                                         uint32_t *pPropertyCount,
                                                                         VkExtensionProperties *pProperties)
{
    if (pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_ARM_libcollector")) {
        if (physicalDevice == VK_NULL_HANDLE)
        {
            return VK_SUCCESS;
        }

        return instance_dispatch_map[get_key(physicalDevice)]->nextEnumerateDeviceExtensionProperties(
            physicalDevice, pLayerName, pPropertyCount, pProperties);
    }

    if (pPropertyCount != nullptr) {
        *pPropertyCount = 0;
    }

    return VK_SUCCESS;
}


VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL lc_vkGetDeviceProcAddr(VkDevice device, const char *funcName) {
    GET_PROC_ADDR(vkGetDeviceProcAddr);
    GET_PROC_ADDR(vkDestroyDevice);
    GET_PROC_ADDR(vkGetDeviceQueue);

    if (glibcollector_config.get("frame_trigger_function", "").asString() == std::string("vkQueueSubmit")) {
        DBG_LOG("LIBCOLLECTOR LAYER: Sampling function set to vkQueueSubmit\n");
        GET_PROC_ADDR(vkQueueSubmit);
    } else {
        DBG_LOG("LIBCOLLECTOR LAYER: Sampling function set to vkQueuePresentKHR\n");
        GET_PROC_ADDR(vkQueuePresentKHR);
    }

    context_mutex.lock();
    vkCollectorContext* context = device_to_context_map[device];
    PFN_vkGetDeviceProcAddr gdpa;
    gdpa = context->gdpa;
    context_mutex.unlock();

    return gdpa(device, funcName);
}


VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *funcName) {
    return lc_vkGetDeviceProcAddr(device, funcName);
}


VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL lc_vkGetInstanceProcAddr(VkInstance instance,
                                                                       const char *funcName) {
    if (!gconfig_initialized) {
        // Do this as early as possible
        read_config();

        gconfig_initialized = true;
    }

    GET_PROC_ADDR(vkGetInstanceProcAddr);
    GET_PROC_ADDR(vkCreateInstance);
    GET_PROC_ADDR(vkCreateDevice);
    GET_PROC_ADDR(vkDestroyInstance);

    context_mutex.lock();
    InstanceDispatchTable* instance_dtable = instance_dispatch_map[get_key(instance)];
    PFN_vkGetInstanceProcAddr gipa = instance_dtable->gipa;
    context_mutex.unlock();

    return gipa(instance, funcName);
}


VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    return lc_vkGetInstanceProcAddr(instance, funcName);
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
    pVersionStruct->pfnGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)&lc_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)&lc_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    return VK_SUCCESS;
}


// Context

vkCollectorContext::vkCollectorContext():
    frames_processed(0),
    current_frame(0),
    current_subframe(0),
    num_subframes_per_frame(glibcollector_config.get("num_subframes_per_frame", 1).asInt()),
    start_frame(0),
    end_frame(99999),
    collectors_started(false),
    collectors_finished(false),
    postprocessed(false),
    init_failed(false),
    collectors(nullptr)
{
    start_frame = glibcollector_config.get("start_frame", 0).asInt();
    end_frame = glibcollector_config.get("end_frame", 99999).asInt();

    id_mutex.lock();
    id = next_id++;
    id_mutex.unlock();
}

vkCollectorContext::~vkCollectorContext() {
    postprocess_results();
    if (collectors) {
        delete collectors;
    }
}

void vkCollectorContext::initialize_collectors() {
    collectors = new Collection(glibcollector_config["collectors"]);
    collectors->initialize();
}

void vkCollectorContext::process_frame() {
    current_subframe +=1;

    if (current_subframe >= num_subframes_per_frame) {
        current_frame += 1;
        current_subframe = 0;

        if ((current_frame > start_frame) && (!collectors_started)) {
            run_start_wall_timer = std::chrono::high_resolution_clock::now();
            run_start_cpu_timer = std::clock();

            collectors->start();
            collectors_started = true;
        } else if ((current_frame > end_frame) && collectors_started) {
            collectors->stop();

            std::chrono::duration<double> diff = std::chrono::high_resolution_clock::now() - run_start_wall_timer;
            run_duration_wall_timer = diff.count();
            run_duration_cpu_timer = (std::clock() - run_start_cpu_timer) / static_cast<double>(CLOCKS_PER_SEC);

            collectors_finished = true;
        } else if (collectors_started && (!collectors_finished)) {
            collectors->collect();
            frames_processed += 1;
        }
    }
}

void vkCollectorContext::postprocess_results() {
    if (postprocessed) {
        return;
    }

    if (!collectors_started) {
        DBG_LOG("LIBCOLLECTOR LAYER ERROR: Attempted to postprocess results, but the collectors were never started.\n");
        return;
    }

    if (!collectors_finished) {
        collectors->stop();

        std::chrono::duration<double> diff = std::chrono::high_resolution_clock::now() - run_start_wall_timer;
        run_duration_wall_timer = diff.count();
        run_duration_cpu_timer = (std::clock() - run_start_cpu_timer) / static_cast<double>(CLOCKS_PER_SEC);

        collectors_finished = true;
    }

    Json::Value result_data_value;
    Json::Value libcollector_results = collectors->results();

    result_data_value["frames"] = static_cast<int>(frames_processed);
    result_data_value["cpu_runtime_seconds"] = static_cast<double>(run_duration_cpu_timer);
    result_data_value["fps"] = static_cast<double>(frames_processed) / run_duration_wall_timer;
    result_data_value["system_runtime_seconds"] = static_cast<double>(run_duration_wall_timer);

    if (libcollector_results.isMember("ferret")) {
        if (libcollector_results["ferret"].empty()) {
            DBG_LOG("LIBCOLLECTOR LAYER: Ferret data detected, but the results are empty.\n");
        } else {
            DBG_LOG("LIBCOLLECTOR LAYER: Ferret data detected, checking for postprocessed results...\n");
            // Calculate CPU FPS
            if (libcollector_results["ferret"].isMember("postprocessed_results")) {
                DBG_LOG("LIBCOLLECTOR LAYER: Postprocessed ferret data detected, calculating CPU FPS!\n");

                int main_thread = libcollector_results["ferret"]["postprocessed_results"]["main_thread_index"].asInt();
                double main_thread_megacycles = libcollector_results["ferret"]["postprocessed_results"]["main_thread_megacycles"].asInt();
                DBG_LOG("LIBCOLLECTOR LAYER: Main (heaviest) thread index set to: %d, main thread mega cycles consumed = %f\n", main_thread, main_thread_megacycles);

                result_data_value["main_thread_cpu_runtime@3GHz"] = main_thread_megacycles / 3000.0;

                double main_thread_cpu_runtime_3ghz = result_data_value["main_thread_cpu_runtime@3GHz"].asDouble();
                if (main_thread_cpu_runtime_3ghz == 0.0) {
                    DBG_LOG("LIBCOLLECTOR LAYER WARNING: Main thread CPU megacycles reported as 0. Possibly invalid ferret results.\n");
                    result_data_value["cpu_fps_main_thread@3GHz"] = 0.0;
                } else {
                    result_data_value["cpu_fps_main_thread@3GHz"] = static_cast<double>(frames_processed) / main_thread_cpu_runtime_3ghz;
                }

                double total_megacycles = libcollector_results["ferret"]["postprocessed_results"]["megacycles_sum"].asDouble();
                DBG_LOG("LIBCOLLECTOR LAYER: Total CPU mega cycles consumed = %f\n", total_megacycles);

                result_data_value["full_system_cpu_runtime@3GHz"] = total_megacycles / 3000.0;

                double full_system_cpu_runtime_3ghz = result_data_value["full_system_cpu_runtime@3GHz"].asDouble();
                if (full_system_cpu_runtime_3ghz == 0.0) {
                    DBG_LOG("LIBCOLLECTOR LAYER WARNING: Total CPU megacycles reported as 0. Possibly invalid ferret results.\n");
                    result_data_value["cpu_fps_full_system@3GHz"] = 0.0;
                } else {
                    result_data_value["cpu_fps_full_system@3GHz"] = static_cast<double>(frames_processed) / full_system_cpu_runtime_3ghz;
                }

                if (run_duration_wall_timer < 5.0) {
                    DBG_LOG("LIBCOLLECTOR LAYER WARNING: Runtime was less than 5 seconds, this can lead to inaccurate CPU load measurements (increasing runtime is highly recommended)\n");
                }

                DBG_LOG("CPU full system FPS@3GHz = %f, CPU main thread FPS@3GHz = %f\n", result_data_value["cpu_fps_full_system@3GHz"].asDouble(), result_data_value["cpu_fps_main_thread@3GHz"].asDouble());
            } else {
                DBG_LOG("LIBCOLLECTOR LAYER ERROR: Ferret data has no postprocessed results! Possibly wrong input parameters (check frequency list and cpu list in input json)\n");
            }
        }
    }

    result_data_value["frame_data"] = libcollector_results;

    Json::StyledWriter writer;
    std::string collector_data = writer.write(result_data_value);

    std::string outputfile = glibcollector_config["result_file_basename"].asString();
    std::string json_token = ".json";

    if (std::equal(json_token.rbegin(), json_token.rend(), outputfile.rbegin())) {
        outputfile = outputfile.replace(outputfile.find(json_token), outputfile.length(), "_device_" + _to_string(id) + json_token);
    } else {
        outputfile += "_device_" + _to_string(id) + json_token;
    }

    FILE* file_handle = fopen(outputfile.c_str(), "w");
    if (!file_handle) {
        DBG_LOG("LIBCOLLECTOR LAYER ERROR: Failed to open output result JSON %s: %s\n", outputfile.c_str(), strerror(errno));
        return;
    }

    size_t written;
    int err = 0;
    do {
        written = fwrite(collector_data.c_str(), collector_data.size(), 1, file_handle);
        err = ferror(file_handle);
    } while (!written && (err == EWOULDBLOCK || err == EINTR || err == EAGAIN));

    if (ferror(file_handle)) {
        DBG_LOG("LIBCOLLECTOR LAYER ERROR: Failed to write output result JSON: %s\n", strerror(errno));
    }

    fsync(fileno(file_handle));
    fclose(file_handle);

    DBG_LOG("LIBCOLLECTOR LAYER: Results writter to %s.\n", outputfile.c_str());

    postprocessed = true;
}
