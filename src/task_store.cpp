#include "task_store.hpp"

#include <chrono>
#include <stdexcept>

namespace {
std::int64_t now_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string generate_task_id() {
    return "task-" + std::to_string(now_ms());
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

Task read_task(sqlite3_stmt* stmt) {
    Task task;
    task.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    task.payload = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const auto status = task_status_from_string(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
    if (!status.has_value()) {
        throw std::runtime_error("invalid task status stored in database");
    }
    task.status = *status;
    task.attempt_count = sqlite3_column_int(stmt, 3);
    task.max_attempts = sqlite3_column_int(stmt, 4);

    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
        task.assigned_worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    }
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
        task.lease_expires_at_ms = sqlite3_column_int64(stmt, 6);
    }

    task.created_at_ms = sqlite3_column_int64(stmt, 7);
    task.updated_at_ms = sqlite3_column_int64(stmt, 8);

    if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
        task.completed_at_ms = sqlite3_column_int64(stmt, 9);
    }
    return task;
}
}

TaskStore::TaskStore(std::string db_path): db_path_(std::move(db_path)) {
    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("failed to open SQLite database");
    }
}

TaskStore::~TaskStore() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void TaskStore::initialize() {
    exec(R"SQL(
        PRAGMA journal_mode = WAL;
    )SQL");

    exec(R"SQL(
        CREATE TABLE IF NOT EXISTS tasks(
            id TEXT PRIMARY KEY,
            payload TEXT NOT NULL,
            status TEXT NOT NULL,
            attempt_count INTEGER NOT NULL,
            max_attempts INTEGER NOT NULL,
            assigned_worker_id TEXT,
            lease_expires_at_ms INTEGER,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL,
            completed_at_ms INTEGER
        );
    )SQL");
}

Task TaskStore::create_task(const std::string& payload, int max_attempts) {
    const auto timestamp = now_ms();
    Task task;
    task.id = generate_task_id();
    task.payload = payload;
    task.status = TaskStatus::Pending;
    task.max_attempts = max_attempts;
    task.created_at_ms = timestamp;
    task.updated_at_ms = timestamp;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        INSERT INTO tasks(
            id, payload, status, attempt_count, max_attempts,
            assigned_worker_id, lease_expires_at_ms, created_at_ms,
            updated_at_ms, completed_at_ms
        )
        VALUES (?, ?, ?, ?, ?, NULL, NULL, ?, ?, NULL);
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare task insert");
    }

    bind_text(stmt, 1, task.id);
    bind_text(stmt, 2, task.payload);
    bind_text(stmt, 3, to_string(task.status));
    sqlite3_bind_int(stmt, 4, task.attempt_count);
    sqlite3_bind_int(stmt, 5, task.max_attempts);
    sqlite3_bind_int64(stmt, 6, task.created_at_ms);
    sqlite3_bind_int64(stmt, 7, task.updated_at_ms);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("failed to insert task");
    }
    sqlite3_finalize(stmt);
    return task;
}

std::vector<Task> TaskStore::list_tasks() const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT id, payload, status, attempt_count, max_attempts,
               assigned_worker_id, lease_expires_at_ms, created_at_ms,
               updated_at_ms, completed_at_ms
        FROM tasks
        ORDER BY created_at_ms ASC;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare task list");
    }

    std::vector<Task> tasks;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        tasks.push_back(read_task(stmt));
    }
    sqlite3_finalize(stmt);
    return tasks;
}

std::optional<Task> TaskStore::get_task(const std::string& task_id) const {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT id, payload, status, attempt_count, max_attempts,
               assigned_worker_id, lease_expires_at_ms, created_at_ms,
               updated_at_ms, completed_at_ms
        FROM tasks
        WHERE id = ?;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare task lookup");
    }

    bind_text(stmt, 1, task_id);
    std::optional<Task> task;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        task = read_task(stmt);
    }
    sqlite3_finalize(stmt);
    return task;
}

void TaskStore::exec(const char* sql) const {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? "unknown SQLite error" : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}
