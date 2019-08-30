#ifdef WIN32
#include "stdafx.h"
#endif
#include "PSubLocal.h"
#include "Logger_Dispatcher.h"
#include "configuration.hxx"

#include <stdint.h>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/bind.hpp>
#include <string>
#include <sstream>
#include <set>

namespace BA = boost::asio;
namespace BF = boost::filesystem;

using namespace Logging;

PSubLocal::PSubLocal(Logger_Dispatcher& disp)
	: Task::TTask<PSubLocal>(disp.getMsgDispatcher())
	, Logging::LogClient(disp)
	, m_disp(disp)
	, m_sock(disp.iosvc())
	, m_running(false)
{
}


PSubLocal::~PSubLocal()
{
}

template <> void PSubLocal::processEvent<ReconnectEvt>(void)
{
	if (m_running)
		start();
}

template <> void PSubLocal::processEvent<NewfileEvt>(void)
{
	std::unique_lock<std::recursive_mutex> s(m_lk);
	initNewFile();
}

template <> void PSubLocal::processEvent<NewfileEvtSync>(void)
{
	std::unique_lock<std::recursive_mutex> s(m_lk);
	initNewFile();
	m_disp.enqueue<Logger_Dispatcher::evNewFileCreated>();
}

template <> void PSubLocal::processEvent<PSubLocal::FlushEvt>(void)
{
	std::unique_lock<std::recursive_mutex> s(m_lk);
	m_strm.rdbuf()->syncflush();
}

void PSubLocal::OnReadSome(const boost::system::error_code& error, size_t bytes_transferred)
{
	if (!error)
	{
		try
		{
			processBuffer((char*)readBuff, bytes_transferred);
		}
		catch (const std::exception& ex)
		{
			LOG(LL_Warning, LC_Local, "Error processing pSub buffer. Read buffers reset");
			LOG(LL_Dump, LC_Local, readBuff);
		}
		m_sock.async_read_some(boost::asio::buffer(readBuff, 1024), boost::bind(&PSubLocal::OnReadSome, this, BA::placeholders::error, BA::placeholders::bytes_transferred));
	}
	else
	{
		LOG(LL_Warning, LC_Local, "Lost local connection to pSub bus");

		resetPSub();
		if (error != BA::error::operation_aborted)
		{
			if (m_reconectMsg)
				m_reconectMsg->cancelMsg();
			m_reconectMsg = enqueueWithDelay<ReconnectEvt>(1000);
		}
	}
}

void PSubLocal::OnConnect(const boost::system::error_code& error)
{
	if (error == BA::error::already_connected)
		LOG(LL_Info, LC_Local, "OnConnect error already_connected");
	else if (error)
	{
		if (m_flushMsg)
		{
			m_flushMsg->cancelMsg();
			m_flushMsg.reset();
		}

		enqueueWithDelay<ReconnectEvt>(1000);
	}
	else
	{
		LOG(LL_Info, LC_Local, "Connected to pSub bus");

		initNewFile();
		subscribe({ "*" });

		m_sock.async_read_some(BA::buffer(readBuff, 1024), boost::bind(&PSubLocal::OnReadSome, this, BA::placeholders::error, BA::placeholders::bytes_transferred));
	}
}

bool PSubLocal::initNewFile(void)
{
	if (m_strm.good())
		m_strm.close();

	uint32_t fcnt = 0;
	BF::path p(m_disp.cfg().LogPath());
	std::set<BF::path> dir;
	std::string fnroot = m_disp.cfg().FileNameRoot();
	for (BF::directory_entry d : BF::directory_iterator(p))
	{
		std::string droot = d.path().filename().string().substr(0, fnroot.size());
		if (droot == fnroot)
		{
			++fcnt;
			dir.insert(d);
		}
	}

	if (fcnt >= m_disp.cfg().MaxFileCount())
	{
		for (uint32_t x = m_disp.cfg().MaxFileCount(); x <= fcnt; ++x)
		{
			BF::remove(*dir.begin());
			dir.erase(dir.begin());
		}
	}

	std::chrono::system_clock::time_point mk = std::chrono::system_clock::now();
	std::chrono::system_clock::time_point nowsec = std::chrono::time_point_cast<std::chrono::seconds>(mk);

	std::time_t tt = std::chrono::system_clock::to_time_t(mk);
#if defined(WIN32)
	tm t;
	gmtime_s(&t, &tt);
#else
	tm t = *gmtime(&tt);
#endif

	std::stringstream fname;
	fname << m_disp.cfg().LogPath() << "/" << m_disp.cfg().FileNameRoot() << "_"
		<< std::put_time(&t, "%Y%m%d%H%M%S") << "." << std::chrono::duration_cast<std::chrono::milliseconds>(mk - nowsec).count()
		<< ".rec.gz";

	m_fname = fname.str();
	m_strm.open(m_fname.c_str());
	if (m_strm.good())
		m_strm << "START " << std::put_time(&t, "%Y%m%d%H%M%S") << "." << std::chrono::duration_cast<std::chrono::milliseconds>(mk - nowsec).count() << std::endl;

	m_start_time = m_time_marker = std::chrono::steady_clock::now();

	if (m_disp.cfg().FlushSec_present() && m_disp.cfg().FlushSec() > 0)
		m_flushMsg = enqueueWithDelay<FlushEvt>(m_disp.cfg().FlushSec(), true);

	LOG(LL_Info, LC_Local, "Created new log file " << m_fname);
	return m_strm.good();
}

void PSubLocal::start()
{
	LOG(LL_Debug, LC_Local, "start");
	std::unique_lock<std::recursive_mutex> s(m_lk);
	if (m_running)
		return;

	m_running = true;

	m_sock.close();
	m_sock.async_connect(m_disp.pSubAddr(), boost::bind(&PSubLocal::OnConnect, this, BA::placeholders::error));
}

void PSubLocal::stop()
{
	LOG(LL_Debug, LC_Local, "stop");

	std::unique_lock<std::recursive_mutex> s(m_lk);

	if (m_running)
		m_sock.close();

	if (m_strm.good())
		m_strm.close();

	m_running = false;
}

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>

using namespace boost::archive::iterators;

void PSubLocal::processMsg(const PubSub::Message& m)
{
	std::string str;
	LOG(LL_Debug, LC_Local, "Received msg " << PubSub::toString(m.subject, str));
	std::unique_lock<std::recursive_mutex> s(m_lk);

	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	std::chrono::milliseconds tdiff1 = std::chrono::duration_cast<std::chrono::milliseconds>(m_time_marker - m_start_time);
	std::chrono::milliseconds tdiff2 = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start_time);
	std::chrono::milliseconds tdiff3 = std::chrono::duration_cast<std::chrono::milliseconds>(tdiff2 - tdiff1);
	m_time_marker = now;

	//typedef transform_width< binary_from_base64<std::string::const_iterator>, 8, 6 > it_binary_t;
	typedef base64_from_binary<transform_width<std::string::const_iterator, 6, 8> > it_base64_t;

	// Encode
	unsigned int writePaddChars = (3 - m.payload.length() % 3) % 3;
	std::string base64(it_base64_t(m.payload.begin()), it_base64_t(m.payload.end()));
	base64.append(writePaddChars, '=');

	m_strm << tdiff3.count() << " " << m.age << " " << m.ttl << " ";

	for (uint32_t p : m.postmarks)
		m_strm << p << " ";

	m_strm << PubSub::toString(m.subject) << " " << base64 << std::endl;

	if (m_disp.cfg().MaxFileEventCount_present() && ++m_evtCount >= m_disp.cfg().MaxFileEventCount())
	{
		initNewFile();
		m_evtCount = 0;
	}

	//cout << "Base64 representation: " << base64 << endl;

	//// Decode
	//unsigned int paddChars = count(base64.begin(), base64.end(), '=');
	//std::replace(base64.begin(), base64.end(), '=', 'A'); // replace '=' by base64 encoding of '\0'
	//string result(it_binary_t(base64.begin()), it_binary_t(base64.end())); // decode
	//result.erase(result.end() - paddChars, result.end());  // erase padding '\0' characters
	//cout << "Decoded: " << result << endl;
	//return 0;
}
