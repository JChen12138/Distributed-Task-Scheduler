#include <cstdlib>
#include <crow.h>
#include <iostream>
#include <stdexcept>
#include <string>

#include "task_store.hpp"

namespace {
crow::response json_response(int status, crow::json::wvalue body) {
    return crow::response(status, "application/json", body.dump());
}

crow::response json_error(int status, const std::string& message) {
    crow::json::wvalue body;
    body["error"] = message;
    return json_response(status, std::move(body));
}

crow::json::wvalue task_to_json(const Task& task) {
    crow::json::wvalue body;
    body["id"] = task.id;
    body["payload"] = task.payload;
    body["status"] = to_string(task.status);
    body["attempt_count"] = task.attempt_count;
    body["max_attempts"] = task.max_attempts;
    body["lease_id"] = task.lease_id;
    body["created_at_ms"] = task.created_at_ms;
    body["updated_at_ms"] = task.updated_at_ms;

    if (task.assigned_worker_id.has_value()) {
        body["assigned_worker_id"] = *task.assigned_worker_id;
    }
    if (task.lease_expires_at_ms.has_value()) {
        body["lease_expires_at_ms"] = *task.lease_expires_at_ms;
    }
    if (task.completed_at_ms.has_value()) {
        body["completed_at_ms"] = *task.completed_at_ms;
    }
    if (task.last_error.has_value()) {
        body["last_error"] = *task.last_error;
    }

    return body;
}

crow::json::wvalue worker_to_json(const Worker& worker) {
    crow::json::wvalue body;
    body["id"] = worker.id;
    body["status"] = to_string(worker.status);
    body["registered_at_ms"] = worker.registered_at_ms;
    body["last_heartbeat_at_ms"] = worker.last_heartbeat_at_ms;
    return body;
}

int parse_positive_int_field(const crow::json::rvalue& request_body,
                             const char* field_name,
                             int default_value) {
    if (!request_body.has(field_name)) {
        return default_value;
    }
    if (request_body[field_name].t() != crow::json::type::Number) {
        throw std::invalid_argument(std::string("Field '") + field_name + "' must be a number");
    }

    const int value = static_cast<int>(request_body[field_name].i());
    if (value <= 0) {
        throw std::invalid_argument(std::string("Field '") + field_name + "' must be greater than zero");
    }
    return value;
}

std::string parse_required_string_field(const crow::json::rvalue& request_body,
                                        const char* field_name) {
    if (!request_body.has(field_name) ||
        request_body[field_name].t() != crow::json::type::String) {
        throw std::invalid_argument(std::string("Field '") + field_name + "' is required and must be a string");
    }

    const std::string value = std::string(request_body[field_name].s());
    if (value.empty()) {
        throw std::invalid_argument(std::string("Field '") + field_name + "' must not be empty");
    }
    return value;
}

std::int64_t parse_required_positive_i64_field(const crow::json::rvalue& request_body,
                                               const char* field_name) {
    if (!request_body.has(field_name) ||
        request_body[field_name].t() != crow::json::type::Number) {
        throw std::invalid_argument(std::string("Field '") + field_name + "' is required and must be a number");
    }

    const auto value = request_body[field_name].i();
    if (value <= 0) {
        throw std::invalid_argument(std::string("Field '") + field_name + "' must be greater than zero");
    }
    return value;
}

crow::response task_report_response(const TaskReportResult& result) {
    if (result.status == TaskReportStatus::TaskNotFound) {
        return json_error(404, "Task not found");
    }
    if (result.status == TaskReportStatus::WorkerNotFound) {
        return json_error(404, "Worker not registered");
    }
    if (result.status == TaskReportStatus::StaleLease) {
        crow::json::wvalue body;
        body["error"] = "Task lease is no longer current";
        if (result.task.has_value()) {
            body["task"] = task_to_json(*result.task);
        }
        return json_response(409, std::move(body));
    }

    crow::json::wvalue body;
    body["accepted"] = true;
    body["status"] = result.status == TaskReportStatus::Completed
        ? "COMPLETED"
        : result.status == TaskReportStatus::FailedRetryable
            ? "FAILED_RETRYABLE"
            : "FAILED_FINAL";
    body["task"] = task_to_json(*result.task);
    return json_response(200, std::move(body));
}

int parse_port(int argc, char** argv) {
    if (argc > 2) {
        return std::stoi(argv[2]);
    }

    const char* env_port = std::getenv("SCHEDULER_PORT");
    if (env_port != nullptr) {
        return std::stoi(env_port);
    }

    return 8080;
}
}

int main(int argc, char** argv) {
    const std::string db_path = argc > 1 ? argv[1] : "scheduler.db";

    try {
        TaskStore store(db_path);
        store.initialize();

        crow::SimpleApp app;

        CROW_ROUTE(app, "/healthcheck").methods("GET"_method)([] {
            crow::json::wvalue body;
            body["status"] = "ok";
            return json_response(200, std::move(body));
        });

        CROW_ROUTE(app, "/tasks").methods("POST"_method)([&store](const crow::request& req) {
            const auto request_body = crow::json::load(req.body);
            if (!request_body) {
                return json_error(400, "Request body must be valid JSON");
            }
            if (!request_body.has("payload") ||
                request_body["payload"].t() != crow::json::type::String) {
                return json_error(400, "Field 'payload' is required and must be a string");
            }

            int max_attempts = 3;
            if (request_body.has("max_attempts")) {
                if (request_body["max_attempts"].t() != crow::json::type::Number) {
                    return json_error(400, "Field 'max_attempts' must be a number");
                }
                max_attempts = static_cast<int>(request_body["max_attempts"].i());
                if (max_attempts <= 0) {
                    return json_error(400, "Field 'max_attempts' must be greater than zero");
                }
            }

            try {
                const Task task = store.create_task(std::string(request_body["payload"].s()), max_attempts);
                return json_response(201, task_to_json(task));
            } catch (const std::exception& ex) {
                return json_error(500, ex.what());
            }
        });

        CROW_ROUTE(app, "/tasks").methods("GET"_method)([&store] {
            try {
                const auto tasks = store.list_tasks();
                crow::json::wvalue::list task_list;
                task_list.reserve(tasks.size());
                for (const auto& task : tasks) {
                    task_list.push_back(task_to_json(task));
                }

                crow::json::wvalue body;
                body["count"] = static_cast<int>(tasks.size());
                body["tasks"] = std::move(task_list);
                return json_response(200, std::move(body));
            } catch (const std::exception& ex) {
                return json_error(500, ex.what());
            }
        });

        CROW_ROUTE(app, "/tasks/<string>").methods("GET"_method)([&store](const std::string& task_id) {
            try {
                const auto task = store.get_task(task_id);
                if (!task.has_value()) {
                    return json_error(404, "Task not found");
                }
                return json_response(200, task_to_json(*task));
            } catch (const std::exception& ex) {
                return json_error(500, ex.what());
            }
        });

        CROW_ROUTE(app, "/workers/register").methods("POST"_method)([&store](const crow::request& req) {
            const auto request_body = crow::json::load(req.body);
            if (!request_body) {
                return json_error(400, "Request body must be valid JSON");
            }
            if (!request_body.has("worker_id") ||
                request_body["worker_id"].t() != crow::json::type::String) {
                return json_error(400, "Field 'worker_id' is required and must be a string");
            }

            const std::string worker_id = std::string(request_body["worker_id"].s());
            if (worker_id.empty()) {
                return json_error(400, "Field 'worker_id' must not be empty");
            }

            try {
                const Worker worker = store.register_worker(worker_id);
                return json_response(201, worker_to_json(worker));
            } catch (const std::exception& ex) {
                return json_error(500, ex.what());
            }
        });

        CROW_ROUTE(app, "/workers/<string>/heartbeat").methods("POST"_method)(
            [&store](const std::string& worker_id) {
                if (worker_id.empty()) {
                    return json_error(400, "Worker id must not be empty");
                }

                try {
                    const auto worker = store.record_worker_heartbeat(worker_id);
                    if (!worker.has_value()) {
                        return json_error(404, "Worker not registered");
                    }
                    return json_response(200, worker_to_json(*worker));
                } catch (const std::exception& ex) {
                    return json_error(500, ex.what());
                }
            });

        CROW_ROUTE(app, "/workers").methods("GET"_method)([&store] {
            try {
                const auto workers = store.list_workers();
                crow::json::wvalue::list worker_list;
                worker_list.reserve(workers.size());
                for (const auto& worker : workers) {
                    worker_list.push_back(worker_to_json(worker));
                }

                crow::json::wvalue body;
                body["count"] = static_cast<int>(workers.size());
                body["workers"] = std::move(worker_list);
                return json_response(200, std::move(body));
            } catch (const std::exception& ex) {
                return json_error(500, ex.what());
            }
        });

        CROW_ROUTE(app, "/workers/<string>/poll").methods("POST"_method)(
            [&store](const crow::request& req, const std::string& worker_id) {
                if (worker_id.empty()) {
                    return json_error(400, "Worker id must not be empty");
                }

                int lease_ms = 30000;
                if (!req.body.empty()) {
                    const auto request_body = crow::json::load(req.body);
                    if (!request_body) {
                        return json_error(400, "Request body must be valid JSON");
                    }

                    try {
                        lease_ms = parse_positive_int_field(request_body, "lease_ms", lease_ms);
                    } catch (const std::invalid_argument& ex) {
                        return json_error(400, ex.what());
                    }
                }

                try {
                    const auto poll_result = store.poll_task_for_worker(worker_id, lease_ms);
                    if (poll_result.status == WorkerPollStatus::WorkerNotFound) {
                        return json_error(404, "Worker not registered");
                    }

                    crow::json::wvalue body;
                    body["worker_id"] = worker_id;
                    body["lease_ms"] = lease_ms;

                    if (poll_result.status == WorkerPollStatus::NoTaskAvailable) {
                        body["leased"] = false;
                        body["task"] = nullptr;
                        return json_response(200, std::move(body));
                    }

                    body["leased"] = true;
                    body["task"] = task_to_json(*poll_result.task);
                    return json_response(200, std::move(body));
                } catch (const std::exception& ex) {
                    return json_error(500, ex.what());
                }
            });

        CROW_ROUTE(app, "/tasks/<string>/complete").methods("POST"_method)(
            [&store](const crow::request& req, const std::string& task_id) {
                const auto request_body = crow::json::load(req.body);
                if (!request_body) {
                    return json_error(400, "Request body must be valid JSON");
                }

                std::string worker_id;
                std::int64_t lease_id = 0;
                try {
                    worker_id = parse_required_string_field(request_body, "worker_id");
                    lease_id = parse_required_positive_i64_field(request_body, "lease_id");
                } catch (const std::invalid_argument& ex) {
                    return json_error(400, ex.what());
                }

                try {
                    return task_report_response(store.complete_task(task_id, worker_id, lease_id));
                } catch (const std::exception& ex) {
                    return json_error(500, ex.what());
                }
            });

        CROW_ROUTE(app, "/tasks/<string>/fail").methods("POST"_method)(
            [&store](const crow::request& req, const std::string& task_id) {
                const auto request_body = crow::json::load(req.body);
                if (!request_body) {
                    return json_error(400, "Request body must be valid JSON");
                }

                std::string worker_id;
                std::int64_t lease_id = 0;
                std::string error_message;
                try {
                    worker_id = parse_required_string_field(request_body, "worker_id");
                    lease_id = parse_required_positive_i64_field(request_body, "lease_id");
                    if (request_body.has("error")) {
                        if (request_body["error"].t() != crow::json::type::String) {
                            return json_error(400, "Field 'error' must be a string");
                        }
                        error_message = std::string(request_body["error"].s());
                    }
                } catch (const std::invalid_argument& ex) {
                    return json_error(400, ex.what());
                }

                try {
                    return task_report_response(store.fail_task(task_id, worker_id, lease_id, error_message));
                } catch (const std::exception& ex) {
                    return json_error(500, ex.what());
                }
            });

        const int port = parse_port(argc, argv);
        std::cout << "scheduler listening on http://127.0.0.1:" << port << "\n";
        std::cout << "using task database: " << db_path << "\n";

        app.bindaddr("127.0.0.1").port(port).multithreaded().run();
    } catch (const std::exception& ex) {
        std::cerr << "scheduler failed: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}

