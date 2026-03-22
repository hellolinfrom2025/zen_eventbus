#include "eventbus_factory.h"

#define ZEN_RTTR_DLL
#include <zen_rttr/registration>

#include "eventbus_impl.h"

using namespace zen::eventbus;

namespace zen
{
	namespace eventbus
	{
		EventBusFactoryPtr _getEventBusFactory();
	}
}

//注册RTTR
RTTR_REGISTRATION
{
	using namespace zen_rttr;
	registration::method("zen::eventbus::getEventBusFactory", &zen::eventbus::_getEventBusFactory);
} 

//---------------

std::string EventBusFactory::version() {
	return std::string("1.0");
}

EventBusPtr EventBusFactory::createEventBus(bool auto_start /*= true*/, const size_t thread_num /*= std::thread::hardware_concurrency()*/) {
	return std::make_shared<EventBus>(auto_start, thread_num);
}

//---------------
EventBusFactoryPtr zen::eventbus::_getEventBusFactory() {
	return std::make_shared<EventBusFactory>();
}


