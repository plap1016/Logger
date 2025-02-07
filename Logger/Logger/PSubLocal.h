#pragma once

#include "Logging/Log.h"
#include "gzstream.h"

#include "Task/TTask.h"
#include "HubApp/HubApp.h"
#include "configuration.hxx"

#include <mutex>
#include <memory>
#include <thread>
#include <deque>
#include <chrono>

class Logger_Dispatcher;
class ReconnectEvt;
class NewfileEvt;
class NewfileEvtSync;

namespace BA = boost::asio;

class PSubLocal : public Task::TTask<PSubLocal>, public Logging::LogClient
{
	//Logger_Dispatcher& m_disp;
	loggercfg::Logger m_cfg;
	std::function<void()>& m_onNewFile;

	friend HubApps::HubHandler;
	std::unique_ptr<HubApps::HubHandler> m_hub;
	void receiveEvent(PubSub::Message&& msg) { /*hand off to thread queue*/enqueue<PubSub::Message&&>(std::move(msg)); }
	void receiveUnknown(uint8_t, const std::string&) {}
	void eventBusConnected(HubApps::HubConnectionState state);

	uint32_t m_evtCount{0};
	uint32_t m_evtMax{1000000}; // Sane default but should be overridden by default config anyway
	uint32_t m_flushSec{3600};  // As above

	std::mutex m_lk;
	ogzstream m_strm{};
	std::string m_fname;
	std::chrono::steady_clock::time_point m_start_time;
	std::chrono::steady_clock::time_point m_time_marker;

	bool m_running{false};
	Task::MsgDelayMsgPtr m_flushMsg;

	bool initNewFile(void);

public:
	explicit PSubLocal(Task::TaskMsgDispatcher&, Logging::LogFile&, HubApps::HubCore&, const loggercfg::Logger&, std::function<void()>);

	void start();
	void stop();

	const std::string& currentFileName() { return m_fname; }

	struct FlushEvt;
	template <typename T> void processEvent(void);

	void processMsg(PubSub::Message&& m);
};

