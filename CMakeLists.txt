cmake_minimum_required(VERSION 3.15)

project(DistributedTaskScheduler VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

add_library(scheduler_core
    src/task_store.cpp
)

target_include_directories(scheduler_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(SQLite3 REQUIRED)
find_package(Crow CONFIG REQUIRED)

target_link_libraries(scheduler_core PUBLIC
    SQLite::SQLite3
)

add_executable(scheduler
    src/scheduler_main.cpp
)

target_link_libraries(scheduler PRIVATE
    scheduler_core
    Crow::Crow
)

if(WIN32)
    target_link_libraries(scheduler PRIVATE
        ws2_32
        mswsock
    )
endif()

add_executable(worker
    src/worker_main.cpp
)

target_link_libraries(worker PRIVATE
    scheduler_core
)

