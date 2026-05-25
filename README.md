# Distributed Task Scheduler (C++17)

A hands-on C++17 backend/systems project focused on building a lease-based distributed task scheduler with a scheduler HTTP API, SQLite-backed task persistence, explicit task state tracking, worker-agent groundwork, and a roadmap toward heartbeat-driven lease recovery and operational metrics.

## Project Goal

Build a portfolio-quality distributed systems project that demonstrates:

- Durable task state management
- Clear scheduler API boundaries
- Lease-based ownership semantics
- At-least-once execution reasoning
- Failure recovery and retry design
- Practical local operability in C++

## Current Scope

- Crow-based scheduler HTTP service
- SQLite-backed `TaskStore` with WAL mode enabled
- Durable `tasks` table for payload, status, retry metadata, lease metadata, and timestamps
- Task creation, listing, and lookup endpoints
- Health check endpoint
- Shared `scheduler_core` library used by scheduler and worker targets
- CMake build with separate `scheduler` and `worker` executables
- Worker model and status types defined as groundwork
- Placeholder worker-agent executable

## Features

- Task submission through `POST /tasks`
- Task listing through `GET /tasks`
- Single-task lookup through `GET /tasks/{task_id}`
- JSON request validation and JSON error responses
- Configurable scheduler database path from argv
- Configurable scheduler port from argv or `SCHEDULER_PORT`
- Thread-safe store access through an internal mutex
- SQLite persistence with explicit task lifecycle columns
- Task statuses represented as a typed enum: `PENDING`, `LEASED`, `SUCCEEDED`, `FAILED_FINAL`, and `CANCELLED`
- Generated task IDs with timestamp and process-local sequence number
- C++17 implementation using Crow and SQLite

## Important Behavior Notes

- The scheduler currently persists task records and exposes the scheduler API; full worker polling, lease assignment, completion reporting, and heartbeat handling are planned next-stage work.
- Task execution is intended to use at-least-once semantics. A task may run more than once if a worker loses its lease, crashes, or reports completion after lease expiry.
- Workers should treat external side effects as idempotent because lease-based recovery can reassign work.
- `TaskStore` serializes SQLite access with a mutex, which keeps the current scheduler behavior straightforward while the project is still single-scheduler-process.
- SQLite WAL mode is enabled during store initialization.
- The current worker executable is intentionally a placeholder while the scheduler API and persistence layer are established.
- `max_attempts` defaults to `3` and is validated as greater than zero when supplied in task creation requests.

## Project Structure

```text
.
|-- include/
|   |-- task.hpp
|   |-- task_store.hpp
|   `-- worker.hpp
|-- src/
|   |-- scheduler_main.cpp
|   |-- task_store.cpp
|   `-- worker_main.cpp
|-- CMakeLists.txt
`-- README.md
```

## Build and Run

### Requirements

- C++17 compiler
- CMake 3.15+
- SQLite3
- Crow
- vcpkg, or another package setup that provides CMake packages for SQLite3 and Crow

### Canonical Local Build

Configure with MSVC and vcpkg:

```powershell
cmake -S . -B build-msvc-vcpkg -DCMAKE_TOOLCHAIN_FILE=C:/Users/16210/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-msvc-vcpkg --config Debug
```

Run the scheduler API:

```powershell
.\build-msvc-vcpkg\Debug\scheduler.exe .\scheduler.db 8080
```

The scheduler also accepts the port from `SCHEDULER_PORT` when a port argument is not provided:

```powershell
$env:SCHEDULER_PORT = "8081"
.\build-msvc-vcpkg\Debug\scheduler.exe .\scheduler.db
```

Service endpoint:

```text
http://127.0.0.1:8080
```

### Worker Target

The repository builds a separate worker executable as part of the intended distributed-scheduler shape:

```powershell
.\build-msvc-vcpkg\Debug\worker.exe
```

Current output:

```text
worker agent placeholder
```

## Scheduler API

### Health Check

```text
GET /healthcheck
```

Example:

```powershell
curl.exe http://127.0.0.1:8080/healthcheck
```

### Create Task

```text
POST /tasks
Content-Type: application/json

{
  "payload": "demo task",
  "max_attempts": 3
}
```

Example:

```powershell
curl.exe -X POST http://127.0.0.1:8080/tasks -H "Content-Type: application/json" -d "{\"payload\":\"demo task\",\"max_attempts\":3}"
```

Successful response shape:

```json
{
  "id": "task-1710000000000-0",
  "payload": "demo task",
  "status": "PENDING",
  "attempt_count": 0,
  "max_attempts": 3,
  "created_at_ms": 1710000000000,
  "updated_at_ms": 1710000000000
}
```

### List Tasks

```text
GET /tasks
```

Example:

```powershell
curl.exe http://127.0.0.1:8080/tasks
```

Response shape:

```json
{
  "count": 1,
  "tasks": [
    {
      "id": "task-1710000000000-0",
      "payload": "demo task",
      "status": "PENDING",
      "attempt_count": 0,
      "max_attempts": 3,
      "created_at_ms": 1710000000000,
      "updated_at_ms": 1710000000000
    }
  ]
}
```

### Get One Task

```text
GET /tasks/{task_id}
```

Example:

```powershell
curl.exe http://127.0.0.1:8080/tasks/<task-id>
```

Missing tasks return:

```json
{
  "error": "Task not found"
}
```

## Task State Model

The implemented task status model is:

```text
PENDING
  -> LEASED
  -> SUCCEEDED

PENDING
  -> LEASED
  -> lease expired
  -> PENDING

LEASED
  -> FAILED_FINAL

PENDING or LEASED
  -> CANCELLED
```

The current API creates tasks in `PENDING`. The lease, completion, failure, and cancellation transitions are part of the planned worker/scheduler coordination layer.

## Architecture (Client -> Scheduler -> Store)

```text
Layer 1: Client API
  Crow HTTP server
  - accepts task creation and lookup requests
  - validates JSON input
  - returns JSON responses and JSON errors

Layer 2: Scheduler Service
  scheduler_main.cpp
  - owns the TaskStore instance
  - maps HTTP routes to store operations
  - exposes health check and task endpoints

Layer 3: Persistence
  TaskStore
  - initializes SQLite schema
  - creates durable task records
  - lists and fetches tasks
  - serializes DB access with a mutex
```

## Execution Flow Overview

This project is being built around a distributed scheduler flow: Submit -> Persist -> Lease -> Execute -> Report -> Recover.

1. Submit
- a client sends `POST /tasks` with a payload and optional retry limit
- the scheduler validates JSON shape and `max_attempts`
- `TaskStore::create_task(...)` inserts a durable `PENDING` task row

2. Persist
- task state is stored in SQLite with status, attempt count, max attempts, lease metadata, and timestamps
- `GET /tasks` and `GET /tasks/{task_id}` read from the same durable table

3. Lease
- planned worker polling will atomically move eligible `PENDING` tasks into `LEASED`
- leased tasks will record `assigned_worker_id` and `lease_expires_at_ms`

4. Execute and Report
- planned workers will execute leased work and report success or failure back to the scheduler
- success will transition a task to `SUCCEEDED`
- retry exhaustion will transition a task to `FAILED_FINAL`

5. Recover
- planned lease scanning will return expired `LEASED` tasks to `PENDING` when retry budget remains
- this creates at-least-once execution behavior and protects progress when workers disappear

## Planned Scheduler APIs

These endpoints are not implemented yet, but they describe the next intended milestone:

```text
POST /workers/register
POST /workers/{worker_id}/heartbeat
POST /workers/{worker_id}/poll
POST /tasks/{task_id}/complete
POST /tasks/{task_id}/fail
GET  /metrics
```

## Engineering Positioning

This project is intended as a backend/systems portfolio project rather than a CRUD demo. The focus is on showing how durable state, leases, retries, worker health, and recovery rules fit together in a small scheduler architecture.

The current milestone is the scheduler API and persistence foundation for a lease-based task execution platform. The next milestone is distributed worker coordination: registration, heartbeat tracking, lease assignment, completion/failure reporting, lease expiry, and metrics.

## Roadmap

- Add worker registration and heartbeat APIs
- Add worker polling with atomic lease assignment
- Add task completion and failure reporting endpoints
- Add bounded retry handling tied to `attempt_count` and `max_attempts`
- Add background lease-expiration scanning and task reassignment
- Add Prometheus-compatible metrics for task lifecycle and worker health
- Add Docker Compose demo for scheduler, workers, and failure/reassignment behavior
- Add focused tests for persistence, route validation, lease transitions, and retry exhaustion

## Author

**Weijia (J) Chen**  
C++ Backend / Systems Developer

## License

MIT License (c) 2025 Weijia Chen
