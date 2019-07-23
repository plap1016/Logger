#pragma once

#include "Logging/Log.h"
#include "Task/TTask.h"
#include "PubSubLib/PubSub.h"
#include "configuration.hxx"
#include "syscfg.hxx"

#include <thread>
#include <memory>
#include <boost/asio.hpp>
//#include <curl/curl.h>
#include <libssh2/libssh2.h>
#include <libssh2/libssh2_sftp.h>

#if defined(_DEBUG) && defined(WIN32)
extern HANDLE g_exitEvent;
#endif

namespace Logging
{
	const uint32_t LC_Task = 0x0001;
	const uint32_t LC_PubSub = 0x0002;
	const uint32_t LC_TcpConn = 0x0004;
	const uint32_t LC_Local = 0x0008;
	const uint32_t LC_Logger = 0x0010;
}

class ConfigMsg;
class PSubLocal;

class Logger_Dispatcher
	: public Task::TActiveTask<Logger_Dispatcher>
	, public PubSub::TPubSubClient<Logger_Dispatcher>
	, public Logging::LogClient
{
	friend PubSub::TPubSubClient<Logger_Dispatcher>;

	uint8_t readBuff[1024];

	std::thread m_sockThread;
	VEvent m_exitEvt;
	void socketThread();
	boost::asio::ip::tcp::endpoint m_pubsubaddr;
	boost::asio::io_service m_iosvc;
	std::shared_ptr<boost::asio::ip::tcp::socket> m_sockptr;
	void sendBuffer(const std::string& buff)
	{
		boost::system::error_code error = boost::asio::error::broken_pipe;

		boost::asio::write(*m_sockptr, boost::asio::buffer(buff), error);
		if (error)
			m_sockptr->close();
	};

	std::recursive_mutex m_dispLock;
	LogConfig::Logger m_cfg;
	void configure(const std::string& cfgStr);
	bool haveCfg = false;

	syscfg::Shared m_syscfg;
	void configSys(const std::string& cfgStr);
	bool m_haveSysCfg = false;

	VEvent m_newFileComplete;

	Task::MsgDelayMsgPtr m_cfgAliveDeferred;
	Task::MsgDelayMsgPtr m_here;

	std::shared_ptr<PSubLocal> m_local;

	void start();
	void ftpUpload();
	//bool upload(CURL *curlhandle, const std::string& remotepath, const std::string& localpath, long timeout, long tries);
	//bool sftpResumeUpload(CURL *curlhandle, const std::string& remotepath, const std::string& localpath);
	//curl_off_t sftpGetRemoteFileSize(const char *i_remoteFile);
	bool matchEvent(const LogConfig::event_string_t& ev, const std::string& payload);

	void OnConnect(const boost::system::error_code& error);
	void OnReadSome(const boost::system::error_code& error, size_t bytes_transferred);

public:
	Logger_Dispatcher(Logging::LogFile& log, const std::string& psubAddr = "127.0.0.1");
	~Logger_Dispatcher();

	//void start();
	//void stop();

	const LogConfig::Logger& cfg() const { return m_cfg; }
	boost::asio::io_service& iosvc() { return m_iosvc; }
	boost::asio::ip::tcp::endpoint pSubAddr(void) { return m_pubsubaddr; }

	void processMsg(const PubSub::Message& m);

	struct evCfgDeferred;
	struct evHereTime;
	struct evNewFile;
	struct evNewFileCreated;
	struct evFlushFile;
	struct evFtpUpload;
	template <typename M> void processEvent();

};

