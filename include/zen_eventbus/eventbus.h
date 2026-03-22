#ifndef __ZEN_EVENTBUS_H__
#define __ZEN_EVENTBUS_H__

#define ZEN_EVENTBUS_NAMESPACE_BEGIN namespace zen{ namespace eventbus{
#define ZEN_EVENTBUS_NAMESPACE_END   }}

#include <memory>
#include <cassert>
#include <string>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <atomic>

#define ZEN_RTTR_DLL
#include <zen_rttr/variant.h>
#include <zen_rttr/library.h>

#ifdef _DEBUG
#pragma comment(lib,"zen_rttr_core_096d.lib")
#else
#pragma comment(lib,"zen_rttr_core_096.lib")
#endif // _DEBUG

ZEN_EVENTBUS_NAMESPACE_BEGIN

class IEventBus;
class IEventBusFactory;
using EventBusPtr = std::shared_ptr<IEventBus>;
using EventBusFactoryPtr = std::shared_ptr<IEventBusFactory>;

struct TopicInfo {
	TopicInfo() = default;
	TopicInfo(const std::string& name, const std::string& group = std::string(),
		const std::string& desp = std::string(), const std::string& val_desp = std::string(), const bool enable = true)
		:name(name), group(group), desp(desp), val_desp(val_desp), enable(enable) {};
	std::string name;
	std::string group;
	std::string desp;
	std::string val_desp;
	bool enable = true;
};
using handler_t = std::function<void(const zen_rttr::variant&)>;

inline EventBusFactoryPtr getEventBusFactory(const std::string& lib_dir) {
	//线程安全的一次性 DLL 加载（双重检查加锁）
	static std::mutex s_mtx;
	static std::atomic<bool> s_init{ false };
	static std::unique_ptr<zen_rttr::library> s_lib;   // 持久化，防止析构时卸载 DLL
	if (!s_init.load(std::memory_order_acquire) && !lib_dir.empty()) {
		std::lock_guard<std::mutex> lck(s_mtx);
		if (!s_init.load(std::memory_order_relaxed)) {
#ifdef _DEBUG
			s_lib = std::make_unique<zen_rttr::library>(lib_dir + "/zen_eventbusd");
#else
			s_lib = std::make_unique<zen_rttr::library>(lib_dir + "/zen_eventbus");
#endif // _DEBUG
			if (!s_lib->is_loaded()) {
				if (!s_lib->load()) {
					std::string err = s_lib->get_error_string().to_string();  // 不再是 .data()，无悬空指针
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

class IEventBusFactory
{
public:
	virtual ~IEventBusFactory() = default;
	virtual std::string version() = 0;
	virtual EventBusPtr createEventBus(bool auto_start = true, const size_t thread_num = std::thread::hardware_concurrency()) = 0;
};

class IEventBus {
public:
	IEventBus() = default;
	virtual ~IEventBus() = default;
	IEventBus(const IEventBus&) = delete;
	IEventBus& operator=(const IEventBus&) = delete;
	virtual std::string version() = 0;

	//主题管理
	virtual bool addTopic(const TopicInfo& topic) = 0;
	virtual bool delTopic(const std::string& topic_name) = 0;
	virtual bool updateTopic(const TopicInfo& topic) = 0;
	virtual bool initTopics(const std::unordered_map<std::string, TopicInfo>& topics) = 0;
	virtual std::unordered_map<std::string, TopicInfo> getTopics() = 0;
	virtual TopicInfo getTopic(const std::string& topic_name) = 0;
	//主题订阅
	virtual  std::string subscribe(const std::string& topic, handler_t handler) = 0;
	virtual bool unsubscribe(const std::string& conn_id) = 0;
	virtual bool publish(const std::string& topic, const zen_rttr::variant& msg) = 0;
	//状态控制
	virtual bool start(const size_t thread_num = std::thread::hardware_concurrency()) = 0;
	virtual bool stop(bool wait_done = false) = 0;
	virtual bool stopped() = 0;
	virtual bool forceStop() = 0;
	virtual bool pause() = 0;
	virtual bool resume() = 0;
};

ZEN_EVENTBUS_NAMESPACE_END
#endif// !__ZEN_EVENTBUS_H__