#include <iostream>
#include <string>

#include "task_store.hpp"

int main(int argc, char** argv) {
    const std::string db_path = argc > 1 ? argv[1] : "scheduler.db";

    try {
        TaskStore store(db_path);
        store.initialize();

        const Task task = store.create_task(R"({"type":"demo","sleep_ms":1000})", 3);

        std::cout << "scheduler storage initialized at " << db_path << "\n";
        std::cout << "created demo task: " << task.id << "\n";
        std::cout << "task count: " << store.list_tasks().size() << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "scheduler failed: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
