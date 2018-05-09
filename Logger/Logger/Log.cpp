#ifdef WIN32
#include "stdafx.h"
#endif // WIN32
#include "Log.h"

#include <cstring>

namespace Logging
{
	//LogFile g_log;

	template <enum LogEntryLevel L> const char* getLLStr()	{ return "DEFAULT"; }
	template <> const char* getLLStr<LL_Severe>()		{ return "SEVERE "; }
	template <> const char* getLLStr<LL_Warning>()		{ return "WARNING"; }
	template <> const char* getLLStr<LL_Info>()			{ return "INFO   "; }
	template <> const char* getLLStr<LL_Trace>()		{ return "TRACE  "; }
	template <> const char* getLLStr<LL_Debug>()		{ return "DEBUG  "; }
	template <> const char* getLLStr<LL_Dump>()			{ return "DUMP   "; }
	template <> const char* getLLStr<LL_Test>()			{ return "TEST"; }

	template <enum LogComponent C> const char* getLCStr()		{ return "GLOBAL     "; }
	template <> const char* getLCStr<LC_Task     >() { return "Task    "; }
	template <> const char* getLCStr<LC_PubSub   >() { return "PubSub  "; }
	template <> const char* getLCStr<LC_TcpConn  >() { return "TcpConn "; }
	template <> const char* getLCStr<LC_Local    >() { return "Local   "; }
	template <> const char* getLCStr<LC_Logger   >() { return "Logger  "; }
	template <> const char* getLCStr<LC_Service  >() { return "Service "; }

	unsigned char getComponentMask(const char* descr, unsigned char m)
	{
		if (0 == strcmp(descr, getLCStr<LC_Task     >()))      return m | LC_Task;
		else if (0 == strcmp(descr, getLCStr<LC_PubSub   >())) return m | LC_PubSub;
		else if (0 == strcmp(descr, getLCStr<LC_TcpConn  >())) return m | LC_TcpConn;
		else if (0 == strcmp(descr, getLCStr<LC_Local    >())) return m | LC_Local;
		else if (0 == strcmp(descr, getLCStr<LC_Logger   >())) return m | LC_Logger;
		else if (0 == strcmp(descr, getLCStr<LC_Service  >())) return m | LC_Service;
		else
			return m;
	}

	template <> void LogFile::printMsg<LL_Dump>(const std::string& msg)
	{
		// Hex buffer dump format
		unsigned int msgsz(msg.size());
		m_file << "Dumping buffer contents (" << msgsz << " bytes):" << std::endl;

		unsigned char *d = (unsigned char*)msg.c_str();

		for (unsigned int ln = 0; ln * HEX_DUMP_WIDTH < msgsz; ++ln)
		{
			std::ostringstream h, s;
			h << std::setfill('0') << std::hex << std::uppercase;
			for (unsigned int x = 0; x < HEX_DUMP_WIDTH && (ln * HEX_DUMP_WIDTH + x) < msgsz; ++x)
			{
				unsigned int n = d[ln * HEX_DUMP_WIDTH + x];
				char c = d[ln * HEX_DUMP_WIDTH + x];
				h << ' ' << std::setw(2) << n;
				s << (std::iscntrl(c, std::locale::classic()) ? '.' : c);
				if ((x + 1) % 4 == 0)
					h << ' ';
			}

			m_file << std::setw(5) << std::hex << std::uppercase << std::right << HEX_DUMP_WIDTH * ln;
			m_file << "  " << std::setw(HEX_DUMP_WIDTH * 3 + HEX_DUMP_WIDTH / 4) << std::left << h.str() << "  " << s.str() << std::endl;
		}
	}
}

