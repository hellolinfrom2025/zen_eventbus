#ifndef __EVENTBUS_FACTORY_H__
#define __EVENTBUS_FACTORY_H__

#include "EventBus.h"

ZEN_EVENTBUS_NAMESPACE_BEGIN

class EventBusFactory : public IEventBusFactory {
public:
	EventBusFactory() = default;
	virtual ~EventBusFactory() = default;
	virtual std::string version() override;
	virtual EventBusPtr createEventBus(bool auto_start = true, const size_t thread_num = std::thread::hardware_concurrency())override;
};

ZEN_EVENTBUS_NAMESPACE_END
#endif// !__EVENTBUS_FACTORY_H__