#include "stdafx.h"
#include "Logger_Dispatcher.h"
#include "configuration-pimpl.hxx"
#include "PSubLocal.h"

#include <stdint.h>
#include <boost/asio.hpp>
#include <boost/optional.hpp>

#include <string>
#include <sstream>

using namespace Logging;

const PubSub::Subject PUB_ALIVE{ "Alive", "Logger" };
const PubSub::Subject PUB_HERE{ "Here", "Logger" };
const PubSub::Subject PUB_DEAD{ "Dead", "Logger" };
const PubSub::Subject SUB_CFG_ALIVE{ "Alive", "CFG" };
const PubSub::Subject SUB_CFG{ "CFG", "Logger" };

#if defined(_DEBUG) && defined(WIN32)
const PubSub::Subject SUB_DIE{ "Die", "Logger" };
HANDLE g_exitEvent;
#endif

extern std::string g_version;

const uint32_t PUB_STATUS_TTL = 60000;

Logger_Dispatcher::Logger_Dispatcher(Logging::LogFile& log, const std::string& psubAddr)
	: Task::TActiveTask<Logger_Dispatcher>(2)
	, Logging::LogClient(log)
	, m_sockThread()
	, m_pubsubaddr(boost::asio::ip::tcp::socket::endpoint_type(boost::asio::ip::address_v4::from_string(psubAddr), 3101))
{
	m_sockThread = std::thread(std::bind(&Logger_Dispatcher::socketThread, this));

	getMsgDispatcher().start();
}


Logger_Dispatcher::~Logger_Dispatcher()
{
	getMsgDispatcher().stop();

	m_iosvc.stop();
}

//void Logger_Dispatcher::start()
//{
//	LOG(LL_Debug, LC_Logger, "start");
//	if (!sockThread)
//		sockThread = new std::thread(std::bind(&Logger_Dispatcher::socketThread, this));
//
//	if (!getMsgDispatcher().started())
//		getMsgDispatcher().start();
//}
//
//void Logger_Dispatcher::stop()
//{
//	LOG(LL_Debug, LC_Logger, "stop");
//	sendMsg(PubSub::Message(PUB_DEAD, PUB_STATUS_TTL));
//
//	if (m_here)
//	{
//		m_here->cancelMsg();
//		m_here.reset();
//	}
//
//	while (getMsgDispatcher().started())
//		getMsgDispatcher().stop();
//
//	m_exitEvt.setAll();
//	if (sockThread)
//	{
//		sockThread->join();
//		delete sockThread;
//		sockThread = nullptr;
//	}
//}

void Logger_Dispatcher::configure(const std::string& cfgStr)
{
	LogConfig::Logger_paggr s;
	xml_schema::document_pimpl d(s.root_parser(), s.root_name());

	std::istringstream cfgstrm(cfgStr);

	s.pre();

	try
	{
		d.parse(cfgstrm);
		m_cfg = s.post();

		m_local.reset(new PSubLocal(*this));
		m_local->start();

		haveCfg = true;
	}
	catch (xml_schema::parser_exception& ex)
	{
		LOG(Logging::LL_Warning, Logging::LC_Logger, "CONFIG ERROR: The following errors were found:\r\n" << ex.what());

		PubSub::Message err;
		err.subject = { "Error", "Logger", "Config" };
		err.payload = ex.text();
		err.payload += " ";
		err.payload += ex.what();

		sendMsg(err);
	}
}

//template <typename MutableBufferSequence>
//std::size_t readWithTimeout(boost::asio::ip::tcp::socket& s, const MutableBufferSequence& buffers, const boost::asio::deadline_timer::duration_type& expiry_time)
//{
//	boost::optional<boost::system::error_code> timer_result;
//	boost::asio::deadline_timer timer(s.get_io_service());
//	timer.expires_from_now(expiry_time);
//	timer.async_wait([&timer_result](const boost::system::error_code& error) { timer_result.reset(error); });
//
//	boost::optional<boost::system::error_code> read_result;
//	boost::optional<std::size_t> read_size_result;
//	s.async_read_some(buffers, [&read_result, &read_size_result](const boost::system::error_code& error, size_t sz) { read_result.reset(error); read_size_result.reset(sz); });
//
//	s.get_io_service().reset();
//	while (s.get_io_service().run_one())
//	{
//		if (read_result)
//			timer.cancel();
//		else if (timer_result)
//			s.cancel();
//	}
//
//	if (!*timer_result)
//		return 0;
//
//	if (*read_result)
//		throw boost::system::system_error(*read_result);
//
//	return *read_size_result;
//}

void Logger_Dispatcher::socketThread()
{
	try
	{
		BA::io_service::work work(m_iosvc);
		start();
		m_iosvc.run();
	}
	catch (const std::exception& ex)
	{
		LOG(LL_Severe, LC_Logger, ex.what());
		throw ex;
	}
}

void Logger_Dispatcher::start()
{
	if (!m_sockptr)
		m_sockptr.reset(new BA::ip::tcp::socket(m_iosvc));
	else
		m_sockptr->close();

	m_sockptr->async_connect(m_pubsubaddr, boost::bind(&Logger_Dispatcher::OnConnect, this, BA::placeholders::error));
}

template <> void Logger_Dispatcher::processEvent<ReconnectEvt>(void)
{
	start();
}

void Logger_Dispatcher::OnConnect(const boost::system::error_code& error)
{
	if (error)
		enqueueWithDelay<ReconnectEvt>(1000);
	else
	{
		LOG(Logging::LL_Info, Logging::LC_PubSub, "Connected to pSub bus");

		// Subscribe to stuff
		subscribe(SUB_CFG);
		subscribe(SUB_CFG_ALIVE);
#if defined(_DEBUG) && defined(WIN32)
		subscribe(SUB_DIE);
#endif
		if (!haveCfg)
			sendMsg(PubSub::Message(PUB_ALIVE, PUB_STATUS_TTL, g_version));

		if (m_here)
			m_here->cancelMsg();
		m_here = enqueueWithDelay<evHereTime>(2000);

		m_sockptr->async_read_some(BA::buffer(readBuff, 1024), boost::bind(&Logger_Dispatcher::OnReadSome, this, BA::placeholders::error, BA::placeholders::bytes_transferred));
	}
}

void Logger_Dispatcher::OnReadSome(const boost::system::error_code& error, size_t bytes_transferred)
{
	if (!error)
	{
		processBuffer((char*)readBuff, bytes_transferred);
		m_sockptr->async_read_some(BA::buffer(readBuff, 1024), boost::bind(&Logger_Dispatcher::OnReadSome, this, BA::placeholders::error, BA::placeholders::bytes_transferred));
	}
	else
	{
		LOG(Logging::LL_Warning, Logging::LC_PubSub, "Lost connection to pSub bus");

		if (m_here)
		{
			m_here->cancelMsg();
			m_here.reset();
		}
		enqueueWithDelay<ReconnectEvt>(1000);
	}
}

//void Logger_Dispatcher::socketThread()
//{
//	sockptr.reset(new boost::asio::ip::tcp::socket(m_iosvc));
//
//	boost::system::error_code error = boost::asio::error::connection_refused;
//
//	try
//	{
//
//		while (true)
//		{
//			if (m_here)
//			{
//				// Disconnected - cancel Here timer
//				m_here->cancelMsg();
//				m_here.reset();
//			}
//
//			for (; error; std::this_thread::sleep_for(std::chrono::milliseconds(200)))
//			{
//				if (m_exitEvt.timedwait(0))
//					return;
//
//				sockptr->close();
//				sockptr->connect(m_pubsubaddr, error);
//			}
//
//			LOG(LL_Info, LC_PubSub, "Connected to pSub bus");
//
//			// Subscribe to stuff
//			subscribe(SUB_CFG);
//			subscribe(SUB_CFG_ALIVE);
//#if defined(_DEBUG) && defined(WIN32)
//			subscribe(SUB_DIE);
//#endif
//
//			sendMsg(PubSub::Message(PUB_ALIVE, PUB_STATUS_TTL, g_version));
//
//			m_here = enqueueWithDelay<evHereTime>(2000);
//
//			for (;;)
//			{
//				if (m_exitEvt.timedwait(0))
//				{
//					LOG(LL_Info, LC_PubSub, "Detected pSub thread exit event");
//					return;
//				}
//
//				char buff[1024];
//
//				try
//				{
//					size_t len = readWithTimeout(*sockptr, boost::asio::buffer(buff), boost::asio::deadline_timer::duration_type(boost::posix_time::seconds(1)));
//
//					if (len > 0)
//						processBuffer(buff, len);
//				}
//				catch (const boost::system::system_error& ex)
//				{
//					error = ex.code();
//					break;
//				}
//			}
//			LOG(LL_Warning, LC_PubSub, "Lost connection to pSub bus");
//		}
//	}
//	catch (const std::exception& ex)
//	{
//		LOG(LL_Severe, LC_Logger, ex.what());
//		throw ex;
//	}
//}

template <> void Logger_Dispatcher::processEvent<Logger_Dispatcher::evCfgAliveDeferred>()
{
	sendMsg(PubSub::Message(PUB_ALIVE, PUB_STATUS_TTL, g_version));
}

template <> void Logger_Dispatcher::processEvent<Logger_Dispatcher::evHereTime>()
{
	sendMsg(PubSub::Message(PUB_HERE, 0));
	m_here = enqueueWithDelay<evHereTime>(2000);
}

void Logger_Dispatcher::processMsg(const PubSub::Message& m)
{
	std::string str;
	LOG(Logging::LL_Debug, Logging::LC_Logger, "Received msg " << PubSub::toString(m.subject, str));


	if (PubSub::match(SUB_CFG, m.subject))
		configure(m.payload);
#if defined(_DEBUG) && defined(WIN32)
	else if (PubSub::match(SUB_DIE, m.subject))
		SetEvent(g_exitEvent);
#endif
	else if (PubSub::match(SUB_CFG_ALIVE, m.subject))
	{
		if (!haveCfg)
		{
			// There's a good chance that the system is starting and that the config service
			// will receive our cached Status/Alive message and serve up our config very soon.
			// Wait a little before issuing the Status/Alive again.

			m_cfgAliveDeferred = enqueueWithDelay<evCfgAliveDeferred>(3000);
		}
	}
	else
		// Unknown message - weird
		;
}
