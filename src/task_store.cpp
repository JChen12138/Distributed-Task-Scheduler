#include "task_store.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>

namespace {
std::atomic<std::uint64_t> next_task_sequence{0};

std::int64_t now_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string generate_task_id() {
    const auto sequence = next_task_sequence.fetch_add(1, std::memory_order_relaxed);
    return "task-" + std::to_string(now_ms()) + "-" + std::to_string(sequence);
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

bool table_has_column(sqlite3* db, const std::string& table_name, const std::string& column_name) {
    sqlite3_stmt* stmt = nullptr;
    const std::string sql = "PRAGMA table_info(" + table_name + ");";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to inspect table schema");
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* raw_name = sqlite3_column_text(stmt, 1);
        if (raw_name != nullptr && column_name == reinterpret_cast<const char*>(raw_name)) {
            found = true;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return found;
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

    task.lease_id = sqlite3_column_int64(stmt, 7);
    task.created_at_ms = sqlite3_column_int64(stmt, 8);
    task.updated_at_ms = sqlite3_column_int64(stmt, 9);

    if (sqlite3_column_type(stmt, 10) != SQLITE_NULL) {
        task.completed_at_ms = sqlite3_column_int64(stmt, 10);
    }
    if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
        task.last_error = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    }
    return task;
}

Worker read_worker(sqlite3_stmt* stmt) {
    Worker worker;
    worker.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const auto status = worker_status_from_string(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    if (!status.has_value()) {
        throw std::runtime_error("invalid worker status stored in database");
    }
    worker.status = *status;
    worker.registered_at_ms = sqlite3_column_int64(stmt, 2);
    worker.last_heartbeat_at_ms = sqlite3_column_int64(stmt, 3);
    return worker;
}

void rollback_if_open(sqlite3* db, bool& transaction_open) {
    if (!transaction_open) {
        return;
    }
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    transaction_open = false;
}

bool worker_exists(sqlite3* db, const std::string& worker_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT id
        FROM workers
        WHERE id = ?;
    )SQL";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare worker existence lookup");
    }

    bind_text(stmt, 1, worker_id);
    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

std::optional<Task> load_task_by_id(sqlite3* db, const std::string& task_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT id, payload, status, attempt_count, max_attempts,
               assigned_worker_id, lease_expires_at_ms, lease_id, created_at_ms,
               updated_at_ms, completed_at_ms, last_error
        FROM tasks
        WHERE id = ?;
    )SQL";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
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

bool is_current_lease(const Task& task,
                      const std::string& worker_id,
                      std::int64_t lease_id,
                      std::int64_t now) {
    return task.status == TaskStatus::Leased &&
           task.assigned_worker_id.has_value() &&
           *task.assigned_worker_id == worker_id &&
           task.lease_expires_at_ms.has_value() &&
           *task.lease_expires_at_ms >= now &&
           task.lease_id == lease_id;
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
    std::lock_guard<std::mutex> lock(mutex_);

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
            lease_id INTEGER NOT NULL DEFAULT 0,
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL,
            completed_at_ms INTEGER,
            last_error TEXT
        );
    )SQL");

    if (!table_has_column(db_, "tasks", "lease_id")) {
        exec("ALTER TABLE tasks ADD COLUMN lease_id INTEGER NOT NULL DEFAULT 0;");
    }
    if (!table_has_column(db_, "tasks", "last_error")) {
        exec("ALTER TABLE tasks ADD COLUMN last_error TEXT;");
    }

    exec(R"SQL(
        CREATE TABLE IF NOT EXISTS workers(
            id TEXT PRIMARY KEY,
            status TEXT NOT NULL,
            registered_at_ms INTEGER NOT NULL,
            last_heartbeat_at_ms INTEGER NOT NULL
        );
    )SQL");
}

Task TaskStore::create_task(const std::string& payload, int max_attempts) {
    std::lock_guard<std::mutex> lock(mutex_);

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
            assigned_worker_id, lease_expires_at_ms, lease_id, created_at_ms,
            updated_at_ms, completed_at_ms, last_error
        )
        VALUES (?, ?, ?, ?, ?, NULL, NULL, ?, ?, ?, NULL, NULL);
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare task insert");
    }

    bind_text(stmt, 1, task.id);
    bind_text(stmt, 2, task.payload);
    bind_text(stmt, 3, to_string(task.status));
    sqlite3_bind_int(stmt, 4, task.attempt_count);
    sqlite3_bind_int(stmt, 5, task.max_attempts);
    sqlite3_bind_int64(stmt, 6, task.lease_id);
    sqlite3_bind_int64(stmt, 7, task.created_at_ms);
    sqlite3_bind_int64(stmt, 8, task.updated_at_ms);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("failed to insert task");
    }
    sqlite3_finalize(stmt);
    return task;
}

std::vector<Task> TaskStore::list_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT id, payload, status, attempt_count, max_attempts,
               assigned_worker_id, lease_expires_at_ms, lease_id, created_at_ms,
               updated_at_ms, completed_at_ms, last_error
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
    std::lock_guard<std::mutex> lock(mutex_);
    return load_task_by_id(db_, task_id);
}

Worker TaskStore::register_worker(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto timestamp = now_ms();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        INSERT INTO workers(id, status, registered_at_ms, last_heartbeat_at_ms)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
            status = excluded.status,
            last_heartbeat_at_ms = excluded.last_heartbeat_at_ms;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare worker registration");
    }

    bind_text(stmt, 1, worker_id);
    bind_text(stmt, 2, to_string(WorkerStatus::Online));
    sqlite3_bind_int64(stmt, 3, timestamp);
    sqlite3_bind_int64(stmt, 4, timestamp);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("failed to register worker");
    }
    sqlite3_finalize(stmt);

    Worker worker;
    worker.id = worker_id;
    worker.status = WorkerStatus::Online;
    worker.last_heartbeat_at_ms = timestamp;

    sqlite3_stmt* lookup_stmt = nullptr;
    const char* lookup_sql = R"SQL(
        SELECT registered_at_ms
        FROM workers
        WHERE id = ?;
    )SQL";

    if (sqlite3_prepare_v2(db_, lookup_sql, -1, &lookup_stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare worker registration lookup");
    }

    bind_text(lookup_stmt, 1, worker_id);
    if (sqlite3_step(lookup_stmt) == SQLITE_ROW) {
        worker.registered_at_ms = sqlite3_column_int64(lookup_stmt, 0);
    } else {
        sqlite3_finalize(lookup_stmt);
        throw std::runtime_error("registered worker could not be loaded");
    }
    sqlite3_finalize(lookup_stmt);
    return worker;
}

std::optional<Worker> TaskStore::record_worker_heartbeat(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto timestamp = now_ms();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        UPDATE workers
        SET status = ?, last_heartbeat_at_ms = ?
        WHERE id = ?;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare worker heartbeat");
    }

    bind_text(stmt, 1, to_string(WorkerStatus::Online));
    sqlite3_bind_int64(stmt, 2, timestamp);
    bind_text(stmt, 3, worker_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("failed to record worker heartbeat");
    }
    sqlite3_finalize(stmt);

    if (sqlite3_changes(db_) == 0) {
        return std::nullopt;
    }

    sqlite3_stmt* lookup_stmt = nullptr;
    const char* lookup_sql = R"SQL(
        SELECT id, status, registered_at_ms, last_heartbeat_at_ms
        FROM workers
        WHERE id = ?;
    )SQL";

    if (sqlite3_prepare_v2(db_, lookup_sql, -1, &lookup_stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare worker heartbeat lookup");
    }

    bind_text(lookup_stmt, 1, worker_id);
    std::optional<Worker> worker;
    if (sqlite3_step(lookup_stmt) == SQLITE_ROW) {
        worker = read_worker(lookup_stmt);
    }
    sqlite3_finalize(lookup_stmt);
    return worker;
}

std::vector<Worker> TaskStore::list_workers() const {
    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"SQL(
        SELECT id, status, registered_at_ms, last_heartbeat_at_ms
        FROM workers
        ORDER BY registered_at_ms ASC;
    )SQL";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("failed to prepare worker list");
    }

    std::vector<Worker> workers;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        workers.push_back(read_worker(stmt));
    }
    sqlite3_finalize(stmt);
    return workers;
}

WorkerPollResult TaskStore::poll_task_for_worker(const std::string& worker_id, std::int64_t lease_ms) {
    if (lease_ms <= 0) {
        throw std::invalid_argument("lease_ms must be greater than zero");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto timestamp = now_ms();
    const auto lease_expires_at_ms = timestamp + lease_ms;
    bool transaction_open = false;

    try {
        exec("BEGIN IMMEDIATE;");
        transaction_open = true;

        sqlite3_stmt* worker_stmt = nullptr;
        const char* worker_sql = R"SQL(
            SELECT id
            FROM workers
            WHERE id = ?;
        )SQL";

        if (sqlite3_prepare_v2(db_, worker_sql, -1, &worker_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("failed to prepare worker lookup for polling");
        }

        bind_text(worker_stmt, 1, worker_id);
        const bool worker_exists = sqlite3_step(worker_stmt) == SQLITE_ROW;
        sqlite3_finalize(worker_stmt);

        if (!worker_exists) {
            rollback_if_open(db_, transaction_open);
            return {WorkerPollStatus::WorkerNotFound, std::nullopt};
        }

        sqlite3_stmt* heartbeat_stmt = nullptr;
        const char* heartbeat_sql = R"SQL(
            UPDATE workers
            SET status = ?, last_heartbeat_at_ms = ?
            WHERE id = ?;
        )SQL";

        if (sqlite3_prepare_v2(db_, heartbeat_sql, -1, &heartbeat_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("failed to prepare worker heartbeat during polling");
        }

        bind_text(heartbeat_stmt, 1, to_string(WorkerStatus::Online));
        sqlite3_bind_int64(heartbeat_stmt, 2, timestamp);
        bind_text(heartbeat_stmt, 3, worker_id);

        if (sqlite3_step(heartbeat_stmt) != SQLITE_DONE) {
            sqlite3_finalize(heartbeat_stmt);
            throw std::runtime_error("failed to update worker heartbeat during polling");
        }
        sqlite3_finalize(heartbeat_stmt);

        sqlite3_stmt* candidate_stmt = nullptr;
        const char* candidate_sql = R"SQL(
            SELECT id
            FROM tasks
            WHERE status = ? AND attempt_count < max_attempts
            ORDER BY created_at_ms ASC
            LIMIT 1;
        )SQL";

        if (sqlite3_prepare_v2(db_, candidate_sql, -1, &candidate_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("failed to prepare task lease candidate lookup");
        }

        bind_text(candidate_stmt, 1, to_string(TaskStatus::Pending));
        std::optional<std::string> task_id;
        if (sqlite3_step(candidate_stmt) == SQLITE_ROW) {
            task_id = reinterpret_cast<const char*>(sqlite3_column_text(candidate_stmt, 0));
        }
        sqlite3_finalize(candidate_stmt);

        if (!task_id.has_value()) {
            exec("COMMIT;");
            transaction_open = false;
            return {WorkerPollStatus::NoTaskAvailable, std::nullopt};
        }

        sqlite3_stmt* lease_stmt = nullptr;
        const char* lease_sql = R"SQL(
            UPDATE tasks
            SET status = ?,
                attempt_count = attempt_count + 1,
                assigned_worker_id = ?,
                lease_expires_at_ms = ?,
                lease_id = lease_id + 1,
                updated_at_ms = ?
            WHERE id = ? AND status = ? AND attempt_count < max_attempts;
        )SQL";

        if (sqlite3_prepare_v2(db_, lease_sql, -1, &lease_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("failed to prepare task lease update");
        }

        bind_text(lease_stmt, 1, to_string(TaskStatus::Leased));
        bind_text(lease_stmt, 2, worker_id);
        sqlite3_bind_int64(lease_stmt, 3, lease_expires_at_ms);
        sqlite3_bind_int64(lease_stmt, 4, timestamp);
        bind_text(lease_stmt, 5, *task_id);
        bind_text(lease_stmt, 6, to_string(TaskStatus::Pending));

        if (sqlite3_step(lease_stmt) != SQLITE_DONE) {
            sqlite3_finalize(lease_stmt);
            throw std::runtime_error("failed to lease task");
        }
        sqlite3_finalize(lease_stmt);

        if (sqlite3_changes(db_) != 1) {
            throw std::runtime_error("task lease update did not modify exactly one row");
        }

        sqlite3_stmt* task_stmt = nullptr;
        const char* task_sql = R"SQL(
            SELECT id, payload, status, attempt_count, max_attempts,
                   assigned_worker_id, lease_expires_at_ms, lease_id, created_at_ms,
                   updated_at_ms, completed_at_ms, last_error
            FROM tasks
            WHERE id = ?;
        )SQL";

        if (sqlite3_prepare_v2(db_, task_sql, -1, &task_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("failed to prepare leased task lookup");
        }

        bind_text(task_stmt, 1, *task_id);
        std::optional<Task> task;
        if (sqlite3_step(task_stmt) == SQLITE_ROW) {
            task = read_task(task_stmt);
        }
        sqlite3_finalize(task_stmt);

        if (!task.has_value()) {
            throw std::runtime_error("leased task could not be loaded");
        }

        exec("COMMIT;");
        transaction_open = false;
        return {WorkerPollStatus::TaskLeased, task};
    } catch (...) {
        rollback_if_open(db_, transaction_open);
        throw;
    }
}

TaskReportResult TaskStore::complete_task(const std::string& task_id,
                                          const std::string& worker_id,
                                          std::int64_t lease_id) {
    if (lease_id <= 0) {
        throw std::invalid_argument("lease_id must be greater than zero");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto timestamp = now_ms();
    bool transaction_open = false;

    try {
        exec("BEGIN IMMEDIATE;");
        transaction_open = true;

        if (!worker_exists(db_, worker_id)) {
            rollback_if_open(db_, transaction_open);
            return {TaskReportStatus::WorkerNotFound, std::nullopt};
        }

        const auto task = load_task_by_id(db_, task_id);
        if (!task.has_value()) {
            rollback_if_open(db_, transaction_open);
            return {TaskReportStatus::TaskNotFound, std::nullopt};
        }

        if (!is_current_lease(*task, worker_id, lease_id, timestamp)) {
            rollback_if_open(db_, transaction_open);
            return {TaskReportStatus::StaleLease, *task};
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql = R"SQL(
            UPDATE tasks
            SET status = ?,
                assigned_worker_id = NULL,
                lease_expires_at_ms = NULL,
                updated_at_ms = ?,
                completed_at_ms = ?,
                last_error = NULL
            WHERE id = ?
              AND status = ?
              AND assigned_worker_id = ?
              AND lease_id = ?
              AND lease_expires_at_ms >= ?;
        )SQL";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("failed to prepare task completion");
        }

        bind_text(stmt, 1, to_string(TaskStatus::Succeeded));
        sqlite3_bind_int64(stmt, 2, timestamp);
        sqlite3_bind_int64(stmt, 3, timestamp);
        bind_text(stmt, 4, task_id);
        bind_text(stmt, 5, to_string(TaskStatus::Leased));
        bind_text(stmt, 6, worker_id);
        sqlite3_bind_int64(stmt, 7, lease_id);
        sqlite3_bind_int64(stmt, 8, timestamp);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("failed to complete task");
        }
        sqlite3_finalize(stmt);

        if (sqlite3_changes(db_) != 1) {
            rollback_if_open(db_, transaction_open);
            return {TaskReportStatus::StaleLease, task};
        }

        const auto updated_task = load_task_by_id(db_, task_id);
        if (!updated_task.has_value()) {
            throw std::runtime_error("completed task could not be loaded");
        }

        exec("COMMIT;");
        transaction_open = false;
        return {TaskReportStatus::Completed, updated_task};
    } catch (...) {
        rollback_if_open(db_, transaction_open);
        throw;
    }
}

TaskReportResult TaskStore::fail_task(const std::string& task_id,
                                      const std::string& worker_id,
                                      std::int64_t lease_id,
                                      const std::string& error_message) {
    if (lease_id <= 0) {
        throw std::invalid_argument("lease_id must be greater than zero");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto timestamp = now_ms();
    bool transaction_open = false;

    try {
        exec("BEGIN IMMEDIATE;");
        transaction_open = true;

        if (!worker_exists(db_, worker_id)) {
            rollback_if_open(db_, transaction_open);
            return {TaskReportStatus::WorkerNotFound, std::nullopt};
        }

        const auto task = load_task_by_id(db_, task_id);
        if (!task.has_value()) {
            rollback_if_open(db_, transaction_open);
            return {TaskReportStatus::TaskNotFound, std::nullopt};
        }

        if (!is_current_lease(*task, worker_id, lease_id, timestamp)) {
            rollback_if_open(db_, transaction_open);
            return {TaskReportStatus::StaleLease, *task};
        }

        const bool retryable = task->attempt_count < task->max_attempts;
        const TaskStatus next_status = retryable ? TaskStatus::Pending : TaskStatus::FailedFinal;
        const std::string normalized_error = error_message.empty()
            ? "worker reported failure"
            : error_message;

        sqlite3_stmt* stmt = nullptr;
        const char* sql = R"SQL(
            UPDATE tasks
            SET status = ?,
                assigned_worker_id = NULL,
                lease_expires_at_ms = NULL,
                updated_at_ms = ?,
                completed_at_ms = ?,
                last_error = ?
            WHERE id = ?
              AND status = ?
              AND assigned_worker_id = ?
              AND lease_id = ?
              AND lease_expires_at_ms >= ?;
        )SQL";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("failed to prepare task failure");
        }

        bind_text(stmt, 1, to_string(next_status));
        sqlite3_bind_int64(stmt, 2, timestamp);
        if (retryable) {
            sqlite3_bind_null(stmt, 3);
        } else {
            sqlite3_bind_int64(stmt, 3, timestamp);
        }
        bind_text(stmt, 4, normalized_error);
        bind_text(stmt, 5, task_id);
        bind_text(stmt, 6, to_string(TaskStatus::Leased));
        bind_text(stmt, 7, worker_id);
        sqlite3_bind_int64(stmt, 8, lease_id);
        sqlite3_bind_int64(stmt, 9, timestamp);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("failed to fail task");
        }
        sqlite3_finalize(stmt);

        if (sqlite3_changes(db_) != 1) {
            rollback_if_open(db_, transaction_open);
            return {TaskReportStatus::StaleLease, task};
        }

        const auto updated_task = load_task_by_id(db_, task_id);
        if (!updated_task.has_value()) {
            throw std::runtime_error("failed task could not be loaded");
        }

        exec("COMMIT;");
        transaction_open = false;
        return {
            retryable ? TaskReportStatus::FailedRetryable : TaskReportStatus::FailedFinal,
            updated_task
        };
    } catch (...) {
        rollback_if_open(db_, transaction_open);
        throw;
    }
}

void TaskStore::exec(const char* sql) const {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? "unknown SQLite error" : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}
