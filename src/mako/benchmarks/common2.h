/**
 * @file common2.h
 * @brief Additional common utilities for Mako benchmarks (dbtest specific)
 * @author weihshen
 * @date 3/29/21
 * 
 * This file contains utilities specifically for dbtest and TPC-C benchmarks.
 */

#ifndef MAKO_BENCHMARKS_COMMON2_H
#define MAKO_BENCHMARKS_COMMON2_H

#include "sto/ThreadPool.h"
#include "bench.h"
#include "benchmarks/sto/ReplayDB.h"
#include "benchmarks/sto/sync_util.hh"

#include <unistd.h>
#include <unordered_map>
#include <thread>
#include <vector>
#include <mutex>
#include <sstream>
#include <iterator>
#include <string>
#include <cstring>

// Constants
namespace mako {
    namespace constants {
        constexpr int NOOPS_COMMAND_LENGTH = 8;
        constexpr char NOOPS_PREFIX[] = "no-ops:";
        constexpr int NOOPS_PREFIX_LENGTH = 7;
    }
}

/**
 * @brief Split a string by whitespace
 * @param s Input string to split
 * @return Vector of whitespace-separated tokens
 */
static std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::copy(std::istream_iterator<std::string>(iss),
              std::istream_iterator<std::string>(),
              std::back_inserter(result));
    return result;
}

/**
 * @brief Check if log entry is a no-ops command with epoch number
 * @param log Log entry to check
 * @param len Length of log entry
 * @return Epoch number if it's a no-ops command (pattern: "no-ops:4"), -1 otherwise
 */
int isNoops(const char* log, int len) {
    if (len == mako::constants::NOOPS_COMMAND_LENGTH) {
        if (std::strncmp(log, mako::constants::NOOPS_PREFIX, mako::constants::NOOPS_PREFIX_LENGTH) == 0) {
            return log[mako::constants::NOOPS_PREFIX_LENGTH] - '0';
        }
    }
    return -1;
}

/**
 * @brief Start TPC-C worker threads
 * @param leader_config Configuration for leader or learner (new leader)
 * @param db Database instance
 * @param threads_nums Number of threads
 * @param skip_load Skip loading phase (for failover)
 * @param run Run mode: 0 = threads start, 1 = start run
 * @param rc Existing bench_runner to reuse
 * @return Pointer to bench_runner instance
 */
bench_runner* start_workers_tpcc(int leader_config,
                                 abstract_db* db,
                                 int threads_nums,
                                 bool skip_load = false,
                                 int run = 0,
                                 bench_runner* rc = nullptr) {
    const std::string bench_type = "tpcc";
    const std::string bench_opts = skip_load ? "--f_mode=1" : "--f_mode=0";

    std::vector<std::string> bench_toks = split_ws(bench_opts);
    int argc_bench = 1 + static_cast<int>(bench_toks.size());
    
    // Use vector instead of C-style array
    std::vector<char*> argv_bench;
    argv_bench.reserve(argc_bench);
    argv_bench.push_back(const_cast<char*>(bench_type.c_str()));
    
    for (const auto& token : bench_toks) {
        argv_bench.push_back(const_cast<char*>(token.c_str()));
    }
    
    bench_runner* result = tpcc_do_test(db, argc_bench, argv_bench.data(), run, rc);
    return result;
}

/**
 * @brief Monitor mode run implementation
 * @param db Database instance
 * @param thread_nums Number of threads
 * @param R Bench runner instance
 */
void modeMonitorRun(abstract_db* db, int thread_nums, bench_runner* R) {
    // Wait until mainPaxos sends data
    std::unique_lock<std::mutex> lk(sync_util::sync_logger::m);
    sync_util::sync_logger::cv.wait(lk, [] { 
        return sync_util::sync_logger::toLeader; 
    });

    Warning("start for modeMonitorRun, running:%d", sync_util::sync_logger::worker_running);   
    if (!sync_util::sync_logger::worker_running) {
        return;
    }
    
    // Start TPC-C workers in leader configuration with skip_load=true, run=1
    start_workers_tpcc(1, db, thread_nums, true, 1, R);
    
    if (BenchmarkConfig::getInstance().getIsReplicated()) {
        Warning("######--------------###### send endLlogs #####---------------######");
        const std::string endLogInd = "";
        for (int i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++) {
            add_log_to_nc(const_cast<char*>(endLogInd.c_str()), 0, i);
        }
    }

    lk.unlock();
    sync_util::sync_logger::cv.notify_one();
}

/**
 * @brief Start monitor mode in a separate thread
 * @param db Database instance
 * @param thread_nums Number of threads
 * @param R Bench runner instance
 */
void modeMonitor(abstract_db* db, int thread_nums, bench_runner* R) {
    Warning("start for modeMonitor, running:%d", sync_util::sync_logger::worker_running);
    std::thread mimic_thread(&modeMonitorRun, db, thread_nums, R);
    pthread_setname_np(mimic_thread.native_handle(), "modeMonitor");
    mimic_thread.detach();
}

// Global database wrapper instance (initialized once)
abstract_db* ThreadDBWrapperMbta::replay_thread_wrapper_db = new mbta_wrapper;

#endif // MAKO_BENCHMARKS_COMMON2_H