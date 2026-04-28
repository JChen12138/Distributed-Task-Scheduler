#pragma once

#include <cstdint>
#include <string>

enum class WorkerStatus {
    Online,
    Offline
};

inline std::string to_string(WorkerStatus status) {
    switch (status) {
        case WorkerStatus::Online: return "ONLINE";
        case WorkerStatus::Offline: return "OFFLINE";
    }
    return "UNKNOWN";
}

struct Worker {
    std::string id;
    WorkerStatus status = WorkerStatus::Online;
    std::int64_t registered_at_ms = 0;
    std::int64_t last_heartbeat_at_ms = 0;
};
