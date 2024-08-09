#pragma once

#include "collector_utility.hpp"
#include "interface.hpp"
#include <map>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

enum hw_cnt_length
{
    b32 = 0,
    b64,
};

enum cmn_node_type
{
    CMN_TYPE_INVALID = 0,
    CMN_TYPE_DVM,
    CMN_TYPE_CFG,
    CMN_TYPE_DTC,
    CMN_TYPE_HNI,
    CMN_TYPE_HNF,
    CMN_TYPE_XP,
    CMN_TYPE_SBSX,
    CMN_TYPE_MPAM_S,
    CMN_TYPE_MPAM_NS,
    CMN_TYPE_RNI = 0xa,
    CMN_TYPE_RND = 0xd,
    CMN_TYPE_RNSAM = 0xf,
    CMN_TYPE_MTSX = 0x10,
    CMN_TYPE_CXRA = 0x100,
    CMN_TYPE_CXHA = 0x101,
    CMN_TYPE_CXLA = 0x102,
    /* Not a real node type */
    CMN_TYPE_WP = 0x7770,
};

enum collect_scope_flags: int32_t
{
    COLLECT_NOOP = 0x00,
    COLLECT_ALL_THREADS = 0x01,
    COLLECT_REPLAY_THREADS = 0x01 << 1,
    COLLECT_BG_THREADS = 0x01 << 2,
    COLLECT_MULTI_PMU_THREADS = 0x01 << 3,
    COLLECT_BOOKER_THREADS = 0x01 << 4,
    COLLECT_CSPMU_THREADS = 0x01 << 5,
};

struct snapshot {
    snapshot() : size(0) {}

    uint64_t size;
    long long values[8] = {0};
};

struct event {
    std::string name;
    uint32_t type;
    uint64_t config;
    bool exc_user;    // default is false
    bool exc_kernel;  // default is false
    enum hw_cnt_length len; // default is 32bit pmu counter
    bool booker_ci;  // default is false
    bool cspmu;
    std::string device; // default is ""
    uint32_t inherited; // default is 1
};

class event_context
{
public:
    event_context()
    {
        group = -1;
        last_snap_func_id = -1;
    }

    ~event_context() {}

    bool init(std::vector<struct event> &events, int tid, int cpu);
    bool start();
    struct snapshot collect(int64_t now);

    struct snapshot collect_scope(int64_t now, uint16_t func_id, bool stopping);

    // If not -1, then we are in the middle of collect_scope_start/stop.
    uint16_t last_snap_func_id;
    struct snapshot last_snap;

    bool stop();
    bool deinit();

    inline void update_data(struct snapshot &snap, CollectorValueResults &result)
    {
        for (unsigned int i = 0; i < mCounters.size(); i++)
            result[mCounters[i].name].push_back(snap.values[i]);
    }

    inline void update_data_scope(uint16_t func_id, bool is_calling, struct snapshot &snap_start, struct snapshot &snap_end, CollectorValueResults &result)
    {
        if (!mValueResults) mValueResults = &result;
        long long diff_acc = 0;
        for (unsigned int i = 0; i < mCounters.size(); i++) {
            long long diff = snap_end.values[i] - snap_start.values[i];
            if (mCounters[i].scope_values.size() <= func_id) {
                mCounters[i].scope_values.resize(std::min(func_id * 2 + 1, UINT16_MAX - 1), 0);
            }
            mCounters[i].scope_values[func_id] += diff;
            diff_acc += diff;
        }
        if (diff_acc > 0 && is_calling) {
            if (scope_num_calls.size() <= func_id) {
                scope_num_calls.resize(std::min(func_id * 2 + 1, UINT16_MAX - 1), 0);
            }
            scope_num_calls[func_id]++;
        }
        if (diff_acc > 0) {
            if (scope_num_with_perf.size() <= func_id) {
                scope_num_with_perf.resize(std::min(func_id * 2 + 1, UINT16_MAX - 1), 0);
            }
            scope_num_with_perf[func_id]++;
        }
    }

private:
    struct counter
    {
        std::string name;
        int fd;
        // Record accumulated values for update_data_scope, where the index of the vector is the uint16_t func_id.
        std::vector<long long> scope_values;

        counter() {
            scope_values.reserve(512);
            scope_values.resize(512, 0);
        }
    };

    int group;
    std::vector<struct counter> mCounters;
    // Record number of scope calls with perf counter incremental greater than 0 (can happen in multiple bg threads)
    std::vector<int32_t> scope_num_with_perf;
    // Record number of scope calls that actually triggered the collect_scope (happen in 1 thread that calls the collection method)
    std::vector<int32_t> scope_num_calls;
    CollectorValueResults *mValueResults = nullptr;
};

class PerfCollector : public Collector
{
public:
    PerfCollector(const Json::Value& config, const std::string& name);

    virtual bool init() override;
    virtual bool deinit() override;
    virtual bool start() override;
    virtual bool stop() override;
    virtual bool collect(int64_t) override;
    virtual bool available() override;

    virtual void clear() override;
    virtual bool postprocess(const std::vector<int64_t>& timing) override;
    virtual void summarize() override;

    /// Collector functions for perapi perf instrumentations.
    virtual bool collect_scope_start(int64_t now, uint16_t func_id, int32_t flags);
    virtual bool collect_scope_stop(int64_t now, uint16_t func_id, int32_t flags);
    bool perf_counter_pause();
    bool perf_counter_resume();

  private:
    void create_perf_thread();
    void saveResultsFile();

private:
    int mSet = -1;
    int mInherit = 1;
    bool mAllThread = true;
    std::vector<struct event> mEvents;
    std::vector<struct event> mBookerEvents;
    std::map<int, std::vector<struct event>> mMultiPMUEvents;
    std::map<int, std::vector<struct event>> mCSPMUEvents;
    std::map<std::string, std::vector<struct timespec>> mClocks; // device_name -> clock_vector
    int last_collect_scope_flags = 0;

    uint64_t PMCNTENSET_EL0_safe = 0;
    uint64_t PMCR_EL0_safe = 0;

    struct perf_thread
    {
        perf_thread(const int tid, const std::string &name): tid(tid), name(name), eventCtx{} {}

        void update_data(struct snapshot& snap)
        {
            eventCtx.update_data(snap, mResultsPerThread);
        }

        void update_data_scope(uint16_t func_id, struct snapshot& snap_start, struct snapshot& snap_end)
        {
            pid_t cur_tid = syscall(SYS_gettid);
            eventCtx.update_data_scope(func_id, cur_tid == tid, snap_start, snap_end, mResultsPerThread);
        }

        void clear()
        {
            for (auto& pair : mResultsPerThread)
                pair.second.clear();
        }

        void postprocess(Json::Value& value)
        {
            Json::Value v;
            for (const auto& pair : mResultsPerThread)
            {
                bool need_sum = false;
                if (value.isMember(pair.first)) need_sum = true;
                v[pair.first] = Json::arrayValue;

                unsigned int index = 0;
                int64_t total = 0;
                for (const CollectorValue& cv : pair.second.data())
                {
                    int64_t s = cv.i64;
                    if (need_sum) s += value[pair.first][index++].asInt64();
                    v[pair.first].append((Json::Value::Int64)s);
                    total += s;
                }
                value[pair.first] = v[pair.first];
                value["SUM"][pair.first] = (Json::Value::Int64)total;
            }
        }

        void summarize()
        {
            for (auto& pair : mResultsPerThread)
            {
                pair.second.summarize();
            }
        }

        const int tid;
        const std::string name;
        event_context eventCtx;
        CollectorValueResults mResultsPerThread;
        std::string device_name;
    };

    std::vector<struct perf_thread> mReplayThreads;
    std::vector<struct perf_thread> mBgThreads;
    std::vector<struct perf_thread> mBookerThread;
    std::vector<struct perf_thread> mMultiPMUThreads;
    std::vector<struct perf_thread> mCSPMUThreads;
};
