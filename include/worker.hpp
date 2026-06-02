#pragma once

#include <cstdint>
#include <optional>
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

inline std::optional<WorkerStatus> worker_status_from_string(const std::string& value) {
    if (value == "ONLINE") return WorkerStatus::Online;
    if (value == "OFFLINE") return WorkerStatus::Offline;
    return std::nullopt;
}

struct Worker {
    std::string id;
    WorkerStatus status = WorkerStatus::Online;
    std::int64_t registered_at_ms = 0;
    std::int64_t last_heartbeat_at_ms = 0;
};
