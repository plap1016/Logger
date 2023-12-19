#define fopen_s(FD, FPATH, FLAGS) *FD = fopen(FPATH, FLAGS);

#include "Logger_Dispatcher.h"
#include "configuration-pimpl.hxx"
#include "syscfg-pimpl.hxx"
#include "PSubLocal.h"
#include "pugixml/pugixml.hpp"

#include <stdint.h>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <boost/optional.hpp>
#include <boost/filesystem.hpp>

#include <string>
#include <sstream>
#include <set>
#include <cstdio>
#include <regex>

namespace Logging
{
	template <> const char* getLCStr<LC_Task     >() { return "Task    "; }
	template <> const char* getLCStr<LC_PubSub   >() { return "PubSub  "; }
	template <> const char* getLCStr<LC_TcpConn  >() { return "TcpConn "; }
	template <> const char* getLCStr<LC_Local    >() { return "Local   "; }
	template <> const char* getLCStr<LC_Logger   >() { return "Logger  "; }
}

using namespace Logging;

const PubSub::Subject PUB_ALIVE{ "Alive", "Logger" };
const PubSub::Subject PUB_HERE{ "Here", "Logger" };
const PubSub::Subject PUB_DEAD{ "Dead", "Logger" };
const PubSub::Subject SUB_CFG_ALIVE{ "Alive", "CFG" };
const PubSub::Subject SUB_CFG{ "CFG", "Logger" };
const PubSub::Subject SUB_SHARED_CFG{ "CFG", "Shared" };
const PubSub::Subject PUB_CFG_REQUEST{ "CFG", "Request", "Logger" };
const PubSub::Subject PUB_SHARED_CFG_REQUEST{ "CFG", "Request", "Shared" };

const PubSub::Subject SUB_NEW_FILE{ "Logger", "Newfile" };
const PubSub::Subject SUB_FLUSH_FILE{ "Logger", "Flush" };


#if defined(_DEBUG) && defined(WIN32)
const PubSub::Subject SUB_DIE{ "Die", "Logger" };
HANDLE g_exitEvent;
#endif

extern std::string g_version;

constexpr qpc_clock::duration TTL_LONGTIME{std::chrono::hours(-12)}; // up to 12 hrs or until superseded
constexpr qpc_clock::duration TTL_STATUS{std::chrono::minutes(1)};

Logger_Dispatcher::Logger_Dispatcher(Logging::LogFile& log, const std::string& psubAddr)
	: Task::TActiveTask<Logger_Dispatcher>(2)
	, Logging::LogClient(log)
	, m_sockThread()
	, m_pubsubaddr(psubAddr)
	, m_sock(m_iosvc)
{
	m_sockThread = std::thread([=] { socketThread(); });

	getMsgDispatcher().start();
}


Logger_Dispatcher::~Logger_Dispatcher()
{
	try
	{
		LOG(LL_Debug, LC_Logger, "stop");
		sendMsg(PubSub::Message(PUB_DEAD, TTL_LONGTIME));

		getMsgDispatcher().stop();
		m_iosvc.stop();

		m_sockThread.join();
	}
	catch (const std::exception& ex)
	{
		LOG(LL_Warning, LC_Logger, "ERROR: The following errors were found:\r\n" << ex.what());
	}
}

void Logger_Dispatcher::configure(const std::string& cfgStr)
{
	loggercfg::Logger_paggr s;
	xml_schema::document_pimpl d(s.root_parser(), s.root_name());

	std::istringstream cfgstrm(cfgStr);

	s.pre();

	try
	{
		if (!haveCfg)
		{
			d.parse(cfgstrm);
			s.post()->_copy(m_cfg);

			m_local.reset(new PSubLocal(*this));
			m_local->start();

			if (m_cfg.Flush_present())
				for (const loggercfg::event_string_t& e : m_cfg.Flush().Event())
					subscribe(PubSub::parseSubject(e));

			if (m_cfg.NewFile_present())
				for (const loggercfg::event_string_t& e : m_cfg.NewFile().Event())
					subscribe(PubSub::parseSubject(e));

			if (m_cfg.FtpUpload_present())
				for (const loggercfg::event_string_t& e : m_cfg.FtpUpload().Event())
					subscribe(PubSub::parseSubject(e));
			else
				m_haveSysCfg = true; // Don't bother waiting for or requesting Shared config since we wont use it

			haveCfg = true;
		}
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

void Logger_Dispatcher::configSys(const std::string& cfgStr)
{
	syscfg::Shared_paggr cfg_p;
	xml_schema::document_pimpl d(cfg_p.root_parser(), cfg_p.root_name());

	std::stringstream cfgStrm(cfgStr);

	cfg_p.pre();

	try
	{
		d.parse(cfgStrm);
		m_syscfg = cfg_p.post();

		m_haveSysCfg = true;

	}
	catch (xml_schema::parser_exception& ex)
	{
		PubSub::Message err;
		err.subject = { "Error", "Logger", "Config", "Shared" };
		err.ttl = TTL_STATUS;
		std::stringstream s;
		s << ex.what() << ": " << ex.text() << ". line: " << ex.line() << " column: " << ex.column();
		err.payload = s.str();

		LOG(Logging::LL_Warning, Logging::LC_Logger, "SHARED CONFIG ERROR: " << err.payload);

		sendMsg(err);
	}
}

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
	std::string::size_type i = m_pubsubaddr.find(':');
	if (i == std::string::npos)
		connect(m_pubsubaddr, "3101");
	else
		connect(m_pubsubaddr.substr(0, i), m_pubsubaddr.substr(i + 1));
}

template <> void Logger_Dispatcher::processEvent<ReconnectEvt>(void)
{
	start();
}

void Logger_Dispatcher::connect(const std::string& address, const std::string& port)
{
	namespace BIP = boost::asio::ip;

	std::shared_ptr<BIP::tcp::resolver> resolver = std::make_shared<BIP::tcp::resolver>(m_iosvc);

	auto connectHandler = [this] (const boost::system::error_code& errorCode, const BIP::tcp::endpoint& ep)
	{
		if (errorCode)
			onConnectionError("Could not connect: " + errorCode.message());
		else
			onConnected(ep);
	};

	auto resolveHandler = [resolver, connectHandler, this]
		(const boost::system::error_code& errorCode, const BIP::tcp::resolver::results_type results)
	{
		if (errorCode)
			onConnectionError("Could not resolve address: " + errorCode.message());
		else
			boost::asio::async_connect(m_sock, results, connectHandler);
	};

	resolver->async_resolve(address, port, resolveHandler);
}

void Logger_Dispatcher::onConnected(const BA::ip::tcp::endpoint& ep)
{
	LOG(Logging::LL_Info, Logging::LC_PubSub, "Connected to pSub bus at " << ep);

	resetPSub();

	m_sock.set_option(boost::asio::socket_base::keep_alive(true));

	// Subscribe to stuff
	subscribe(SUB_CFG);
	subscribe(SUB_SHARED_CFG);
	subscribe(SUB_NEW_FILE);
	subscribe(SUB_FLUSH_FILE);
#if defined(_DEBUG) && defined(WIN32)
	subscribe(SUB_DIE);
#endif

	sendMsg(PubSub::Message(PUB_ALIVE, g_version, TTL_LONGTIME));

	if (!haveCfg)
		m_cfgAliveDeferred = enqueueWithDelay<evCfgDeferred>(3s);
	else
	{
		if (m_cfg.Flush_present())
			for (const loggercfg::event_string_t& e : m_cfg.Flush().Event())
				subscribe(PubSub::parseSubject(e));

		if (m_cfg.NewFile_present())
			for (const loggercfg::event_string_t& e : m_cfg.NewFile().Event())
				subscribe(PubSub::parseSubject(e));

		if (m_cfg.FtpUpload_present())
			for (const loggercfg::event_string_t& e : m_cfg.FtpUpload().Event())
				subscribe(PubSub::parseSubject(e));
	}

	if (m_here)
		m_here->cancelMsg();
	m_here = enqueueWithDelay<evHereTime>(2s, true);

	m_sock.async_read_some(BA::buffer(readBuff, 1024), [&](const boost::system::error_code& error, size_t bytes){ OnReadSome(error, bytes); });
}

void Logger_Dispatcher::onConnectionError(const std::string& error)
{
	LOG(Logging::LL_Warning, Logging::LC_PubSub, error << " Reconnect in 1 second");
	enqueueWithDelay<ReconnectEvt>(1s);
}

void Logger_Dispatcher::OnReadSome(const boost::system::error_code& error, size_t bytes_transferred)
{
	if (!error)
	{
		try
		{
			processBuffer((char*)readBuff, bytes_transferred);
		}
		catch (const std::exception& ex)
		{
			LOG(Logging::LL_Warning, Logging::LC_PubSub, "Error " << ex.what() << " processing pSub buffer. Read buffers reset");
			LOG(Logging::LL_Dump, Logging::LC_PubSub, readBuff);
		}
		m_sock.async_read_some(BA::buffer(readBuff, 1024), [&](const boost::system::error_code& error, size_t bytes){ OnReadSome(error, bytes); });
	}
	else
	{
		LOG(Logging::LL_Warning, Logging::LC_PubSub, "Lost connection to pSub bus");

		resetPSub();
		if (m_here)
		{
			m_here->cancelMsg();
			m_here.reset();
		}
		enqueueWithDelay<ReconnectEvt>(1s);
	}
}

template <> void Logger_Dispatcher::processEvent<Logger_Dispatcher::evCfgDeferred>()
{
	if (!haveCfg)
		sendMsg(PubSub::Message(PUB_CFG_REQUEST, g_version));

	if (!m_haveSysCfg)
		sendMsg(PubSub::Message(PUB_SHARED_CFG_REQUEST, 0));

	if (!(haveCfg && m_haveSysCfg))
		m_cfgAliveDeferred = enqueueWithDelay<evCfgDeferred>(3s);

}

template <> void Logger_Dispatcher::processEvent<Logger_Dispatcher::evHereTime>()
{
	sendMsg(PubSub::Message(PUB_HERE));
}

template <> void Logger_Dispatcher::processEvent<Logger_Dispatcher::evNewFile>()
{
	m_local->enqueue<NewfileEvt>();
}

template <> void Logger_Dispatcher::processEvent<Logger_Dispatcher::evNewFileCreated>()
{
	m_newFileComplete.set();
}

template <> void Logger_Dispatcher::processEvent<Logger_Dispatcher::evFlushFile>()
{
	m_local->enqueue<PSubLocal::FlushEvt>();
}

template <> void Logger_Dispatcher::processEvent<Logger_Dispatcher::evFtpUpload>()
{
	ftpUpload();
}

void Logger_Dispatcher::processMsg(const PubSub::Message& m)
{
	std::string str;
	LOG(Logging::LL_Debug, Logging::LC_Logger, "Received msg " << PubSub::toString(m.subject, str));

	std::unique_lock<std::recursive_mutex> s(m_dispLock);

	if (PubSub::match(SUB_CFG, m.subject))
		configure(m.payload);
	if (PubSub::match(SUB_SHARED_CFG, m.subject))
		configSys(m.payload);
	else if (PubSub::match(SUB_NEW_FILE, m.subject))
		m_local->enqueue<NewfileEvt>();
	else if (PubSub::match(SUB_FLUSH_FILE, m.subject))
		m_local->enqueue<PSubLocal::FlushEvt>();
#if defined(_DEBUG) && defined(WIN32)
	else if (PubSub::match(SUB_DIE, m.subject))
		SetEvent(g_exitEvent);
#endif
	else
	{
		if (m_cfg.FtpUpload_present())
			for (const loggercfg::event_string_t& e : m_cfg.FtpUpload().Event())
				if (PubSub::match(PubSub::parseSubject(e), m.subject) && matchEvent(e, m.payload))
				{
					LOG(Logging::LL_Info, Logging::LC_Logger, "Upload trigger \"" << PubSub::toString(m.subject) << "\" detected");
					ftpUpload();
					break;
				}

		if (m_cfg.Flush_present())
			for (const loggercfg::event_string_t& e : m_cfg.Flush().Event())
				if (PubSub::match(PubSub::parseSubject(e), m.subject) && matchEvent(e, m.payload))
				{
					LOG(Logging::LL_Info, Logging::LC_Logger, "Flush trigger \"" << PubSub::toString(m.subject) << "\" detected");
					m_local->enqueue<PSubLocal::FlushEvt>();
					break;
				}

		if (m_cfg.NewFile_present())
			for (const loggercfg::event_string_t& e : m_cfg.NewFile().Event())
				if (PubSub::match(PubSub::parseSubject(e), m.subject) && matchEvent(e, m.payload))
				{
					LOG(Logging::LL_Info, Logging::LC_Logger, "New file trigger \"" << PubSub::toString(m.subject) << "\" detected");
					m_local->enqueue<NewfileEvt>();
					break;
				}
	}
}

void Logger_Dispatcher::ftpUpload()
{
	// synchronous call
	m_local->enqueue<NewfileEvtSync>();
	m_newFileComplete.wait();
	BF::path currFile(m_local->currentFileName());

#ifdef WIN32
	SOCKET sock = 0;
#else
	int sock = 0;
#endif
	LIBSSH2_SESSION* session = nullptr;
	LIBSSH2_SFTP *sftp_session = nullptr;

	struct myerr
	{
		std::string msg;
};

	try
	{
		int rc;
		rc = libssh2_init(0);
		if (rc)
		{
			LOG(Logging::LL_Warning, Logging::LC_Logger, "libssh2 initialization failed " << rc);
			return;
		}

		session = libssh2_session_init();
		if (session == NULL)
		{
			LOG(Logging::LL_Warning, Logging::LC_Logger, "libssh2 session initialization failed");
			return;
		}

		sock = socket(AF_INET, SOCK_STREAM, 0);

		addrinfo hints, *res;
		int errcode;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags |= AI_CANONNAME;

		errcode = getaddrinfo(m_cfg.FtpUpload().Host().c_str(), NULL, &hints, &res);
		if (errcode != 0)
		{
			LOG(Logging::LL_Warning, Logging::LC_Logger, "getaddrinfo fail: " << errcode);
			return;
		}

		while (res)
		{
			if (res->ai_family == AF_INET)
			{

				struct sockaddr_in sin = *((struct sockaddr_in *)res->ai_addr);
				sin.sin_port = htons(22);
				if (::connect(sock, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in)) == 0)
				{
					LOG(Logging::LL_Info, Logging::LC_Logger, "Connected");
					break;
				}
			}
			res = res->ai_next;
		}
		if (!res)
		{
			std::stringstream strm; strm << "Failed to connect to " << m_cfg.FtpUpload().Host() << ":22";
			throw myerr{ strm.str() };
		}

		libssh2_session_set_blocking(session, 1);
		rc = libssh2_session_handshake(session, sock);

		if (rc)
		{
			std::stringstream strm; strm << "Failure establishing SSH session: " << rc;
			throw myerr{ strm.str() };
		}

		/* At this point we havn't yet authenticated.  The first thing to do
		* is check the hostkey's fingerprint against our known hosts Your app
		* may have it hard coded, may go to a file, may present it to the
		* user, that's your call
		*/
		//const char *fingerprint = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1);
		//fprintf(stderr, "Fingerprint: ");
		//for (i = 0; i < 20; i++)
		//{
		//	fprintf(stderr, "%02X ", (unsigned char)fingerprint[i]);
		//}
		//fprintf(stderr, "\n");

		//if (auth_pw)
		//{
		/* We could authenticate via password */
		if (libssh2_userauth_password(session, m_cfg.FtpUpload().username().c_str(), m_cfg.FtpUpload().password().c_str()))
			throw myerr{ "Authentication by username/password failed" };
		//}
		//else
		//{
		//	/* Or by public key */
		//	const char *pubkey = "/home/username/.ssh/id_rsa.pub";
		//	const char *privkey = "/home/username/.ssh/id_rsa.pub";
		//	if (libssh2_userauth_publickey_fromfile(session, username,

		//		pubkey, privkey,
		//		password))
		//	{
		//		LOG(Logging::LL_Debug, Logging::LC_Logger, "Authentication by public key failed");
		//		goto shutdown;
		//	}
		//}

		sftp_session = libssh2_sftp_init(session);
		if (!sftp_session)
			throw myerr{ "Unable to init SFTP session" };

		BF::path p(cfg().LogPath());
		std::string fnroot = cfg().FileNameRoot();
		auto fnprefix = [&]()
		{
			if (m_haveSysCfg)
			{
				if (m_syscfg.uuid_present())
					return m_syscfg.uuid() + '_';

				if (!m_syscfg.TerminalID().empty())
					return m_syscfg.TerminalID() + '_';
			}
			return std::string();
		};
		std::string destpath = m_cfg.FtpUpload().path() + '/' + fnprefix();
		for (BF::directory_entry d : BF::directory_iterator(p))
		{
			if (d.path().filename().empty())
				continue;

			std::string droot = d.path().filename().string().substr(0, fnroot.size());
			if (droot == fnroot && d.path().filename().string() != currFile.filename().string())
			{
				std::string destfname = destpath + d.path().filename().string();
				LOG(Logging::LL_Info, Logging::LC_Logger, "Uploading " << d.path().filename());

				LIBSSH2_SFTP_HANDLE *sftp_handle;
				libssh2_session_set_blocking(session, 1);

				/* Request a file via SFTP */
				sftp_handle = libssh2_sftp_open(sftp_session, destfname.c_str(),
												LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT /*| LIBSSH2_FXF_TRUNC*/,
												LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
												LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);

				if (!sftp_handle)
				{
					char* errmsg;
					libssh2_session_last_error(session, &errmsg, nullptr, 0);
					uint32_t sftperr = libssh2_sftp_last_error(sftp_session);
					LOG(Logging::LL_Warning, Logging::LC_Logger, "Unable to open " << destfname << " with SFTP. " << errmsg << " " << sftperr);
				}
				else
				{
					LIBSSH2_SFTP_ATTRIBUTES attr;
					libssh2_sftp_fstat(sftp_handle, &attr);
					libssh2_sftp_seek64(sftp_handle, attr.filesize);

					FILE *loc;
					fopen_s(&loc, d.path().string().c_str(), "rb");
					if (fseek(loc, attr.filesize, SEEK_SET))
					{
						LOG(Logging::LL_Warning, Logging::LC_Logger, "Remote file already larger than local ");
						continue;
					}

					char buff[0xFFFFu] = { 0 };
					char* ptr = 0;
					rc = -1;
					libssh2_session_set_blocking(session, 0);
					do
					{
						size_t nread = fread(buff, 1, sizeof(buff), loc);
						if (nread <= 0)
						{
							rc = nread == 0 ? 0 : rc;
							break;
						}

						do
						{
							/* write data in a loop until we block */
							rc = libssh2_sftp_write(sftp_handle, buff, nread);

							if (rc == 0 || rc == LIBSSH2_ERROR_EAGAIN) // Would have blocked or nothing sent
							{
								std::this_thread::sleep_for(500ms);
								continue;
							}

							if (rc < 0)
								break;
							ptr += rc;
							nread -= rc;
						} while (nread);

					} while (rc > 0);

					fclose(loc);
					libssh2_sftp_close(sftp_handle);

					if (rc == 0)
					{
						LOG(Logging::LL_Info, Logging::LC_Logger, "Upload success. Deleting " << d.path().filename());
						BF::remove(d.path());
					}
				}
			}
		}

		libssh2_sftp_shutdown(sftp_session);
	}
	catch (const myerr& e)
	{
		LOG(Logging::LL_Warning, Logging::LC_Logger, e.msg);
	}

#ifdef WIN32
	closesocket(sock);
#else
	close(sock);
#endif
	libssh2_session_disconnect(session, "Normal Shutdown");
	libssh2_session_free(session);
	libssh2_exit();
}

//void Logger_Dispatcher::ftpUpload()
//{
//	// synchronous call
//	m_local->enqueue<NewfileEvtSync>();
//	m_newFileComplete.wait();
//	BF::path currFile(m_local->currentFileName());
//
//	CURL *curlhandle = NULL;
//	curl_global_init(CURL_GLOBAL_ALL);
//	curlhandle = curl_easy_init();
//
//	BF::path p(cfg().LogPath());
//	std::string fnroot = cfg().FileNameRoot();
//	std::string destpath = m_cfg.FtpUpload().Host() + '/' + (m_haveSysCfg ? std::to_string(m_syscfg.TerminalID()) + '_' : "");
//	for (BF::directory_entry d : BF::directory_iterator(p))
//	{
//		if (d.path().filename().empty())
//			continue;
//
//		std::string droot = d.path().filename().string().substr(0, fnroot.size());
//		if (droot == fnroot && d.path().filename().string() != currFile.filename().string())
//		{
//			std::string destfname = destpath + d.path().filename().string();
//			LOG(Logging::LL_Info, Logging::LC_Logger, "Uploading " << d.path().filename());
//			if (upload(curlhandle, destfname, d.path().string(), 0, 3))
//			{
//				LOG(Logging::LL_Debug, Logging::LC_Logger, "Upload success. Deleting " << d.path().filename());
//				BF::remove(d.path());
//			}
//		}
//	}
//
//	curl_easy_cleanup(curlhandle);
//	curl_global_cleanup();
//}

///* parse headers for Content-Length */
//size_t getcontentlengthfunc(char *ptr, size_t size, size_t nmemb, void *stream)
//{
//	int r;
//	long len = 0;
//
//#ifdef WIN32
//	r = sscanf_s(ptr, "Content-Length: %ld\n", &len);
//#else
//	r = std::sscanf(ptr, "Content-Length: %ld\n", &len);
//#endif
//	if (r)
//		*((long *)stream) = len;
//
//	return size * nmemb;
//}
//
//curl_off_t Logger_Dispatcher::sftpGetRemoteFileSize(const char *i_remoteFile)
//{
//	CURLcode result = CURLE_GOT_NOTHING;
//	curl_off_t remoteFileSizeByte = -1;
//	CURL *curlHandlePtr = NULL;
//
//	curlHandlePtr = curl_easy_init();
//	curl_easy_setopt(curlHandlePtr, CURLOPT_VERBOSE, 1L);
//
//	curl_easy_setopt(curlHandlePtr, CURLOPT_URL, i_remoteFile);
//	curl_easy_setopt(curlHandlePtr, CURLOPT_NOBODY, 1);
//	curl_easy_setopt(curlHandlePtr, CURLOPT_HEADER, 1);
//	curl_easy_setopt(curlHandlePtr, CURLOPT_FILETIME, 1);
//	curl_easy_setopt(curlHandlePtr, CURLOPT_NOPROGRESS, 1);
//
//	result = curl_easy_perform(curlHandlePtr);
//	if (CURLE_OK == result)
//	{
//#if LIBCURL_VERSION_NUM >= 0x073700
//		result = curl_easy_getinfo(curlHandlePtr, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &remoteFileSizeByte);
//#else
//		double fs = 0.0;
//		result = curl_easy_getinfo(curlHandlePtr, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &fs);
//		remoteFileSizeByte = (curl_off_t)fs;
//#endif
//		if (result)
//			return -1;
//		LOG(Logging::LL_Debug, Logging::LC_Logger, "Remote file size = " << remoteFileSizeByte);
//	}
//	curl_easy_cleanup(curlHandlePtr);
//
//	return remoteFileSizeByte;
//}
//
///* discard downloaded data */
//size_t discardfunc(char *ptr, size_t size, size_t nmemb, void *stream)
//{
//	(void)ptr;
//	(void)stream;
//	return size * nmemb;
//}
//
///* read data to upload */
//size_t readfunc(char *ptr, size_t size, size_t nmemb, void *stream)
//{
//	std::ifstream* f = (std::ifstream*)stream;
//
//	if (!*f)
//		return CURL_READFUNC_ABORT;
//
//	f->read(ptr, size * nmemb);
//
//	return (size_t)f->gcount();
//}
//
//bool Logger_Dispatcher::sftpResumeUpload(CURL *curlhandle, const std::string& remotepath, const std::string& localpath)
//{
//	std::ifstream f;
//	CURLcode result = CURLE_GOT_NOTHING;
//
//	curl_off_t remoteFileSizeByte = sftpGetRemoteFileSize(remotepath.c_str());
//
//	f.open(localpath.c_str(), std::ios_base::binary | std::ios_base::in);
//	if (!f.good())
//	{
//		perror(NULL);
//		return false;
//	}
//
//	curl_easy_setopt(curlhandle, CURLOPT_VERBOSE, 0L);
//	curl_easy_setopt(curlhandle, CURLOPT_UPLOAD, 1L);
//	curl_easy_setopt(curlhandle, CURLOPT_URL, remotepath.c_str());
//	curl_easy_setopt(curlhandle, CURLOPT_READFUNCTION, readfunc);
//	curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, discardfunc);
//	curl_easy_setopt(curlhandle, CURLOPT_READDATA, &f);
//	curl_easy_setopt(curlhandle, CURLOPT_FTP_CREATE_MISSING_DIRS, 1L);
//
//	if (remoteFileSizeByte > 0)
//	{
//		f.seekg(remoteFileSizeByte, std::ios_base::beg);
//		curl_easy_setopt(curlhandle, CURLOPT_APPEND, 1L);
//	}
//	else
//		curl_easy_setopt(curlhandle, CURLOPT_APPEND, 0L);
//
//	result = curl_easy_perform(curlhandle);
//
//	f.close();
//
//	if (result != CURLE_OK)
//		LOG(Logging::LL_Debug, Logging::LC_Logger, "Upload fail - " << curl_easy_strerror(result));
//
//	return result == CURLE_OK;
//}
//
//bool Logger_Dispatcher::upload(CURL *curlhandle, const std::string& remotepath, const std::string& localpath, long timeout, long tries)
//{
//	int c;
//	for (c = 0; c < tries; ++c)
//		if (sftpResumeUpload(curlhandle, remotepath, localpath))
//			break;
//
//	return c < tries;
//}

bool Logger_Dispatcher::matchEvent(const loggercfg::event_string_t& ev, const std::string& payload)
{
	pugi::xpath_value_type xPathType = pugi::xpath_type_string;
	bool found = true;
	std::string foundText;

	if (ev.xpath_present())
	{
		pugi::xml_document doc;
		pugi::xml_parse_result r = doc.load_string(payload.c_str());
		if (r.status != pugi::xml_parse_status::status_ok)
		{
			LOG(Logging::LL_Warning, Logging::LC_Logger, "Payload for event " << ev << " not valid XML");
			return false;
		}

		try
		{
			pugi::xpath_query xp(ev.xpath().c_str());
			xPathType = xp.return_type();
			switch (xPathType)
			{
			case pugi::xpath_type_node_set:
				found = !xp.evaluate_node_set(doc).empty();
				break;
			case pugi::xpath_type_number:
				found = xp.evaluate_number(doc) != 0.0;
				break;
			case pugi::xpath_type_string:
				foundText = xp.evaluate_string(doc);
				found = !foundText.empty();
				break;
			case pugi::xpath_type_boolean:
				found = xp.evaluate_boolean(doc);
				break;
			case pugi::xpath_type_none:// Unknown type (query failed to compile)
			default:
				LOG(Logging::LL_Warning, Logging::LC_Logger, "Xpath for event " << ev << " not valid");
				found = false;
				break;
			}
		}
		catch (const pugi::xpath_exception& ex)
		{
			std::cerr << ex.result().description();
			return 1;
		}
	}

	if (ev.regex_present() && found && xPathType == pugi::xpath_type_string)
	{
		std::regex rg(ev.regex());
		found = std::regex_search(ev.xpath_present() ? foundText : payload, rg);
	}

	return found;
}
