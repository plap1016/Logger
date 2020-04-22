#pragma once

#include "Logging/Log.h"
#include "gzstream.h"

#include "Task/TTask.h"
#include "PubSubLib/PubSub.h"

#include <boost/asio.hpp>
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

class PSubLocal : public Task::TTask<PSubLocal>, public PubSub::TPubSubClient<PSubLocal>, public Logging::LogClient
{
	friend PubSub::TPubSubClient<PSubLocal>;

	uint8_t readBuff[1024];
	Logger_Dispatcher& m_disp;

	uint32_t m_evtCount = 0;

	std::recursive_mutex m_lk;
	ogzstream m_strm;
	std::string m_fname;
	qpc_clock::time_point m_start_time;
	qpc_clock::time_point m_time_marker;

	BA::ip::tcp::socket m_sock;
	bool m_running;
	void sendBuffer(const std::string& buff)
	{
		boost::system::error_code error = BA::error::broken_pipe;

		BA::write(m_sock, BA::buffer(frameMsg(buff)), error);
		if (error)
			m_sock.close();
	};

	Task::MsgDelayMsgPtr m_reconectMsg;
	Task::MsgDelayMsgPtr m_flushMsg;

	void OnConnect(const boost::system::error_code& error);
	void OnReadSome(const boost::system::error_code& error, size_t bytes_transferred);

	void pSubUnknownMsg(uint8_t type, const std::string& payload) {}

	bool initNewFile(void);

public:
	PSubLocal(Logger_Dispatcher& disp);
	~PSubLocal();

	void start();
	void stop();

	const std::string& currentFileName() { return m_fname; }

	struct FlushEvt;
	template <typename T> void processEvent(void);

	void processMsg(const PubSub::Message& m);
};

