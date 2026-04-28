#pragma once

#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "task.hpp"

class TaskStore {
public:
    explicit TaskStore(std::string db_path);
    ~TaskStore();

    TaskStore(const TaskStore&) = delete;//a TaskStore object is not allowed to be copied.
    TaskStore& operator=(const TaskStore&) = delete;

    void initialize();
    Task create_task(const std::string& payload, int max_attempts);
    std::vector<Task> list_tasks() const;
    std::optional<Task> get_task(const std::string& task_id) const;

private:
    sqlite3* db_ = nullptr;
    std::string db_path_;

    void exec(const char* sql) const;
};
