# Distributed Task Scheduler (C++17)

A hands-on C++17 backend/systems project focused on a lease-based task scheduler with a Crow HTTP API, SQLite-backed task and worker state, atomic task leasing, lease renewal, lease-expiration recovery, lease-token validation, retry handling, and explicit task lifecycle tracking.

## Project Goal

Build a portfolio-quality distributed systems project that demonstrates:

- durable task and worker state
- lease-based task ownership
- at-least-once execution reasoning
- retry and stale-result handling
- clear scheduler API boundaries
- practical local operability in C++

## Current Scope

- Crow-based scheduler HTTP service
- SQLite-backed `TaskStore` with WAL mode enabled
- Durable `tasks` table for payload, status, retry metadata, lease metadata, timestamps, and `last_error`
- Durable `workers` table for worker identity, status, registration time, and heartbeat time
- Task creation, listing, lookup, lease renewal, completion, and failure-reporting endpoints
- Worker registration, heartbeat, polling, and listing endpoints
- Atomic task leasing from `PENDING` to `LEASED` using a SQLite transaction
- Lease renewal for long-running tasks
- Background lease-expiration scanning and task reassignment
- Worker timeout detection that marks stale workers `OFFLINE`
- Lease-token validation through a per-task `lease_id`
- Retryable failure behavior until `max_attempts` is exhausted
- Shared `scheduler_core` library with separate `scheduler` and placeholder `worker` executables

## Recent Additions

- Added lease renewal through `POST /tasks/{task_id}/renew`
- Added a background lease-maintenance loop controlled by `SCHEDULER_LEASE_SCAN_INTERVAL_MS`
- Added worker timeout handling controlled by `WORKER_TIMEOUT_MS`
- Added automatic reassignment behavior: expired retryable leases return to `PENDING`
- Added automatic terminal failure for expired leases whose retry budget is exhausted
- Added worker polling through `POST /workers/{worker_id}/poll`
- Added task completion and failure reporting through `POST /tasks/{task_id}/complete` and `POST /tasks/{task_id}/fail`
- Added `lease_id` as a lease-generation token so stale worker reports can be rejected
- Added `last_error` tracking for failed task attempts
- Added lightweight schema migration for existing task databases missing `lease_id` or `last_error`
- Added no-work poll responses so workers can distinguish an empty queue from an error

## Important Behavior Notes

- Worker polling leases the oldest eligible `PENDING` task, assigns it to the polling worker, sets `lease_expires_at_ms`, increments `attempt_count`, and increments `lease_id`.
- Lease renewal extends `lease_expires_at_ms` only when the reported `worker_id` and `lease_id` still match the current task lease.
- The background scanner periodically marks stale workers `OFFLINE`, returns retryable expired leases to `PENDING`, and moves exhausted expired leases to `FAILED_FINAL`.
- Requeued work receives a new `lease_id` the next time a worker polls it, so old worker reports are rejected.
- Completion and failure reports must include the current `worker_id` and `lease_id`; stale workers, stale lease generations, and expired leases are rejected with `409 Conflict`.
- Failure reporting stores `last_error` and returns the task to `PENDING` while attempts remain. Once `attempt_count` reaches `max_attempts`, the task moves to `FAILED_FINAL`.
- Polling refreshes the worker heartbeat and keeps the worker marked `ONLINE`.
- Task execution is intended to use at-least-once semantics, so workers should treat external side effects as idempotent.
- The current worker executable is still a placeholder; the implemented work is on the scheduler-side API and persistence boundary.

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

```powershell
cmake -S . -B build-msvc-vcpkg -DCMAKE_TOOLCHAIN_FILE=C:/Users/16210/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-msvc-vcpkg --config Debug
```

Run the scheduler API:

```powershell
.\build-msvc-vcpkg\Debug\scheduler.exe .\scheduler.db 8080
```

The scheduler also accepts the port from `SCHEDULER_PORT` when a port argument is not provided.

Optional scheduler timing knobs:

```powershell
$env:SCHEDULER_LEASE_SCAN_INTERVAL_MS = "1000"
$env:WORKER_TIMEOUT_MS = "30000"
```

Defaults:

- `SCHEDULER_LEASE_SCAN_INTERVAL_MS=1000`
- `WORKER_TIMEOUT_MS=30000`

```text
http://127.0.0.1:8080
```

### CMake Status

The repository currently defines three build targets:

- `scheduler_core`: shared library containing SQLite-backed store logic
- `scheduler`: Crow HTTP API process
- `worker`: placeholder worker-agent executable linked against `scheduler_core`

## API Overview

| Endpoint | Purpose |
|----------|---------|
| `GET /healthcheck` | Liveness check |
| `POST /tasks` | Create a pending task |
| `GET /tasks` | List tasks |
| `GET /tasks/{task_id}` | Fetch one task |
| `POST /workers/register` | Register or refresh a worker |
| `POST /workers/{worker_id}/heartbeat` | Record worker heartbeat |
| `GET /workers` | List workers |
| `POST /workers/{worker_id}/poll` | Lease one pending task if available |
| `POST /tasks/{task_id}/renew` | Renew the current task lease |
| `POST /tasks/{task_id}/complete` | Complete a leased task |
| `POST /tasks/{task_id}/fail` | Report task failure |

Minimal local flow:

```powershell
curl.exe -X POST http://127.0.0.1:8080/workers/register -H "Content-Type: application/json" -d "{\"worker_id\":\"worker-1\"}"
curl.exe -X POST http://127.0.0.1:8080/tasks -H "Content-Type: application/json" -d "{\"payload\":\"demo task\",\"max_attempts\":3}"
curl.exe -X POST http://127.0.0.1:8080/workers/worker-1/poll -H "Content-Type: application/json" -d "{\"lease_ms\":30000}"
curl.exe -X POST http://127.0.0.1:8080/tasks/<task-id>/complete -H "Content-Type: application/json" -d "{\"worker_id\":\"worker-1\",\"lease_id\":1}"
```

Report failure instead of completion:

```powershell
curl.exe -X POST http://127.0.0.1:8080/tasks/<task-id>/fail -H "Content-Type: application/json" -d "{\"worker_id\":\"worker-1\",\"lease_id\":1,\"error\":\"execution failed\"}"
```

Polling request body is optional; when `lease_ms` is omitted, the scheduler uses a 30 second lease. Polling an empty queue returns a successful no-task response instead of an error.

Renew a lease while a worker is still running:

```powershell
curl.exe -X POST http://127.0.0.1:8080/tasks/<task-id>/renew -H "Content-Type: application/json" -d "{\"worker_id\":\"worker-1\",\"lease_id\":1,\"lease_ms\":30000}"
```

Renewal keeps the same `lease_id`; it only extends `lease_expires_at_ms`. If the lease already expired, was reassigned, or belongs to another worker, renewal returns `409 Conflict`.

## Task State Model

```text
PENDING
  -> LEASED
  -> SUCCEEDED

LEASED
  -> failure reported, retry attempts remain
  -> PENDING

LEASED
  -> FAILED_FINAL

LEASED
  -> lease expired
  -> PENDING

LEASED
  -> lease expired, retry attempts exhausted
  -> FAILED_FINAL

PENDING or LEASED
  -> CANCELLED  planned
```

## Architecture

```text
Client / Worker
  -> Crow scheduler API
  -> TaskStore
  -> SQLite tasks/workers tables
```

`TaskStore` owns the main coordination rules: schema initialization, task creation, worker registration, heartbeat updates, atomic lease assignment, lease renewal, lease-expiration recovery, and lease validation before completion or failure reports are accepted. Access is serialized with an internal mutex, which keeps the current single-scheduler-process design straightforward.

## Execution Flow

1. A client creates a task with `POST /tasks`; the scheduler stores it as `PENDING`.
2. A worker registers, then polls through `POST /workers/{worker_id}/poll`.
3. The scheduler leases the oldest eligible pending task, updates lease metadata, and returns the task to the worker.
4. The worker renews the lease if execution needs more time.
5. The worker reports success or failure with its `worker_id` and `lease_id`.
6. The scheduler accepts only current leases; stale or expired reports are rejected.
7. Failed tasks retry until `max_attempts` is exhausted, then move to `FAILED_FINAL`.
8. If a worker disappears, the background scanner marks it `OFFLINE` and requeues or terminal-fails its leased tasks based on retry budget.

## Tests

There is not a dedicated automated test suite in this repository yet. The current verification path is API-level manual testing with `curl.exe` against the running scheduler.

Manual coverage currently exercises:

- task creation, listing, and lookup
- worker registration, heartbeat, and listing
- worker polling with task lease assignment
- empty-queue polling behavior
- task completion with current lease ownership
- retryable failure and terminal failure behavior
- stale worker result rejection
- lease renewal with current lease ownership
- expired lease reassignment to another worker
- expired lease terminal failure after retry exhaustion
- stale worker timeout to `OFFLINE`

## Engineering Positioning

This project is a backend/systems portfolio project rather than a CRUD demo. The focus is on durable state, leases, retries, worker health, and recovery rules in a small scheduler architecture.

The current milestone is the scheduler API and persistence foundation for a lease-based task execution platform. The next milestone is turning the placeholder worker target into a real worker agent and adding metrics/demo scripts.

## Roadmap

- Replace the placeholder worker executable with a polling worker agent
- Add task cancellation endpoint
- Add Prometheus-compatible metrics for task lifecycle and worker health
- Add Docker Compose demo for scheduler, workers, and failure/reassignment behavior
- Add focused tests for persistence, route validation, lease transitions, and retry exhaustion

## Author

**Weijia (J) Chen**  
C++ Backend / Systems Developer

## License

MIT License (c) 2025 Weijia Chen
