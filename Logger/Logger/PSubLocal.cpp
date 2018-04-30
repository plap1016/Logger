#ifdef WIN32
#include "stdafx.h"
#endif
#include "PSubLocal.h"
#include "Logger_Dispatcher.h"
#include "configuration.hxx"

#include <stdint.h>
#include <boost/asio.hpp>
#include <string>
#include <sstream>

namespace BA = boost::asio;

using namespace Logging;

PSubLocal::PSubLocal(Logger_Dispatcher& disp)
	: Task::TTask<PSubLocal>(disp.getMsgDispatcher())
	, Logging::LogClient(disp)
	, m_disp(disp)
	, m_running(false)
	, m_sock(disp.iosvc())
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

void PSubLocal::OnReadSome(const boost::system::error_code& error, size_t bytes_transferred)
{
	if (!error)
	{
		processBuffer((char*)readBuff, bytes_transferred);
		m_sock.async_read_some(boost::asio::buffer(readBuff, 1024), boost::bind(&PSubLocal::OnReadSome, this, BA::placeholders::error, BA::placeholders::bytes_transferred));
	}
	else
	{
		LOG(LL_Warning, LC_Local, "Lost local connection to pSub bus");

		if (error != BA::error::operation_aborted)
			if (m_reconectMsg)
				m_reconectMsg->cancelMsg();
		m_reconectMsg = enqueueWithDelay<ReconnectEvt>(1000);
	}
}

void PSubLocal::OnConnect(const boost::system::error_code& error)
{
	if (error)
		enqueueWithDelay<ReconnectEvt>(1000);
	else
	{
		subscribe({ "*" });

		m_sock.async_read_some(BA::buffer(readBuff, 1024), boost::bind(&PSubLocal::OnReadSome, this, BA::placeholders::error, BA::placeholders::bytes_transferred));
	}
}

bool PSubLocal::initNewFile(void)
{
	if (m_strm.good())
		m_strm.close();

	m_time_marker = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point nowsec = std::chrono::time_point_cast<std::chrono::seconds>(m_time_marker);

	std::time_t tt = std::chrono::steady_clock::to_time_t(m_time_marker);
#if defined(WIN32)
	tm t;
	gmtime_s(&t, &tt);
#else
	tm t = *gmtime(&tt);
#endif

	std::stringstream fname;
	fname << m_disp.cfg().FileNameRoot() << "_" << std::put_time(&t, "%Y%m%d%H%M%S") << "." << std::chrono::duration_cast<std::chrono::milliseconds>(m_time_marker - nowsec).count() << ".zrec";

	m_strm.open(fname.str().c_str());
	if (m_strm.good())
		m_strm << "START " << std::put_time(&t, "%Y%m%d%H%M%S") << "." << std::chrono::duration_cast<std::chrono::milliseconds>(m_time_marker - nowsec).count() << std::endl;

	return m_strm.good();
}

void PSubLocal::start()
{
	LOG(LL_Debug, LC_Local, "start");
	std::unique_lock<std::recursive_mutex> s(m_lk);
	m_running = true;

	if (initNewFile())
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
	std::chrono::milliseconds tdiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_time_marker);
	m_time_marker = now;

	typedef transform_width< binary_from_base64<std::string::const_iterator>, 8, 6 > it_binary_t;
	typedef base64_from_binary<transform_width<std::string::const_iterator, 6, 8> > it_base64_t;

	// Encode
	unsigned int writePaddChars = (3 - m.payload.length() % 3) % 3;
	std::string base64(it_base64_t(m.payload.begin()), it_base64_t(m.payload.end()));
	base64.append(writePaddChars, '=');

	m_strm << tdiff.count() << " " << m.ttl << " 0 " << PubSub::toString(m.subject) << " " << base64 << std::endl;

	if (m_disp.cfg().MaxEventCount_present() && ++m_evtCount >= m_disp.cfg().MaxEventCount())
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
