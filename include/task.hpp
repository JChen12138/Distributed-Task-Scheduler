#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class TaskStatus {
    Pending,
    Leased,
    Succeeded,
    FailedFinal,
    Cancelled
};

inline std::string to_string(TaskStatus status) {
    switch (status) {
        case TaskStatus::Pending: return "PENDING";
        case TaskStatus::Leased: return "LEASED";
        case TaskStatus::Succeeded: return "SUCCEEDED";
        case TaskStatus::FailedFinal: return "FAILED_FINAL";
        case TaskStatus::Cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

inline std::optional<TaskStatus> task_status_from_string(const std::string& value) {
    if (value == "PENDING") return TaskStatus::Pending;
    if (value == "LEASED") return TaskStatus::Leased;
    if (value == "SUCCEEDED") return TaskStatus::Succeeded;
    if (value == "FAILED_FINAL") return TaskStatus::FailedFinal;
    if (value == "CANCELLED") return TaskStatus::Cancelled;
    return std::nullopt;
}

struct Task {
    std::string id;
    std::string payload;
    TaskStatus status = TaskStatus::Pending;
    int attempt_count = 0;
    int max_attempts = 3;
    std::optional<std::string> assigned_worker_id;
    std::optional<std::int64_t> lease_expires_at_ms;
    std::int64_t lease_id = 0;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
    std::optional<std::int64_t> completed_at_ms;
    std::optional<std::string> last_error;
};
