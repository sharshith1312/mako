/**
 * @file common.h
 * @brief Common utilities and synchronization classes for Mako benchmarks
 * @author weihshen
 * @date 3/29/21
 */

#ifndef MAKO_BENCHMARKS_COMMON_H
#define MAKO_BENCHMARKS_COMMON_H

#include <iostream>
#include <fstream>
#include <thread>
#include <map>
#include <string>
#include <unordered_map>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Constants
namespace mako {
    namespace constants {
        constexpr int SYNC_POLL_DELAY_US = 0;  // Microseconds to sleep during polling
    }
}

/**
 * @brief Simple wrapper around std::map for string-to-int properties
 */
class HashWrapper {
public:
    std::map<std::string, int> data;
    
    void set_tprops(const std::string& k, int v) {
        data[k] = v;
    }

    int get_tprops(const std::string& k) const {
        auto it = data.find(k);
        if (it != data.end()) {
            return it->second;
        } else {
            return -1;
        }
    }
};

/**
 * @brief NFS-based synchronization utility
 * 
 * This implementation relies on NFS to sync files across nodes.
 * Typically runs on shard-0 in the leader datacenter.
 */
namespace mako {
    class NFSSync {
    public:
        static int set_key(const std::string& kk, const char* value, const char* host, int port) {
            std::string filename = std::string("nfs_sync_") + host + "_" + std::to_string(port) + "_" + kk;
            std::ofstream outfile(filename);
            if (!outfile) {
                std::cerr << "Failed to open file for writing: " << filename << std::endl;
                return 1;
            }
            outfile << value;
            return 0;
        }

        static void wait_for_key(const std::string& kk, const char* host, int port) {
            std::string filename = std::string("nfs_sync_") + host + "_" + std::to_string(port) + "_" + kk;
            while (true) {
                std::ifstream infile(filename);
                if (infile) {
                    break;
                }
                usleep(constants::SYNC_POLL_DELAY_US);
            }
        }

        static std::string get_key(const std::string& kk, const char* host, int port) {
            std::string filename = std::string("nfs_sync_") + host + "_" + std::to_string(port) + "_" + kk;
            std::ifstream infile(filename);
            return std::string((std::istreambuf_iterator<char>(infile)),
                            std::istreambuf_iterator<char>());
        }
    };
}


#endif // MAKO_BENCHMARKS_COMMON_H
