#ifndef __EVENTBUS_IMPL_H__
#define __EVENTBUS_IMPL_H__

#include <memory>
#include <functional>
#include <thread>
#include <string>

#include "eventbus.h"

ZEN_EVENTBUS_NAMESPACE_BEGIN

class EventBus :public IEventBus {
public:
	EventBus(bool auto_start = true, const size_t thread_num = std::thread::hardware_concurrency());
	~EventBus();
	EventBus(const EventBus&) = delete;
	EventBus& operator=(const EventBus&) = delete;
	virtual std::string version() override;
	//主题管理
	virtual bool addTopic(const TopicInfo& topic)override;
	virtual bool delTopic(const std::string& topic_name)override;
	virtual bool updateTopic(const TopicInfo& topic)override;
	virtual bool initTopics(const std::unordered_map<std::string, TopicInfo>& topics)override;
	virtual std::unordered_map<std::string, TopicInfo> getTopics()override;
	virtual TopicInfo getTopic(const std::string& topic_name)override;
	//主题订阅
	virtual  std::string subscribe(const std::string& topic, handler_t handler)override;
	virtual bool unsubscribe(const std::string& conn_id);
	virtual bool publish(const std::string& topic, const zen_rttr::variant& msg)override;
	//状态控制
	virtual bool start(const size_t thread_num = std::thread::hardware_concurrency())override;
	virtual bool stop(bool wait_done = false)override;
	virtual bool stopped()override;
	virtual bool forceStop()override;
	virtual bool pause()override;
	virtual bool resume()override;

private:
	struct Impl;
	std::unique_ptr<Impl> pimpl;
};

ZEN_EVENTBUS_NAMESPACE_END
#endif // !__EVENTBUS_IMPL_H__