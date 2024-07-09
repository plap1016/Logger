#pragma once

#include "Logging/Log.h"
#include "Task/TTask.h"
#include "HubApp/HubApp.h"
#include "configuration.hxx"
#include "syscfg.hxx"

#include <thread>
#include <memory>
#include <boost/asio.hpp>
#if defined(WIN32)
#include <libssh2/libssh2.h>
#include <libssh2/libssh2_sftp.h>
#else
#include <libssh2.h>
#include <libssh2_sftp.h>
#endif

#if defined(_DEBUG) && defined(WIN32)
extern HANDLE g_exitEvent;
#endif

extern std::string g_version;

namespace BA = boost::asio;

namespace Logging
{
	const uint32_t LC_Task = 0x0100;
	const uint32_t LC_Local = 0x0200;
	const uint32_t LC_Logger = 0x0400;
}

class ConfigMsg;
class PSubLocal;

class Logger_Dispatcher : public Task::TActiveTask<Logger_Dispatcher>, public Logging::LogClient
{
	friend HubApps::HubApp;
	HubApps::HubApp m_hub;
	void receiveEvent(PubSub::Message&& msg) { /*hand off to thread queue*/enqueue<PubSub::Message&&>(std::move(msg)); }
	void receiveUnknown(uint8_t, const std::string&) {}
	void eventBusConnected(HubApps::HubConnectionState state);

	//VEvent m_exitEvt;

	std::mutex m_dispLock;
	loggercfg::Logger m_cfg;
	void configure(const std::string& cfgStr);
	bool haveCfg = false;

	syscfg::Shared m_syscfg;
	void configSys(const std::string& cfgStr);
	bool m_haveSysCfg = false;

	VEvent m_newFileComplete;

	std::shared_ptr<PSubLocal> m_local;

	void start();
	void ftpUpload();
	//bool upload(CURL *curlhandle, const std::string& remotepath, const std::string& localpath, long timeout, long tries);
	//bool sftpResumeUpload(CURL *curlhandle, const std::string& remotepath, const std::string& localpath);
	//curl_off_t sftpGetRemoteFileSize(const char *i_remoteFile);
	bool matchEvent(const loggercfg::event_string_t& ev, const std::string& payload);

public:
	explicit Logger_Dispatcher(Logging::LogFile& log, const std::string& psubAddr = "127.0.0.1");
	~Logger_Dispatcher();

	const loggercfg::Logger& cfg() const { return m_cfg; }
	HubApps::HubApp& hub() { return m_hub; }
	constexpr const char* appName() const { return "Logger"; }
	constexpr std::string& version() const { return g_version; }

	void processMsg(PubSub::Message&& m);

	struct evNewFile;
	struct evNewFileCreated;
	struct evFlushFile;
	struct evFtpUpload;
	template <typename M> void processEvent();

};

