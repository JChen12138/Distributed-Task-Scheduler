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

## Recent Additions

- Added a durable `workers` table alongside the existing task table
- Added worker registration with idempotent re-registration behavior
- Added worker heartbeat updates that refresh `last_heartbeat_at_ms` and keep workers marked `ONLINE`
- Added worker listing through the scheduler API
- Extended the shared store layer so task and worker state are managed through the same SQLite-backed persistence boundary

## Current Scope

- Crow-based scheduler HTTP service
- SQLite-backed `TaskStore` with WAL mode enabled
- Durable `tasks` table for payload, status, retry metadata, lease metadata, and timestamps
- Durable `workers` table for worker identity, status, registration time, and heartbeat time
- Task creation, listing, and lookup endpoints
- Worker registration, heartbeat, and listing endpoints
- Health check endpoint
- Shared `scheduler_core` library used by scheduler and worker targets
- CMake build with separate `scheduler` and `worker` executables
- Worker model and status types defined as groundwork
- Placeholder worker-agent executable

## Features

- Task submission through `POST /tasks`
- Task listing through `GET /tasks`
- Single-task lookup through `GET /tasks/{task_id}`
- Worker registration through `POST /workers/register`
- Worker heartbeat updates through `POST /workers/{worker_id}/heartbeat`
- Worker listing through `GET /workers`
- JSON request validation and JSON error responses
- Configurable scheduler database path from argv
- Configurable scheduler port from argv or `SCHEDULER_PORT`
- Thread-safe store access through an internal mutex
- SQLite persistence with explicit task lifecycle columns
- Task statuses represented as a typed enum: `PENDING`, `LEASED`, `SUCCEEDED`, `FAILED_FINAL`, and `CANCELLED`
- Generated task IDs with timestamp and process-local sequence number
- Worker status parsing and serialization through a typed enum: `ONLINE` and `OFFLINE`
- C++17 implementation using Crow and SQLite

## Important Behavior Notes

- The scheduler currently persists task and worker records and exposes the scheduler API; full worker polling, lease assignment, and completion reporting are planned next-stage work.
- Task execution is intended to use at-least-once semantics. A task may run more than once if a worker loses its lease, crashes, or reports completion after lease expiry.
- Workers should treat external side effects as idempotent because lease-based recovery can reassign work.
- `TaskStore` serializes SQLite access with a mutex, which keeps the current scheduler behavior straightforward while the project is still single-scheduler-process.
- SQLite WAL mode is enabled during store initialization.
- The current worker executable is intentionally a placeholder while the scheduler-side worker coordination API is established.
- `max_attempts` defaults to `3` and is validated as greater than zero when supplied in task creation requests.
- Worker registration is idempotent: re-registering an existing worker refreshes heartbeat state and marks it online without replacing the original registration timestamp.
- Heartbeats are accepted only for registered workers; unknown workers receive a `404` response.

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

### CMake Status

The repository currently defines three build targets:

- `scheduler_core`: shared library containing SQLite-backed store logic
- `scheduler`: Crow HTTP API process
- `worker`: placeholder worker-agent executable linked against `scheduler_core`

The current build path expects CMake packages for SQLite3 and Crow to be available through the selected toolchain, such as vcpkg on Windows.

### Worker Target

The repository builds a separate worker executable as part of the intended distributed-scheduler shape:

```powershell
.\build-msvc-vcpkg\Debug\worker.exe
```

Current output:

```text
worker agent placeholder
```

## Tests

There is not a dedicated automated test suite in this repository yet. The current verification path is API-level manual testing with `curl.exe` against the running scheduler.

Manual coverage currently exercises:

- Health check response
- Task creation validation and persistence
- Task listing and lookup
- Worker registration validation and persistence
- Idempotent worker re-registration behavior
- Worker heartbeat success and missing-worker failure
- Worker listing

Planned test coverage:

- Store-level tests for task and worker persistence
- Route validation tests for JSON request shape and error responses
- Lease assignment and retry transition tests once worker polling is implemented

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

### Register Worker

```text
POST /workers/register
Content-Type: application/json

{
  "worker_id": "worker-1"
}
```

Example:

```powershell
curl.exe -X POST http://127.0.0.1:8080/workers/register -H "Content-Type: application/json" -d "{\"worker_id\":\"worker-1\"}"
```

Registration is idempotent. Re-registering the same worker marks it `ONLINE` and refreshes `last_heartbeat_at_ms` while preserving the original `registered_at_ms`.

Successful response shape:

```json
{
  "id": "worker-1",
  "status": "ONLINE",
  "registered_at_ms": 1710000000000,
  "last_heartbeat_at_ms": 1710000000000
}
```

### Worker Heartbeat

```text
POST /workers/{worker_id}/heartbeat
```

Example:

```powershell
curl.exe -X POST http://127.0.0.1:8080/workers/worker-1/heartbeat
```

Missing workers return:

```json
{
  "error": "Worker not registered"
}
```

### List Workers

```text
GET /workers
```

Example:

```powershell
curl.exe http://127.0.0.1:8080/workers
```

Response shape:

```json
{
  "count": 1,
  "workers": [
    {
      "id": "worker-1",
      "status": "ONLINE",
      "registered_at_ms": 1710000000000,
      "last_heartbeat_at_ms": 1710000005000
    }
  ]
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
  - exposes health check, task, and worker endpoints

Layer 3: Persistence
  TaskStore
  - initializes SQLite schema
  - creates durable task records
  - lists and fetches tasks
  - registers workers and records heartbeats
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

3. Register Workers
- workers register through `POST /workers/register`
- worker state is stored in SQLite with identity, status, registration time, and latest heartbeat time
- heartbeats update `last_heartbeat_at_ms` and keep the worker marked `ONLINE`

4. Lease
- planned worker polling will atomically move eligible `PENDING` tasks into `LEASED`
- leased tasks will record `assigned_worker_id` and `lease_expires_at_ms`

5. Execute and Report
- planned workers will execute leased work and report success or failure back to the scheduler
- success will transition a task to `SUCCEEDED`
- retry exhaustion will transition a task to `FAILED_FINAL`

6. Recover
- planned lease scanning will return expired `LEASED` tasks to `PENDING` when retry budget remains
- this creates at-least-once execution behavior and protects progress when workers disappear

## Planned Scheduler APIs

These endpoints are not implemented yet, but they describe the next intended milestone:

```text
POST /workers/{worker_id}/poll
POST /tasks/{task_id}/complete
POST /tasks/{task_id}/fail
GET  /metrics
```

## Engineering Positioning

This project is intended as a backend/systems portfolio project rather than a CRUD demo. The focus is on showing how durable state, leases, retries, worker health, and recovery rules fit together in a small scheduler architecture.

The current milestone is the scheduler API and persistence foundation for a lease-based task execution platform. The next milestone is task leasing: worker polling, lease assignment, completion/failure reporting, lease expiry, and metrics.

## Roadmap

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
