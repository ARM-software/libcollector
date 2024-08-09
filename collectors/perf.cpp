#include "perf.hpp"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#if !defined(ANDROID)
#include <linux/perf_event.h>
#else
#include "perf_event.h"
#endif
#include <asm/unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include <sstream>
#include <fstream>

static std::map<int, std::vector<struct event>> EVENTS = {
{0, { {"CPUInstructionRetired", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, false, false, hw_cnt_length::b32, false},
      {"CPUCacheReferences", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES, false, false, hw_cnt_length::b32, false},
      {"CPUCacheMisses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, false, false, hw_cnt_length::b32, false},
      {"CPUBranchMispredictions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, false, false, hw_cnt_length::b32, false}
    }
},
{1, { {"CPUInstructionRetired", PERF_TYPE_RAW, 0x8, false, false, hw_cnt_length::b32, false},
      {"CPUL1CacheAccesses", PERF_TYPE_RAW, 0x4, false, false, hw_cnt_length::b32, false},
      {"CPUL2CacheAccesses", PERF_TYPE_RAW, 0x16, false, false, hw_cnt_length::b32, false},
      {"CPULASESpec", PERF_TYPE_RAW, 0x74, false, false, hw_cnt_length::b32, false},
      {"CPUVFPSpec", PERF_TYPE_RAW, 0x75, false, false, hw_cnt_length::b32, false},
      {"CPUCryptoSpec", PERF_TYPE_RAW, 0x77, false, false, hw_cnt_length::b32, false},
    }
},
{2, { {"CPUL3CacheAccesses", PERF_TYPE_RAW, 0x2b, false, false, hw_cnt_length::b32, false},
      {"CPUBusAccessRead", PERF_TYPE_RAW, 0x60, false, false, hw_cnt_length::b32, false},
      {"CPUBusAccessWrite", PERF_TYPE_RAW, 0x61, false, false, hw_cnt_length::b32, false},
      {"CPUMemoryAccessRead", PERF_TYPE_RAW, 0x66, false, false, hw_cnt_length::b32, false},
      {"CPUMemoryAccessWrite", PERF_TYPE_RAW, 0x67, false, false, hw_cnt_length::b32, false},
    }
},
{3, { {"CPUBusAccesses", PERF_TYPE_RAW, 0x19, false, false, hw_cnt_length::b32, false},
      {"CPUL2CacheRead", PERF_TYPE_RAW, 0x50, false, false, hw_cnt_length::b32, false},
      {"CPUL2CacheWrite", PERF_TYPE_RAW, 0x51, false, false, hw_cnt_length::b32, false},
      {"CPUMemoryAccessRead", PERF_TYPE_RAW, 0x66, false, false, hw_cnt_length::b32, false},
      {"CPUMemoryAccessWrite", PERF_TYPE_RAW, 0x67, false, false, hw_cnt_length::b32, false},
    }
}
};

std::map<std::string, int> NodeTypes = {
{"DVM", CMN_TYPE_DVM},
{"CFG", CMN_TYPE_CFG},
{"DTC", CMN_TYPE_DTC},
{"HNI", CMN_TYPE_HNI},
{"HNF", CMN_TYPE_HNF},
{"XP" , CMN_TYPE_XP },
{"SBSX", CMN_TYPE_SBSX},
{"MPAM_S", CMN_TYPE_MPAM_S},
{"MPAM_NS", CMN_TYPE_MPAM_NS},
{"RNI", CMN_TYPE_RNI},
{"RND", CMN_TYPE_RND},
{"RNSAM", CMN_TYPE_RNSAM},
{"MTSX", CMN_TYPE_MTSX},
{"CXRA", CMN_TYPE_CXRA},
{"CXHA", CMN_TYPE_CXHA},
{"CXLA", CMN_TYPE_CXLA},
{" ", CMN_TYPE_INVALID},
};

static inline uint64_t makeup_booker_ci_config(int nodetype, int eventid, int bynodeid = 0, uint64_t nodeid = 0)
{
    uint64_t config = 0;
    //bitfields in attr.config. nodeid: GENMASK(47, 32)).     bynodeid: BIT(31)).      eventId: GENMASK(23, 16)).      nodetype: GENMASK(15, 0)
    config = ((nodeid << 32) & 0x0000FFFF00000000) | ((bynodeid << 31) & 0x80000000) | ((eventid << 16) & 0x00FF0000) | (nodetype & 0x0000FFFF);

    return config;
}

PerfCollector::PerfCollector(const Json::Value& config, const std::string& name) : Collector(config, name)
{
    struct event leader = {"CPUCycleCount", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES};

    mSet = mConfig.get("set", -1).asInt();
    mInherit = mConfig.get("inherit", 1).asInt();

    leader.inherited = mInherit;
    mEvents.push_back(leader);

    if ((0 <= mSet) && (mSet <= 3))
    {
        DBG_LOG("Using reserved CPU counter set number %d, this will fail on non-ARM CPU's except set 0.\n", mSet);
        for (const struct event& e : EVENTS[mSet])
            mEvents.push_back(e);
    }
    else if (mConfig.isMember("event"))
    {
        DBG_LOG("Using customized CPU counter event, this will fail on non-ARM CPU's\n");
        Json::Value eventArray = mConfig["event"];
        for (Json::ArrayIndex i = 0; i < eventArray.size(); i++)
        {
            Json::Value item = eventArray[i];
            struct event e;

            if ( !item.isMember("name") || (!item.isMember("type")&&!item.isMember("device")) || !item.isMember("config") )
            {
                DBG_LOG("perf event does not specify name, tpye or config, skip this event!\n");
                continue;
            }
            e.name = item.get("name", "").asString();
            e.type = item.get("type", 0).asInt();
            e.exc_user = item.get("excludeUser", false).asBool();
            e.exc_kernel = item.get("excludeKernel", false).asBool();
            e.len = (item.get("counterLen64bit", 0).asInt() == 0) ? hw_cnt_length::b32 : hw_cnt_length::b64;
            e.booker_ci = item.get("booker-ci", 0).asInt();
            e.cspmu = item.get("CSPMU", 0).asInt();
            e.device = item.get("device", "").asString();
            e.inherited = mInherit;

            if (e.booker_ci)
            {   // booker-ci counter
                int eventid = item.get("config", 0).asInt();
                std::string type = item.get("nodetype", " ").asString();
                int nodetype = NodeTypes[type];
                int bynodeid = item.get("bynodeid", 0).asInt();

                if (bynodeid)
                {
                    Json::Value nodeIdArray = item["nodeid"];
                    for (Json::ArrayIndex idx = 0; idx < nodeIdArray.size(); idx++)
                    {
                        struct event nodeEvent = e;
                        uint64_t nodeid = nodeIdArray[idx].asUInt64();
                        nodeEvent.config = makeup_booker_ci_config(nodetype, eventid, 1, nodeid);
                        nodeEvent.name = item.get("name", "").asString() + "_node" + _to_string(nodeid);
                        mBookerEvents.push_back(nodeEvent);
                    }
                }
                else
                {
                    e.config = makeup_booker_ci_config(nodetype, eventid);
                    mBookerEvents.push_back(e);
                }
            }
            else if(e.device!="")
            {//for d9000, CPU cores on different PMU
                e.config = item.get("config", 0).asUInt64();
                auto type_string = e.device;

                auto event_type_filename = "/sys/devices/" + type_string + "/type";

                std::ifstream event_type(event_type_filename);
                if (getline(event_type, type_string))
                {
                    DBG_LOG("Read event type %s from %s\n", type_string.c_str(), event_type_filename.c_str());
                    e.type=atoi(type_string.c_str());
                    if (e.cspmu)
                    {
                        e.name = e.device+"_"+e.name;
                        if (mCSPMUEvents.count(e.type))
                        {
                            mCSPMUEvents[e.type].push_back(e);
                        }
                        else
                        {
                            mCSPMUEvents.emplace(e.type,std::vector<event>{e});
                        }
                    }
                    else
                    {
                        if (mMultiPMUEvents.count(e.type))
                        {
                            mMultiPMUEvents[e.type].push_back(e);
                        }
                        else
                        {
                            mMultiPMUEvents.emplace(e.type,std::vector<event>{e});
                        }
                    }
                }
                else
                {
                    DBG_LOG("Error: wrong device name, could not find correspoding event type, event skipped\n");
                }
            }
            else
            {
                e.config = item.get("config", 0).asUInt64();
                mEvents.push_back(e);
            }
        }
    }

    mAllThread = mConfig.get("allthread", true).asBool();
}

static inline long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags)
{
    hw_event->size = sizeof(*hw_event);
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

bool PerfCollector::available()
{
    return true;
}

static int add_event(const struct event &e, int tid, int cpu, int group = -1)
{
    struct perf_event_attr pe = {0};

    pe.type = e.type;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = e.config;
    pe.config1 = (e.len == hw_cnt_length::b32) ? 0 : 1;
    pe.disabled = 1;
    pe.inherit = e.inherited;
    pe.exclude_user = e.exc_user;
    pe.exclude_kernel = e.exc_kernel;
    pe.exclude_hv = 0;
    pe.read_format = PERF_FORMAT_GROUP;

    const int fd = perf_event_open(&pe, tid, cpu, group, 0);
    if (fd < 0)
    {
        DBG_LOG("Error opening perf: error %d\n", errno);
        perror("syscall");
        return -1;
    }
    return fd;
}

bool PerfCollector::init()
{
    if (mEvents.size() == 0)
    {
        DBG_LOG("None perf event counter.\n");
        return false;
    }

    create_perf_thread();
    int i=0;
    for (perf_thread& t : mReplayThreads)
    {
        t.eventCtx.init(mEvents, t.tid, -1);
        for (auto& et : mMultiPMUEvents)
        {
            mMultiPMUThreads[i].eventCtx.init(et.second, mMultiPMUThreads[i].tid, -1);
            i++;
        }
    }

    for (perf_thread& t : mBgThreads)
    {
        t.eventCtx.init(mEvents, t.tid, -1);
        for (auto& et : mMultiPMUEvents)
        {
            mMultiPMUThreads[i].eventCtx.init(et.second, mMultiPMUThreads[i].tid, -1);
            i++;
        }
    }

    int n = 0;
    for (auto& iter : mCSPMUEvents)
    {
        mCSPMUThreads[n].eventCtx.init(iter.second, -1, 0);
        n++;
    }

    for (perf_thread& t : mBookerThread)
        t.eventCtx.init(mBookerEvents, -1, 0);

    return true;
}

bool PerfCollector::deinit()
{
    for (perf_thread& t : mReplayThreads)
    {
        t.eventCtx.deinit();
        t.clear();
    }

    for (perf_thread& t : mBgThreads)
    {
        t.eventCtx.deinit();
        t.clear();
    }

    for (perf_thread& t : mMultiPMUThreads)
    {
        t.eventCtx.deinit();
        t.clear();
    }

    for (perf_thread& t : mBookerThread)
    {
        t.eventCtx.deinit();
        t.clear();
    }

    for (perf_thread& t : mCSPMUThreads)
    {
        t.eventCtx.deinit();
        t.clear();
    }

    mEvents.clear();
    mBookerEvents.clear();
    for (auto& et : mMultiPMUEvents) et.second.clear();
    for (auto& et : mCSPMUEvents) et.second.clear();

    mReplayThreads.clear();
    mBgThreads.clear();
    mMultiPMUThreads.clear();
    mBookerThread.clear();
    mCSPMUThreads.clear();
    mClocks.clear();

    clear();

    return true;
}

bool PerfCollector::start()
{
    if (mCollecting)
        return true;

    for (perf_thread& t : mReplayThreads)
        if (!t.eventCtx.start())
            return false;

    for (perf_thread& t : mBgThreads)
        if (!t.eventCtx.start())
            return false;

    for (perf_thread& t : mMultiPMUThreads)
        if (!t.eventCtx.start())
            return false;

    for (perf_thread& t: mBookerThread)
        if (!t.eventCtx.start())
            return false;

    for (perf_thread& t: mCSPMUThreads)
        {
            if (!t.eventCtx.start())
                return false;
            mClocks.emplace(t.device_name, std::vector<timespec>{});
        }
    mCollecting = true;
    return true;
}

void PerfCollector::clear()
{
    if (mCollecting)
        return;

    Collector::clear();
}

bool PerfCollector::stop()
{
    if (!mCollecting)
        return true;

    DBG_LOG("Stopping perf collection.\n");

    for (perf_thread& t : mReplayThreads)
    {
       t.eventCtx.stop();
    }

    for (perf_thread& t : mBgThreads)
    {
       t.eventCtx.stop();
    }

    for (perf_thread& t : mMultiPMUThreads)
    {
       t.eventCtx.stop();
    }

    for (perf_thread& t : mBookerThread)
    {
        t.eventCtx.stop();
    }

    for (perf_thread& t : mCSPMUThreads)
    {
        t.eventCtx.stop();
    }

    mCollecting = false;

    return true;
}

bool PerfCollector::collect(int64_t now)
{
    if (!mCollecting)
        return false;
    struct snapshot snap;
    for (perf_thread& t : mReplayThreads)
    {
        snap = t.eventCtx.collect(now);
        t.update_data(snap);
    }

    for (perf_thread& t : mBgThreads)
    {
        snap = t.eventCtx.collect(now);
        t.update_data(snap);
    }

    for (perf_thread& t : mMultiPMUThreads)
    {
        snap = t.eventCtx.collect(now);
        t.update_data(snap);
    }

    for (perf_thread& t : mBookerThread)
    {
        snap = t.eventCtx.collect(now);
        t.update_data(snap);
    }

    for (perf_thread& t : mCSPMUThreads)
    {
        snap = t.eventCtx.collect(now);
        t.update_data(snap);
        // save CLOCK_MONOTONIC_RAW when collect
        struct timespec tmp_clock = {0, 0};
        clock_gettime(CLOCK_MONOTONIC_RAW, &tmp_clock);
        mClocks[t.device_name].push_back(tmp_clock);
    }

    return true;
}

bool PerfCollector::perf_counter_pause() {
#if defined(__aarch64__)
    asm volatile("mrs %0, PMCNTENSET_EL0" : "=r" (PMCNTENSET_EL0_safe));
    // stop counters for arm64
    asm volatile("mrs %0, PMCR_EL0" : "=r" (PMCR_EL0_safe));
    asm volatile("msr PMCR_EL0, %0" : : "r" (PMCR_EL0_safe & 0xFFFFFFFFFFFFFFFE));
#elif defined(__arm__)
    asm volatile("mrc p15, 0, %0, c9, c12, 1" : "=r"(PMCNTENSET_EL0_safe));
    // stop counters for arm32
    asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(PMCR_EL0_safe));
    asm volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(PMCR_EL0_safe & 0xFFFFFFFE));
#endif
    return true;
}

bool PerfCollector::perf_counter_resume() {
#if defined(__aarch64__)
    // start counters for arm64
    asm volatile("msr PMCNTENSET_EL0, %0" : : "r" (PMCNTENSET_EL0_safe));
    asm volatile("msr PMCR_EL0, %0" : : "r" (PMCR_EL0_safe));
#elif defined(__arm__)
    // start counters for arm32
    asm volatile("mcr p15, 0, %0, c9, c12, 1" : : "r"(PMCNTENSET_EL0_safe));
    asm volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(PMCR_EL0_safe));
#endif
    return true;
}


bool PerfCollector::collect_scope_start(int64_t now, uint16_t func_id, int32_t flags) {
    if (!perf_counter_pause()) return false;
    if (!mCollecting) return false;
    struct snapshot snap;
    if (flags & COLLECT_REPLAY_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mReplayThreads)
        {
            t.eventCtx.collect_scope(now, func_id, false);
        }
    }
    if (flags & COLLECT_BG_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mBgThreads)
        {
            t.eventCtx.collect_scope(now, func_id, false);
        }
    }
    if (flags & COLLECT_MULTI_PMU_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mMultiPMUThreads)
        {
            t.eventCtx.collect_scope(now, func_id, false);
        }
    }
    if (flags & COLLECT_BOOKER_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mBookerThread)
        {
            t.eventCtx.collect_scope(now, func_id, false);
        }
    }
    if (flags & COLLECT_CSPMU_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mCSPMUThreads)
        {
            t.eventCtx.collect_scope(now, func_id, false);
        }
    }
    last_collect_scope_flags = flags;
    if (!perf_counter_resume()) return false;
    return true;
}

bool PerfCollector::collect_scope_stop(int64_t now, uint16_t func_id, int32_t flags) {
    if (!perf_counter_pause()) return false;
    if (!mCollecting) return false;
    if (last_collect_scope_flags != flags) {
        DBG_LOG("Error: Could not find the corresponding collect_scope_start call for func_id %ud.\n", func_id);
        return false;
    }
    struct snapshot snap_start, snap_stop;
    if (flags & COLLECT_REPLAY_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mReplayThreads)
        {
            snap_start = t.eventCtx.last_snap;
            snap_stop = t.eventCtx.collect_scope(now, func_id, true);
            t.update_data_scope(func_id, snap_start, snap_stop);
        }
    }
    if (flags & COLLECT_BG_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mBgThreads)
        {
            snap_start = t.eventCtx.last_snap;
            snap_stop = t.eventCtx.collect_scope(now, func_id, true);
            t.update_data_scope(func_id, snap_start, snap_stop);
        }
    }
    if (flags & COLLECT_MULTI_PMU_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mMultiPMUThreads)
        {
            snap_start = t.eventCtx.last_snap;
            snap_stop = t.eventCtx.collect_scope(now, func_id, true);
            t.update_data_scope(func_id, snap_start, snap_stop);
        }
    }
    if (flags & COLLECT_BOOKER_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mBookerThread)
        {
            snap_start = t.eventCtx.last_snap;
            snap_stop = t.eventCtx.collect_scope(now, func_id, true);
            t.update_data_scope(func_id, snap_start, snap_stop);
        }
    }
    if (flags & COLLECT_CSPMU_THREADS || flags & COLLECT_ALL_THREADS)
    {
        for (perf_thread &t : mCSPMUThreads)
        {
            snap_start = t.eventCtx.last_snap;
            snap_stop = t.eventCtx.collect_scope(now, func_id, true);
            t.update_data_scope(func_id, snap_start, snap_stop);
        }
    }
    if (!perf_counter_resume()) return false;
    return false;
}

bool PerfCollector::postprocess(const std::vector<int64_t>& timing)
{
    Json::Value v;

    if (isSummarized()) mCustomResult["summarized"] = true;
    mCustomResult["thread_data"] = Json::arrayValue;

    Json::Value replayValue;
    replayValue["CCthread"] = "replayMainThreads";
    int i=0;
    for (perf_thread& t : mReplayThreads)
    {
        Json::Value perf_threadValue;
        perf_threadValue["CCthread"] = t.name.c_str();
        t.postprocess(perf_threadValue);
        t.postprocess(replayValue);
        for (unsigned int j =0; j<mMultiPMUEvents.size();j++)
        {
            mMultiPMUThreads[i].postprocess(replayValue);
            i++;
        }
        mCustomResult["thread_data"].append(perf_threadValue);
    }
    for (perf_thread& t : mCSPMUThreads)
    {
        Json::Value perf_threadValue;
        perf_threadValue["CCthread"] = t.name.c_str();
        t.postprocess(perf_threadValue);
        t.postprocess(replayValue);
        std::string sec_name = t.device_name + "_sec";
        std::string nsec_name = t.device_name + "_nsec";
        std::vector<int64_t> clocks_sec;
        std::vector<int64_t> clocks_nsec;
        Json::Value clockValue;
        for (auto iter : mClocks[t.device_name])
        {
            clocks_sec.push_back(iter.tv_sec);
            clocks_nsec.push_back(iter.tv_nsec);
            clockValue[sec_name.c_str()].append((Json::Value::Int64)iter.tv_sec);
            clockValue[nsec_name.c_str()].append((Json::Value::Int64)iter.tv_nsec);
        }
        mCustomResult["thread_data"].append(perf_threadValue);
        mCustomResult["thread_data"].append(clockValue);
    }
    for (perf_thread& t : mBookerThread)
    {
        Json::Value perf_threadValue;
        perf_threadValue["CCthread"] = t.name.c_str();
        t.postprocess(perf_threadValue);
        t.postprocess(replayValue);
        mCustomResult["thread_data"].append(perf_threadValue);
    }
    mCustomResult["thread_data"].append(replayValue);

    if (mAllThread)
    {
        Json::Value bgValue;
        bgValue["CCthread"] = "backgroundThreads";

        Json::Value allValue(replayValue);
        allValue["CCthread"] = "allThreads";

        for (perf_thread& t : mBgThreads)
        {
            Json::Value perf_threadValue;
            perf_threadValue["CCthread"] = t.name.c_str();
            t.postprocess(perf_threadValue);
            t.postprocess(bgValue);
            t.postprocess(allValue);
            for (unsigned int j =0; j<mMultiPMUEvents.size();j++)
            {
                mMultiPMUThreads[i].postprocess(bgValue);
                mMultiPMUThreads[i].postprocess(allValue);
                i++;
            }
            mCustomResult["thread_data"].append(perf_threadValue);
        }

        mCustomResult["thread_data"].append(bgValue);
        mCustomResult["thread_data"].append(allValue);
    }

    return true;
}

void PerfCollector::summarize()
{
    mIsSummarized = true;

    for (perf_thread& t : mReplayThreads)
    {
        t.summarize();
    }

    for (perf_thread& t : mBgThreads)
    {
        t.summarize();
    }

    for (perf_thread& t : mMultiPMUThreads)
    {
        t.summarize();
    }

    for (perf_thread& t : mBookerThread)
    {
        t.summarize();
    }

    for (perf_thread& t : mCSPMUThreads)
    {
        t.summarize();
    }
}

bool event_context::init(std::vector<struct event> &events, int tid, int cpu)
{
    struct counter grp;
    grp.fd = group = add_event(events[0], tid, cpu);
    grp.name = events[0].name;
    mCounters.push_back(grp);

    for (size_t i=1; i<events.size(); i++)
    {
        struct counter c;
        c.fd = add_event(events[i], tid, cpu, group);
        c.name = events[i].name;
        mCounters.push_back(c);
    }

    ioctl(group, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(group, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

    for (const struct counter& c : mCounters) if (c.fd == -1) { DBG_LOG("libcollector perf: Failed to init counter %s\n", c.name.c_str()); return false; }
    return true;
}

bool event_context::deinit()
{
    for (const struct counter& c : mCounters)
        if (c.fd != -1)
            close(c.fd);

    mCounters.clear();
    return true;
}

bool event_context::start()
{
    if (ioctl(group, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) == -1)
    {
        perror("ioctl PERF_EVENT_IOC_RESET");
        return false;
    }
    if (ioctl(group, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) == -1)
    {
        perror("ioctl PERF_EVENT_IOC_ENABLE");
        return false;
    }
    return true;
}

bool event_context::stop()
{
    if (ioctl(group, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) == -1)
    {
        perror("ioctl PERF_EVENT_IOC_DISABLE");
        return false;
    }

    for (struct counter& c : mCounters)
    {
        if (c.scope_values.size() > 0 && mValueResults != nullptr)
        {
            std::string name = c.name + ":ScopeSum";
            for (unsigned int i = 0; i < c.scope_values.size(); i++)
            {
                (*mValueResults)[name].push_back(c.scope_values[i]);
            }
        }
    }

    std::string name_num_func_calls = "CCthread:ScopeNumCalls";
    for (unsigned int i = 0; i < scope_num_calls.size(); i++)
    {
        (*mValueResults)[name_num_func_calls].push_back(scope_num_calls[i]);
    }

    std::string name_num_calls = "CCthread:ScopeNumWithPerf";
    for (unsigned int i = 0; i < scope_num_with_perf.size(); i++)
    {
        (*mValueResults)[name_num_calls].push_back(scope_num_with_perf[i]);
    }

    return true;
}

// Collect and reset the perf counters to 0.
struct snapshot event_context::collect(int64_t now)
{
    struct snapshot snap;

    if (read(group, &snap, sizeof(snap)) == -1) perror("read");
    if (ioctl(group, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) == -1) perror("ioctl PERF_EVENT_IOC_RESET");
    return snap;
}

struct snapshot event_context::collect_scope(int64_t now, uint16_t func_id, bool stopping)
{
    if (stopping && last_snap_func_id != func_id) {
        DBG_LOG("Error: Could not find the corresponding collect_scope_start call for func_id %ud.\n", func_id);
    }
    struct snapshot snap;
    if (read(group, &snap, sizeof(snap)) == -1) perror("read");
    if (stopping) {
        last_snap_func_id = -1;
    } else {
        last_snap_func_id = func_id;
        last_snap = snap;
    }
    return snap;
}

static std::string getThreadName(int tid)
{
    std::stringstream comm_path;
    if (tid == 0)
        comm_path << "/proc/self/comm";
    else
        comm_path << "/proc/self/task/" << tid << "/comm";

    std::string name;
    std::ifstream comm_file { comm_path.str() };
    if (!comm_file.is_open())
    {
        DBG_LOG("Fail to open comm file for thread %d.\n", tid);
    }
    comm_file >> name;
    return name;
}

void PerfCollector::create_perf_thread()
{
    std::string current_pName = getThreadName(0);

    if(!mCSPMUEvents.empty())
    {
        int i=0;
        for (auto pair : mCSPMUEvents)
        {
            mCSPMUThreads.emplace_back(getpid(), current_pName);
            mCSPMUThreads[i].device_name = pair.second[0].device;
            i++;
        }
        return;
    }

    DIR *dirp = NULL;
    if ((dirp = opendir("/proc/self/task")) == NULL)
        return;

    struct dirent *ent = NULL;
    while ((ent = readdir(dirp)) != NULL)
    {
        if (isdigit(ent->d_name[0]))
        {
            int tid =_stol(std::string(ent->d_name));
            std::string thread_name = getThreadName(tid);
            if (!strncmp(thread_name.c_str(), "patrace-", 8))
            {
                mReplayThreads.emplace_back(tid, thread_name);
                //each group of MultiPMUEvents have a thread
                for (unsigned int i =0; i<mMultiPMUEvents.size();i++) mMultiPMUThreads.emplace_back(tid, thread_name);
            }
            if (mAllThread && !strncmp(thread_name.c_str(), "mali-", 5))
            {
                mBgThreads.emplace_back(tid, thread_name);
                for (unsigned int i =0; i<mMultiPMUEvents.size();i++) mMultiPMUThreads.emplace_back(tid, thread_name);
            }
            if (mAllThread && !strncmp(thread_name.c_str(), "ANGLE-", 6))
            {
                mBgThreads.emplace_back(tid, thread_name);
                for (unsigned int i =0; i<mMultiPMUEvents.size();i++) mMultiPMUThreads.emplace_back(tid, thread_name);
            }
        }
    }
    closedir(dirp);

    if (mBookerEvents.size() > 0)
    {
        mBookerThread.emplace_back(getpid(), current_pName);
    }
}

static void writeCSV(int tid, std::string name, const CollectorValueResults &results)
{
    std::stringstream ss;
#ifdef ANDROID
    ss << "/sdcard/" << name.c_str() << tid << ".csv";
#else
    ss << name.c_str() << tid << ".csv";
#endif
    std::string filename = ss.str();
    DBG_LOG("writing perf result to %s\n", filename.c_str());

    FILE *fp = fopen(filename.c_str(), "w");
    if (fp)
    {
        unsigned int number = 0;
        std::string item;
        for (const auto& pair : results)
        {
            item += pair.first + ",";
            number = pair.second.size();
        }
        fprintf(fp, "%s\n", item.c_str());

        for (unsigned int i=0; i<number; i++)
        {
            std::stringstream css;
            std::string value;
            for (const auto& pair : results)
            {
                css << pair.second.at(i).i64 << ",";
            }
            value = css.str();
            fprintf(fp, "%s\n", value.c_str());
        }

        fsync(fileno(fp));
        fclose(fp);
    }
    else DBG_LOG("Fail to open %s\n", filename.c_str());
}

void PerfCollector::saveResultsFile()
{
    for (const perf_thread& t : mReplayThreads)
        writeCSV(t.tid, t.name, t.mResultsPerThread);

    for (const perf_thread& t : mBgThreads)
        writeCSV(t.tid, t.name, t.mResultsPerThread);
}
