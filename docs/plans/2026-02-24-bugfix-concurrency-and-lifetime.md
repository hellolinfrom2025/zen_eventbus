# zen_eventbus Bug Fix Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix 8 identified bugs covering concurrency race conditions, DLL lifetime, incomplete cleanup, and interface design issues.

**Architecture:** All changes are minimal and localized — no public API signature changes. Fixes fall in three files: `zen_eventbus/eventbus.h` (public header), `zen_eventbus/eventbus_impl.cpp` (Impl struct), and the shared header `include/zen_eventbus/eventbus.h` (kept in sync via pre-build copy).

**Tech Stack:** C++20, MSVC v143, boost::signals2, boost::uuid, ctpl thread pool, zen_rttr reflection.

---

## Bug Reference

| # | 文件 | 问题 |
|---|------|------|
| B1 | eventbus_impl.cpp `stop()` | pool_->stop() 在锁外调用，两线程可双重停止 |
| B2 | eventbus.h `getEventBusFactory` | `lib` 是局部变量，DLL 可能在 RTTR 调用前被卸载 |
| B3 | eventbus_impl.cpp `subscribe()` | 状态检查在锁外，subscribe 可在 stop 清理后写入 connections_ |
| B4 | eventbus_impl.cpp `forceStop()` | 未断开信号槽、未清理 signals_/connections_ |
| B5 | eventbus_impl.cpp `start()` | 获锁后未重新校验状态（DCLP 不完整） |
| B6 | eventbus.h `getEventBusFactory` | `get_error_string().data()` 返回悬空指针；Release 下静默穿越 |
| B7 | eventbus.h 接口类 | IEventBus / IEventBusFactory 缺少虚析构函数 |
| B8 | eventbus_impl.cpp `subscribe()` | pause 状态下错误拒绝 subscribe/unsubscribe |

---

## Task 1: 接口虚析构函数（Bug 7）

**Files:**
- Modify: `zen_eventbus/eventbus.h:84-116`

最安全、最独立的修改，先做热身。

**Step 1: 在 IEventBusFactory 添加虚析构**

在 `zen_eventbus/eventbus.h` 第 84 行 `class IEventBusFactory` 的 public 区域首行插入：

```cpp
virtual ~IEventBusFactory() = default;
```

**Step 2: 在 IEventBus 添加虚析构**

在第 91 行 `class IEventBus` 的 public 区域，`IEventBus() = default;` 之后插入：

```cpp
virtual ~IEventBus() = default;
```

**Step 3: 构建验证**

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: Build succeeded, 0 Error(s).

---

## Task 2: DLL 持久化 + 错误处理（Bug 2 + Bug 6）

**Files:**
- Modify: `zen_eventbus/eventbus.h:45-82`

**Step 1: 将 `lib` 改为 static unique_ptr，修复悬空指针**

将 `getEventBusFactory` 函数体中 `if (!s_init.load(...) && !lib_dir.empty())` 块内的内容替换为：

```cpp
inline EventBusFactoryPtr getEventBusFactory(const std::string& lib_dir) {
	static std::mutex s_mtx;
	static std::atomic<bool> s_init{ false };
	static std::unique_ptr<zen_rttr::library> s_lib;   // 持久化，防止 DLL 被卸载
	if (!s_init.load(std::memory_order_acquire) && !lib_dir.empty()) {
		std::lock_guard<std::mutex> lck(s_mtx);
		if (!s_init.load(std::memory_order_relaxed)) {
#ifdef _DEBUG
			s_lib = std::make_unique<zen_rttr::library>(lib_dir + "/zen_eventbusd");
#else
			s_lib = std::make_unique<zen_rttr::library>(lib_dir + "/zen_eventbus");
#endif
			if (!s_lib->is_loaded()) {
				if (!s_lib->load()) {
					std::string err = s_lib->get_error_string(); // 不再悬空
					assert(false && "Failed to load zen_eventbus DLL");
					return nullptr;  // Release 下不再静默穿越
				}
			}
			s_init.store(true, std::memory_order_release);
		}
	}
	zen_rttr::method m = zen_rttr::type::get_global_method("zen::eventbus::getEventBusFactory");
	if (m.is_valid()) {
		zen_rttr::variant result = m.invoke({});
		if (result.is_valid()) {
			auto ptr = result.get_value<EventBusFactoryPtr>();
			if (ptr) {
				return ptr;
			}
		}
	}
	return nullptr;
}
```

注意：原来两个分支（`is_loaded` true/false）都设置 `s_init`，现在合并为构造后统一设置，逻辑更清晰。

**Step 2: 构建验证**

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: Build succeeded, 0 Error(s).

---

## Task 3: `start()` DCLP 补全（Bug 5）

**Files:**
- Modify: `zen_eventbus/eventbus_impl.cpp:209-223`

**Step 1: 在锁内添加状态二次校验**

将 `Impl::start()` 替换为：

```cpp
bool start(const size_t thread_num = std::thread::hardware_concurrency()) {
	// 锁外快速检查，过滤明显不满足条件的调用
	auto s = run_status_.load(std::memory_order_acquire);
	if (s != status::kInit && s != status::kEnd) {
		return false;
	}
	std::lock_guard<std::mutex> lck(mtx_status_change_);
	// 锁内再次校验（DCLP），防止并发 start() 重复创建线程池
	s = run_status_.load(std::memory_order_relaxed);
	if (s != status::kInit && s != status::kEnd) {
		return false;
	}
	if (s == status::kEnd || !pool_) {
		pool_ = std::make_unique<ctpl::thread_pool>(static_cast<int>(thread_num));
	} else {
		pool_->resize(static_cast<int>(thread_num));
	}
	run_status_.store(status::kRun, std::memory_order_release);
	return true;
}
```

**Step 2: 构建验证**

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: Build succeeded, 0 Error(s).

---

## Task 4: `stop()` 并发安全重构（Bug 1 + Bug 3 的 stop 侧）

**Files:**
- Modify: `zen_eventbus/eventbus_impl.cpp:245-263`

**Step 1: 重写 `stop()`**

将整个 `Impl::stop()` 替换为：

```cpp
bool stop(bool wait_done = false) {
	// 锁外快速检查
	if (run_status_.load(std::memory_order_acquire) == status::kEnd) {
		return true;
	}
	std::lock_guard<std::mutex> lck(mtx_status_change_);
	// 锁内二次检查，防止两线程同时进入
	if (run_status_.load(std::memory_order_relaxed) == status::kEnd) {
		return true;
	}
	// 先将状态置为 kEnd，publish() 后续调用将立即失败，不再向线程池推送
	run_status_.store(status::kEnd, std::memory_order_release);
	if (pool_) {
		pool_->stop(wait_done);
	}
	std::unique_lock<std::shared_mutex> lck_sig(mtx_signals_);
	for (auto& pair : signals_) {
		if (pair.second) {
			pair.second->disconnect_all_slots();
		}
	}
	signals_.clear();
	connections_.clear();
	return true;
}
```

关键点：
- `mtx_status_change_` 覆盖整个操作，防止并发双重停止
- `run_status_ = kEnd` 在 `pool_->stop()` 之前，消除 publish 向已停止线程池推任务的时间窗口

**Step 2: 构建验证**

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: Build succeeded, 0 Error(s).

---

## Task 5: `forceStop()` 补齐清理（Bug 4）

**Files:**
- Modify: `zen_eventbus/eventbus_impl.cpp:268-275`

**Step 1: 重写 `forceStop()`，与 stop() 对齐**

将 `Impl::forceStop()` 替换为：

```cpp
bool forceStop() {
	if (run_status_.load(std::memory_order_acquire) == status::kEnd) {
		return true;
	}
	std::lock_guard<std::mutex> lck(mtx_status_change_);
	if (run_status_.load(std::memory_order_relaxed) == status::kEnd) {
		return true;
	}
	run_status_.store(status::kEnd, std::memory_order_release);
	if (pool_) {
		pool_->stop();  // 不等待，立即强制停止
	}
	std::unique_lock<std::shared_mutex> lck_sig(mtx_signals_);
	for (auto& pair : signals_) {
		if (pair.second) {
			pair.second->disconnect_all_slots();
		}
	}
	signals_.clear();
	connections_.clear();
	return true;
}
```

**Step 2: 构建验证**

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: Build succeeded, 0 Error(s).

---

## Task 6: `subscribe()` / `unsubscribe()` 修复（Bug 3 subscribe 侧 + Bug 8）

**Files:**
- Modify: `zen_eventbus/eventbus_impl.cpp:136-178`

**Step 1: 重写 `subscribe()`**

两处改动：
1. 去掉 `kPause` 的早期拒绝（Bug 8）
2. 将状态检查移入 `mtx_signals_` 写锁内部，与 `stop()` 的清理互斥（Bug 3）

将 `Impl::subscribe()` 替换为：

```cpp
std::string subscribe(const std::string& topic, handler_t handler) {
	// 持有主题读锁直至完成订阅，防止与 delTopic 的 TOCTOU 竞争
	std::shared_lock<std::shared_mutex> lck_t(rwmtx_topics_);
	if (valid_topic_.find(topic) == valid_topic_.end()) {
		return std::string();
	}
	std::unique_lock<std::shared_mutex> lck_s(mtx_signals_);
	// 在信号锁内检查状态：与 stop() 的 disconnect_all_slots()+clear 互斥
	// kPause 不再拒绝订阅（pause 只冻结分发，不冻结注册）
	if (run_status_.load(std::memory_order_acquire) == status::kEnd) {
		return std::string();
	}
	auto it = signals_.find(topic);
	if (it == signals_.end()) {
		it = signals_.emplace(topic, std::make_shared<Signal>()).first;
	}
	if (!it->second) {
		it->second = std::make_shared<Signal>();
	}
	auto conn = it->second->connect(handler);
	// 线程局部 UUID 生成器，避免每次调用重新播种（性能优化）
	static thread_local boost::uuids::random_generator uuid_gen;
	std::string c_id;
	c_id.reserve(topic.size() + 37);
	c_id = topic;
	c_id += '_';
	c_id += boost::uuids::to_string(uuid_gen());
	connections_.emplace(c_id, std::move(conn));
	return c_id;
}
```

**Step 2: 修复 `unsubscribe()` — 去掉 kPause 限制（Bug 8）**

`unsubscribe()` 原本只检查 `kEnd`，无需修改。但确认注释一致性——当前代码：

```cpp
bool unsubscribe(const std::string& conn_id) {
	//处于结束状态不可继续
	if (run_status_ == status::kEnd) {
		return false;
	}
```

此处已经正确，无需修改。

**Step 3: 构建验证**

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: Build succeeded, 0 Error(s).

---

## Task 7: 最终全量构建（Debug + Release）

**Step 1: Debug 构建**

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: `bin/x64/Debug/zen_eventbusd.dll` 生成，0 Error(s).

**Step 2: Release 构建**

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe' zen_eventbus.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Expected: `bin/x64/Release/zen_eventbus.dll` 生成，0 Error(s).

---

## 变更摘要

| Task | 文件 | 修复 Bug |
|------|------|----------|
| 1 | eventbus.h | B7 — 虚析构函数 |
| 2 | eventbus.h | B2 + B6 — DLL 持久化 + 悬空指针 |
| 3 | eventbus_impl.cpp | B5 — start() DCLP |
| 4 | eventbus_impl.cpp | B1 + B3(stop侧) — stop() 并发安全 |
| 5 | eventbus_impl.cpp | B4 — forceStop() 清理 |
| 6 | eventbus_impl.cpp | B3(subscribe侧) + B8 — subscribe 竞态 + pause 语义 |
| 7 | — | 全量构建验证 |
