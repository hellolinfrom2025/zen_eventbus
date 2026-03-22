#include "eventbus_impl.h"

#include <map>
#include <memory>
#include <shared_mutex>

#include <boost/signals2.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "ctpl/ctpl.h"

using namespace zen::eventbus;

////////////////////////////////////////////////
struct EventBus::Impl {
	enum class status :int32_t {
		kInit = 0,
		kRun,
		kPause,
		kEnd
	};
	using Signal = boost::signals2::signal<void(const zen_rttr::variant&)>;
	using SignalPtr = std::shared_ptr<Signal>;
	using Connection = boost::signals2::connection;
	std::unordered_map<std::string, SignalPtr> signals_;
	std::unordered_map<std::string, Connection> connections_;
	std::shared_mutex mtx_signals_;
	std::unique_ptr<ctpl::thread_pool> pool_;
	std::atomic<status> run_status_ = status::kInit;
	std::mutex mtx_status_change_;

	std::unordered_map<std::string, TopicInfo>valid_topic_;
	std::shared_mutex rwmtx_topics_;

	Impl(bool auto_start = true, const size_t thread_num = std::thread::hardware_concurrency()) {
		if (auto_start) {
			start(thread_num);
		}
	}

	~Impl() {
		stop(true);
	}

	//
	bool addTopic(const TopicInfo& topic) {
		if (topic.name.empty()) {
			return false;
		}
		std::unique_lock<std::shared_mutex> lck(rwmtx_topics_);
		if (valid_topic_.find(topic.name) != valid_topic_.end()) {
			return false;
		}
		valid_topic_[topic.name] = topic;
		return true;
	}
	bool delTopic(const std::string& topic_name) {
		std::unique_lock<std::shared_mutex> lck(rwmtx_topics_);
		if (valid_topic_.find(topic_name) == valid_topic_.end()) {
			return false;
		}
		valid_topic_.erase(topic_name);
		//删除主题信号及其所有订阅连接
		{
			std::unique_lock<std::shared_mutex> lck_sig(mtx_signals_);
			auto it = signals_.find(topic_name);
			if (it != signals_.end() && it->second != nullptr) {
				it->second->disconnect_all_slots();
				signals_.erase(it);
			}
			//清理该主题下的所有连接记录
			std::string prefix = topic_name + "_";
			for (auto cit = connections_.begin(); cit != connections_.end(); ) {
				if (cit->first.rfind(prefix, 0) == 0) {
					cit = connections_.erase(cit);
				} else {
					++cit;
				}
			}
		}
		return true;
	}
	bool updateTopic(const TopicInfo& topic) {
		std::unique_lock<std::shared_mutex> lck(rwmtx_topics_);
		if (valid_topic_.find(topic.name) == valid_topic_.end()) {
			return false;
		}
		valid_topic_[topic.name] = topic;
		return true;
	}
	bool initTopics(const std::unordered_map<std::string, TopicInfo>& topics) {
		std::unique_lock<std::shared_mutex> lck(rwmtx_topics_);
		valid_topic_ = topics;
		//删除多余主题信号及连接
		{
			std::unique_lock<std::shared_mutex> lck_sig(mtx_signals_);
			for (auto it = signals_.begin(); it != signals_.end();) {
				//如果主题信号已经不存在，则删除
				if (valid_topic_.find(it->first) == valid_topic_.end()) {
					if (it->second) {
						it->second->disconnect_all_slots();
					}
					//清理该主题的连接记录
					std::string prefix = it->first + "_";
					for (auto cit = connections_.begin(); cit != connections_.end(); ) {
						if (cit->first.rfind(prefix, 0) == 0) {
							cit = connections_.erase(cit);
						} else {
							++cit;
						}
					}
					it = signals_.erase(it);
				}
				else {
					it++;
				}
			}
		}
		return true;
	}
	std::unordered_map<std::string, TopicInfo> getTopics() {
		std::shared_lock<std::shared_mutex> lck(rwmtx_topics_);
		return valid_topic_;
	}
	TopicInfo getTopic(const std::string& topic_name) {
		std::shared_lock<std::shared_mutex> lck(rwmtx_topics_);
		if (valid_topic_.find(topic_name) != valid_topic_.end()) {
			return valid_topic_.at(topic_name);
		}
		return TopicInfo();
	}

	//
	std::string subscribe(const std::string& topic, handler_t handler) {
		//持有主题读锁直至完成订阅，防止与 delTopic 的 TOCTOU 竞争
		std::shared_lock<std::shared_mutex> lck_t(rwmtx_topics_);
		if (valid_topic_.find(topic) == valid_topic_.end()) {
			return std::string();
		}
		std::unique_lock<std::shared_mutex> lck_s(mtx_signals_);
		//在信号锁内检查状态：与 stop() 的清理操作互斥；pause 不再阻止订阅
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
		//线程局部 UUID 生成器，避免每次调用重新播种（性能优化）
		static thread_local boost::uuids::random_generator uuid_gen;
		std::string c_id;
		c_id.reserve(topic.size() + 37);
		c_id = topic;
		c_id += '_';
		c_id += boost::uuids::to_string(uuid_gen());
		connections_.emplace(c_id, std::move(conn));
		return c_id;
	}

	bool unsubscribe(const std::string& conn_id) {
		std::unique_lock<std::shared_mutex> lck(mtx_signals_);
		// 在锁内检查状态，与 stop() 的清理互斥
		if (run_status_.load(std::memory_order_acquire) == status::kEnd) {
			return false;
		}
		auto it = connections_.find(conn_id);
		if (it != connections_.end()) {
			it->second.disconnect();
			connections_.erase(it);
			return true;
		}
		return false;
	}

	bool publish(const std::string& topic, const zen_rttr::variant& msg) {
		// 早期检查：线程池未初始化
		if (!pool_) {
			return false;
		}
		// 使用 .load() 保持内存序一致性
		auto status = run_status_.load(std::memory_order_acquire);
		if (status == status::kEnd || status == status::kPause) {
			return false;
		}
		// 验证主题并缓存信号指针（合并两次加锁为一次，消除热路径上的重复查找）
		SignalPtr sig;
		{
			std::shared_lock<std::shared_mutex> lck_t(rwmtx_topics_);
			auto tit = valid_topic_.find(topic);
			if (tit == valid_topic_.end() || !tit->second.enable) {
				return false;
			}
			std::shared_lock<std::shared_mutex> lck_s(mtx_signals_);
			// 在锁内再次检查状态，与 stop() 互斥
			status = run_status_.load(std::memory_order_acquire);
			if (status == status::kEnd || status == status::kPause) {
				return false;
			}
			auto sit = signals_.find(topic);
			if (sit == signals_.end() || !sit->second) {
				return false;
			}
			sig = sit->second;  // 持锁期间拷贝 shared_ptr，延长 Signal 生命周期
		}
		// 异步分发：lambda 不持有任何 Impl 成员引用
		// stop() 调用 disconnect_all_slots() 后，(*sig)(msg) 为空操作，无 use-after-free 风险
		pool_->push([sig, msg](int) {
			(*sig)(msg);
		});
		return true;
	}
	//状态切换
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
		// 总是重建线程池，避免复用已停止的线程池
		pool_ = std::make_unique<ctpl::thread_pool>(static_cast<int>(thread_num));
		run_status_.store(status::kRun, std::memory_order_release);
		return true;
	}

	bool pause() {
		//锁外快速检查
		auto s = run_status_.load(std::memory_order_acquire);
		if (s != status::kRun) {
			return false;
		}
		std::lock_guard<std::mutex> lck(mtx_status_change_);
		//锁内再次校验（DCLP）
		if (run_status_.load(std::memory_order_relaxed) != status::kRun) {
			return false;
		}
		run_status_.store(status::kPause, std::memory_order_release);
		return true;
	}

	bool resume() {
		//锁外快速检查
		auto s = run_status_.load(std::memory_order_acquire);
		if (s != status::kPause) {
			return false;
		}
		std::lock_guard<std::mutex> lck(mtx_status_change_);
		//锁内再次校验（DCLP）
		if (run_status_.load(std::memory_order_relaxed) != status::kPause) {
			return false;
		}
		run_status_.store(status::kRun, std::memory_order_release);
		return true;
	}

	bool stop(bool wait_done = false) {
		//锁外快速检查
		if (run_status_.load(std::memory_order_acquire) == status::kEnd) {
			return true;
		}
		std::lock_guard<std::mutex> lck(mtx_status_change_);
		//锁内二次检查，防止两线程同时进入
		if (run_status_.load(std::memory_order_relaxed) == status::kEnd) {
			return true;
		}
		//先将状态置为 kEnd：publish() 后续调用立即失败，不再向线程池推送
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

	bool stopped() {
		return run_status_.load(std::memory_order_acquire) == status::kEnd;
	}

	bool forceStop() {
		//锁外快速检查
		if (run_status_.load(std::memory_order_acquire) == status::kEnd) {
			return true;
		}
		std::lock_guard<std::mutex> lck(mtx_status_change_);
		if (run_status_.load(std::memory_order_relaxed) == status::kEnd) {
			return true;
		}
		run_status_.store(status::kEnd, std::memory_order_release);
		if (pool_) {
			pool_->stop();
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
};

////////////////////////////////////////////////

EventBus::EventBus(bool auto_start/* = true*/, const size_t thread_num /*= std::thread::hardware_concurrency()*/)
	: pimpl(std::make_unique<Impl>(auto_start, thread_num)) {}

EventBus::~EventBus() { stop(); }

std::string EventBus::version() {
	return std::string("1.0");
}

bool EventBus::addTopic(const TopicInfo& topic) {
	return pimpl->addTopic(topic);
}

bool EventBus::delTopic(const std::string& topic_name) {
	return pimpl->delTopic(topic_name);
}

bool EventBus::updateTopic(const TopicInfo& topic) {
	return pimpl->updateTopic(topic);
}

bool EventBus::initTopics(const std::unordered_map<std::string, TopicInfo>& topics) {
	return pimpl->initTopics(topics);
}

std::unordered_map<std::string, TopicInfo> EventBus::getTopics() {
	return pimpl->getTopics();
}

TopicInfo EventBus::getTopic(const std::string& topic_name) {
	return pimpl->getTopic(topic_name);
}

//
std::string EventBus::subscribe(const std::string& topic, handler_t handler) {
	return pimpl->subscribe(topic, handler);
}

bool EventBus::publish(const std::string& topic, const zen_rttr::variant& msg) {
	return pimpl->publish(topic, msg);
}

bool EventBus::unsubscribe(const std::string& connection_id) {
	return pimpl->unsubscribe(connection_id);
}

bool EventBus::start(const size_t thread_num /*= std::thread::hardware_concurrency()*/) {
	return pimpl->start(thread_num);
}

bool EventBus::stop(bool wait_done/* = false*/) {
	return pimpl->stop(wait_done);
}

bool EventBus::stopped() {
	return pimpl->stopped();
}

bool EventBus::forceStop() {
	return pimpl->forceStop();
}

bool EventBus::pause() {
	return pimpl->pause();
}

bool EventBus::resume() {
	return pimpl->resume();
}