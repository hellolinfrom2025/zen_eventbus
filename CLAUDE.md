# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

This is a **Visual Studio 2022** project (MSVC v143 toolset) targeting **x64 Windows**, built as a **DLL** (`DynamicLibrary`). Language standard is **C++20**.

**Build via MSBuild (from PowerShell):**
```powershell
# Debug
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal

# Release
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Or use the VSCode tasks (`Ctrl+Shift+B` for the default Debug build task).

**Output locations:**
- Debug DLL: `bin/x64/Debug/zen_eventbusd.dll`
- Release DLL: `bin/x64/Release/zen_eventbus.dll`

**Install (deploy to shared library dir):**
```bat
install.bat
```
This copies headers and DLLs to `C:\ResourceLibrary\Code\C++\Libraries\x64-windows`.

## Architecture

`zen_eventbus` is an **asynchronous publish-subscribe event bus** exposed as a DLL and discovered at runtime via the `zen_rttr` reflection library.

### Key design patterns

**Factory + RTTR registration**: Consumers call `zen::eventbus::getEventBusFactory(lib_dir)` (defined inline in the public header). This function dynamically loads the DLL, then uses `zen_rttr` reflection to invoke the globally registered method `"zen::eventbus::getEventBusFactory"` which returns an `EventBusFactoryPtr`. This decouples the consumer from linking against the DLL directly.

**Pimpl idiom**: `EventBus` exposes the `IEventBus` interface; all implementation state lives in `EventBus::Impl` (defined only in `eventbus_impl.cpp`).

**Async dispatch**: `publish()` pushes work onto a `ctpl::thread_pool` (C++ Thread Pool Library). The thread count defaults to `std::thread::hardware_concurrency()`.

**Topic-gated pub/sub**: Topics must be registered (`addTopic`/`initTopics`) before `subscribe` or `publish` will accept them. Disabled topics (`TopicInfo::enable = false`) silently drop publishes. `subscribe()` returns a connection ID string (`"<topic>_<uuid>"`) used for `unsubscribe()`.

**Signals**: Each topic maps to a `boost::signals2::signal`. Subscriptions are `boost::signals2::connection` objects stored in `connections_`. Message values are type-erased as `zen_rttr::variant`.

### State machine

`EventBus::Impl` has four states: `kInit → kRun ↔ kPause → kEnd`. Re-`start()` from `kEnd` recreates the thread pool.

### External dependencies

| Dependency | Role |
|---|---|
| `zen_rttr` | Runtime reflection for DLL discovery; `variant` for type-erased messages |
| `boost::signals2` | Signal/slot mechanism for subscriptions |
| `boost::uuid` | Generates unique connection IDs |
| `ctpl` (header-only) | Thread pool for async dispatch (`ctpl/ctpl.h`) |
| Qt 5.15.2 (QtMsBuild) | Build tooling only; no Qt API used in library code |

Dependencies are resolved via shared `.props` files:
- `3rd/x64-windows/share/3rdLibDir.props`
- `Libraries/x64-windows/share/LibrariesDir.props`
- `VisualStudioProp/Normal.props`

### File layout

```
include/zen_eventbus/eventbus.h   ← Public API header (copied here by pre-build event)
zen_eventbus/eventbus.h           ← Source copy of the same public header
zen_eventbus/eventbus_impl.h/.cpp ← EventBus concrete class (pimpl internals)
zen_eventbus/eventbus_factory.h/.cpp ← Factory + RTTR registration
```

Pre-build event automatically copies `zen_eventbus/eventbus.h` → `include/zen_eventbus/eventbus.h`.

## Utility Scripts

**`convert.ps1`** — Converts all `.cpp`/`.h` source files (excluding `bin/`, `.vs/`, `.tmp/`) to UTF-8 BOM encoding, which MSVC requires for source files containing Chinese comments.
