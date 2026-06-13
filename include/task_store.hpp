#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "task.hpp"
#include "worker.hpp"

enum class WorkerPollStatus {
    WorkerNotFound,
    NoTaskAvailable,
    TaskLeased
};

enum class TaskReportStatus {
    TaskNotFound,
    WorkerNotFound,
    StaleLease,
    Completed,
    FailedRetryable,
    FailedFinal
};

struct WorkerPollResult {
    WorkerPollStatus status = WorkerPollStatus::NoTaskAvailable;
    std::optional<Task> task;
};

struct TaskReportResult {
    TaskReportStatus status = TaskReportStatus::TaskNotFound;
    std::optional<Task> task;
};

class TaskStore {
public:
    explicit TaskStore(std::string db_path);
    ~TaskStore();

    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;

    void initialize();
    Task create_task(const std::string& payload, int max_attempts);
    std::vector<Task> list_tasks() const;
    std::optional<Task> get_task(const std::string& task_id) const;
    Worker register_worker(const std::string& worker_id);
    std::optional<Worker> record_worker_heartbeat(const std::string& worker_id);
    std::vector<Worker> list_workers() const;
    WorkerPollResult poll_task_for_worker(const std::string& worker_id, std::int64_t lease_ms);
    TaskReportResult complete_task(const std::string& task_id,
                                   const std::string& worker_id,
                                   std::int64_t lease_id);
    TaskReportResult fail_task(const std::string& task_id,
                               const std::string& worker_id,
                               std::int64_t lease_id,
                               const std::string& error_message);

private:
    sqlite3* db_ = nullptr;
    std::string db_path_;
    mutable std::mutex mutex_;

    void exec(const char* sql) const;
};
