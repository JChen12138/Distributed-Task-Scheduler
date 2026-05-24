# Distributed Task Scheduler

A C++ backend infrastructure project that implements a lease-based distributed task scheduler. The system includes a scheduler node, multiple worker agents, persistent task state, worker heartbeats, lease expiration, retry handling, task reassignment, and Prometheus-style metrics.

The project is designed to demonstrate distributed systems and platform engineering fundamentals: failure detection, ownership leases, at-least-once execution semantics, graceful worker loss handling, explicit task state transitions, and operational visibility.

## Execution Model

The scheduler uses at-least-once execution semantics. A task is expected to eventually reach a terminal state when healthy workers are available, but it may execute more than once if a worker dies, loses its lease, or reports completion after its lease has expired.

Workers should treat task execution as idempotent when external side effects matter.

## Initial Scope

- Single scheduler process
- Multiple worker-agent processes
- SQLite-backed task persistence
- Worker registration and heartbeat tracking
- Lease-based task assignment
- Lease expiration and task reassignment
- Bounded retry attempts
- Prometheus-style metrics
- Docker Compose failure demo

## Current Implementation

- CMake project with separate `scheduler` and `worker` executables
- Shared `scheduler_core` library
- SQLite-backed `TaskStore`
- Durable `tasks` table
- Scheduler HTTP API for task creation, listing, and lookup
- Placeholder worker entry point

## Scheduler API

Create a task:

```text
POST /tasks
Content-Type: application/json

{
  "payload": "demo task",
  "max_attempts": 3
}
```

List tasks:

```text
GET /tasks
```

Get one task:

```text
GET /tasks/{task_id}
```

Health check:

```text
GET /healthcheck
```

## Task State Model

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

## Build Plan

1. Add SQLite-backed task persistence.
2. Add scheduler HTTP endpoints for task submission and task listing.
3. Add worker registration and heartbeat APIs.
4. Add worker polling with lease assignment.
5. Add completion and failure reporting.
6. Add lease-expiration background scanning.
7. Add worker-agent executable.
8. Add metrics, Docker Compose, and failure demo scripts.

## Local Build

Configure with MSVC and vcpkg:

```powershell
cmake -S . -B build-msvc-vcpkg -DCMAKE_TOOLCHAIN_FILE=C:/Users/16210/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-msvc-vcpkg --config Debug
```

Run the scheduler API:

```powershell
.\build-msvc-vcpkg\Debug\scheduler.exe .\scheduler.db 8080
```

Create a task:

```powershell
curl.exe -X POST http://127.0.0.1:8080/tasks -H "Content-Type: application/json" -d "{\"payload\":\"demo task\",\"max_attempts\":3}"
```

List tasks:

```powershell
curl.exe http://127.0.0.1:8080/tasks
```

Get one task:

```powershell
curl.exe http://127.0.0.1:8080/tasks/<task-id>
```
