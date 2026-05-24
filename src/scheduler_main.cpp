#include <cstdlib>
#include <crow.h>
#include <iostream>
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

    return body;
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

