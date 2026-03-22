# AGENTS.md

This file provides guidance to agents when working with code in this repository.

## Build Commands (MSBuild from PowerShell)

```powershell
# Debug build
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal

# Release build
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Output: `bin/x64/Debug/zen_eventbusd.dll` or `bin/x64/Release/zen_eventbus.dll`

## Critical Project Rules

1. **UTF-8 BOM encoding required** — Run `.\convert.ps1` after editing `.cpp`/`.h` files (MSVC requirement for Chinese comments)

2. **Edit source header, not include/** — `zen_eventbus/eventbus.h` is the source; `include/zen_eventbus/eventbus.h` is auto-copied by pre-build event

3. **Pimpl idiom strictly enforced** — All `EventBus` state lives in `EventBus::Impl` defined only in `.cpp`; never move members to header

4. **No exceptions** — Return `false` or empty `std::string` for errors; use `assert(0)` only for unrecoverable programmer errors

5. **Debug DLL suffix** — `zen_eventbusd.dll` (Debug) vs `zen_eventbus.dll` (Release); use `#ifdef _DEBUG` to select

## Architecture

- **Factory + RTTR Discovery**: Consumers load DLL at runtime via `zen_rttr::library`, invoke `"zen::eventbus::getEventBusFactory"` to get factory
- **State Machine**: `kInit → kRun ↔ kPause → kEnd`; re-`start()` from `kEnd` recreates thread pool
- **Topic-gated pub/sub**: Topics must be registered before use; disabled topics silently drop publishes
- **Async dispatch**: `publish()` pushes to `ctpl::thread_pool`; default threads = `std::thread::hardware_concurrency()`

## Synchronization Pattern

- `std::shared_mutex` for read-heavy maps (`rwmtx_topics_`, `mtx_signals_`)
- `std::mutex` for state changes (`mtx_status_change_`) with DCLP pattern
- Scoped lock blocks to minimize lock duration

## No Test Suite

Validation is manual via separate consumer project. No automated tests exist.
