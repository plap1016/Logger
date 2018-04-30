#pragma once

#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <iomanip>
#include <locale>
#include <string>
#include <stdlib.h>
#include <sys/timeb.h>
#include <time.h>

#include <boost/thread.hpp>

namespace Logging
{
	enum LogEntryLevel
	{
		LL_Severe = 0x01,
		LL_Warning = 0x02,
		LL_Info = 0x04,
		LL_Debug = 0x08,
		LL_Dump = 0x10,
		LL_Trace = 0x20,
		LL_Test = 0x40
	};

	enum LogLevel
	{
		LLSet_Severe = LL_Severe,
		LLSet_Warning = LLSet_Severe | LL_Warning,
		LLSet_Info = LLSet_Warning | LL_Info,
		LLSet_Debug = LLSet_Info | LL_Debug,
		LLSet_Dump = LLSet_Debug | LL_Dump,
		LLSet_Trace = LLSet_Dump | LL_Trace,
		LLSet_Test = LLSet_Trace | LL_Test
	};

	template <enum LogEntryLevel L> const char* getLLStr();
	template <> const char* getLLStr<LL_Severe>();
	template <> const char* getLLStr<LL_Warning>();
	template <> const char* getLLStr<LL_Info>();
	template <> const char* getLLStr<LL_Trace>();
	template <> const char* getLLStr<LL_Debug>();
	template <> const char* getLLStr<LL_Dump>();
	template <> const char* getLLStr<LL_Test>();

	enum LogComponent
	{
		LC_Task     = 0x0001,
		LC_PubSub   = 0x0002,
		LC_TcpConn  = 0x0004,
		LC_Local    = 0x0008,
		LC_Logger   = 0x0010,
		LC_Service  = 0x0200
	};

	template <enum LogComponent C> const char* getLCStr();
	template <> const char* getLCStr<LC_Task    >();
	template <> const char* getLCStr<LC_PubSub  >();
	template <> const char* getLCStr<LC_TcpConn >();
	template <> const char* getLCStr<LC_Local   >();
	template <> const char* getLCStr<LC_Logger  >();
	template <> const char* getLCStr<LC_Service >();

	class LogFile;
	class LogClient
	{
	protected:
		LogFile* m_log;
	public:
		LogClient(LogFile& log) : m_log(&log) {}
		void setLogger(LogFile& log) { m_log = &log; }
		LogFile* getLogger(void) const { return m_log; }
		operator LogFile*() { return m_log; }
	};

	class LogFile
	{
		const static unsigned int HEX_DUMP_WIDTH = 16U;
		std::ofstream m_file;
		LogLevel m_llevel;
		unsigned int m_lmask;

		boost::mutex m_lk;

	protected:
		// Pointer to this to conform to LogClient interface rather than deriving from LogClient
		// and exposing setLogger
		LogFile* m_log;

	public:

		LogFile(void) : m_llevel(LLSet_Debug), m_lmask(0xFFFF), m_log(this)
		{
		}

		void open(const std::string& fname)
		{
			if (m_file.is_open())
				m_file.close();

			m_file.open(fname.c_str(), std::ios_base::out | std::ios_base::app);
		}

		~LogFile(void)
		{
		}

		LogFile* getLogger() { return this; }

		void setLogLevel(LogLevel level) { boost::unique_lock<boost::mutex> syn(m_lk); m_llevel = level; }
		LogLevel getLogLevel(void) const { return m_llevel; }
		void setLogMask(int mask) { boost::unique_lock<boost::mutex> syn(m_lk); m_lmask = mask; }
		int getLogMask(void) const { return m_lmask; }
		bool isOpen(void) const { return m_file.is_open(); }

		template <enum LogEntryLevel L> void printMsg(const std::string& msg)
		{
			m_file << msg << std::endl;
		}
		//template <> void printMsg<LL_Dump>(const std::string& msg);

		template <enum LogEntryLevel L, enum LogComponent C> void logOut(const std::string& msg, const char* file, int ln)
		{
			boost::unique_lock<boost::mutex> syn(m_lk);

			std::string sfp(file);

#if defined(WIN32)
			std::string::size_type ri = sfp.rfind('\\');
			std::string sfn;
			if (ri != std::string::npos)
				sfn = sfp.substr(ri);
			else
				sfn = sfp;
#else
			std::string sfn = sfp.substr(sfp.rfind('/'));
#endif // defined
			timeb now;
			ftime(&now);
			char tmbuff[16];
#ifdef WIN32
			struct tm t;
			localtime_s(&t, &now.time);
#else
			struct tm t = *localtime(&now.time);
#endif
			strftime(tmbuff, 16, "%Y%m%d%H%M%S", &t);
			m_file << tmbuff << "." << std::dec << std::setw(3) << std::setfill('0') << std::right << now.millitm << " :[" << getLLStr<L>() << ", " << getLCStr<C>() << "] " << sfn.c_str() << ":" << ln << " ";
			m_file << std::setw(0) << std::setfill(' ') << std::left;

			printMsg<L>(msg);

#if defined(_DEBUG) && defined(WIN32)
			OutputDebugStringA(getLLStr<L>());
			OutputDebugStringA(" ");
			OutputDebugStringA(getLCStr<C>());
			OutputDebugStringA(" ");
			OutputDebugStringA(msg.c_str());
			OutputDebugStringA("\n");
#endif
		};
	};

	template <> void LogFile::printMsg<LL_Dump>(const std::string& msg);

	unsigned char getComponentMask(const char* descr, unsigned char m = 0x00);

} //namespace Logging

#define LOG(LEVEL, COMP, MSG) \
	do{ \
		if (m_log->isOpen() && LEVEL & m_log->getLogLevel() && COMP & m_log->getLogMask()) \
		{ \
			std::ostringstream s; \
			s << MSG; \
			m_log->logOut<LEVEL,COMP>(s.str(), __FILE__, __LINE__); \
		} \
	} while(0) \

#define LOGW(LEVEL, COMP, MSG) \
	do{ \
		if (m_log->isOpen() && LEVEL & m_log->getLogLevel() && COMP & m_log->getLogMask()) \
		{ \
			std::wostringstream s; \
			s << MSG; \
			m_log->logOut<LEVEL,COMP>(std::string(W2CA(s.str().c_str())), __FILE__, __LINE__); \
		} \
	} while(0) \

#define LOG2(LEVEL, COMP, MSG, FILE, LINE) \
	do{ \
		if (m_log->isOpen() && LEVEL & m_log->getLogLevel() && COMP & m_log->getLogMask()) \
		{ \
			std::ostringstream s; \
			s << MSG; \
			m_log->logOut<LEVEL,COMP>(s.str(), FILE, LINE); \
		} \
	} while(0) \

#define LOGTO(LOGGER, LEVEL, COMP, MSG) \
	do{ \
		if (LOGGER && LOGGER->getLogger()->isOpen() && LEVEL & LOGGER->getLogger()->getLogLevel() && COMP & LOGGER->getLogger()->getLogMask()) \
		{ \
			std::ostringstream s; \
			s << MSG; \
			LOGGER->getLogger()->logOut< LEVEL, COMP >(s.str(), __FILE__, __LINE__); \
		} \
	} while(0) \

#define LOGWTO(LOGGER, LEVEL, COMP, MSG) \
	do{ \
		if (LOGGER && LOGGER->getLogger()->isOpen() && LEVEL & LOGGER->getLogger()->getLogLevel() && COMP & LOGGER->getLogger()->getLogMask()) \
		{ \
			std::wostringstream s; \
			s << MSG; \
			LOGGER->getLogger()->logOut<LEVEL,COMP>(std::string(W2CA(s.str().c_str())), __FILE__, __LINE__); \
		} \
	} while(0) \

// Macros so g_log need not be seen in other source files
#define LOGISOPEN isOpen()
#define LOGOPEN(FNAME) open(FNAME)
#define LOGLEVELSET(LEVEL) setLogLevel(LEVEL)
#define LOGLEVELGET getLogLevel()
#define LOGMASKSET(LEVEL) setLogMask(LEVEL)
#define LOGMASKGET getLogMask()

