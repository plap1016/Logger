#include "Misc/signals.h"
#include "Logging/Log.h"
#include "Logger/Logger_Dispatcher.h"
#include "Task/lock.h"

#define DAEMON_NAME "ccmloggerd"

namespace Logging
{
	const uint32_t LC_Service = 0x0200;
	template <> const char* getLCStr<LC_Service  >() { return "Service "; }
}

void usage();
bool parseCmdLine(int argc, char *argv[]);

bool g_exe{false};
std::string g_psubaddr("127.0.0.1");
std::string g_version = "1.1.8";

std::string logfilen{DAEMON_NAME ".log"};
Logging::LogFile logfile, *plogfile(&logfile);
VEvent stopEvent;

int main(int argc, char* argv[])
{
	logfilen = DAEMON_NAME ".log";

	if (!parseCmdLine(argc, argv))
		return -1;

	/* Debug logging
	setlogmask(LOG_UPTO(LOG_DEBUG));
	openlog(DAEMON_NAME, LOG_CONS, LOG_USER);
	*/

	if (!g_exe)
	{
		/* Logging */
		setlogmask(LOG_UPTO(LOG_INFO));
		openlog(DAEMON_NAME, LOG_CONS | LOG_PERROR, LOG_USER);

		syslog(LOG_INFO, "Daemon starting up");

		/* Deamonize */
		daemonize("/tmp/", "/tmp/" DAEMON_NAME ".pid");

		syslog(LOG_INFO, "Daemon running");
	}

	signalSetup();

	if (!logfilen.empty())
		logfile.open(logfilen);

	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "************************************ STARTUP ****************************************");
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Executable: " << argv[0]);
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Version: " << g_version);
	for (int x = 1; x < argc; ++x)
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Command line parameter: " << argv[x]);
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "*************************************************************************************");

	{
		Logger_Dispatcher disp(logfile, g_psubaddr);

		while(!stopEvent.timedwait(10000))
			syslog(LOG_INFO, DAEMON_NAME " alive");

		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "main loop finished. Stopping dispatcher");
	}

	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Shut down complete.  Exit");
	syslog(LOG_INFO, "Shut down complete.  Exit");
	exit(0);
}

bool parseCmdLine(int argc, char *argv[])
{
	bool ret(true);

	if (argc < 1)
	{
		std::cout << "Invalid command line parameters" << std::endl;
		usage();
		return false;
	}

	plogfile->setLogLevel(Logging::LLSet_Info);
	plogfile->setMaxFiles(5);
	plogfile->setSizeLimit(0x00A00000); // 10 Mb
	for (int x = 1; x < argc; ++x)
	{
		if (argv[x][0] == '-')
		{
			// an option
			int optlen = strlen(argv[x]);
			for (int y = 1; y < optlen; ++y)
			{
				switch (argv[x][y])
				{
				case 'h':
					usage();
					return false;
				case 'd':
					plogfile->setLogLevel(Logging::LLSet_Debug);
					break;
				case 'm':
					plogfile->setLogLevel(Logging::LLSet_Dump);
					break;
				case 't':
					plogfile->setLogLevel(Logging::LLSet_Trace);
					break;
				case 'T':
					plogfile->setLogLevel(Logging::LLSet_Test);
					break;
				case 'e': // run as exe
					g_exe = true;
					break;
				case 'b':
					if (y == optlen - 1 && ++x < argc && argv[x][0] != '-')
						g_psubaddr = argv[x];
					else
					{
						std::cout << "Invalid command line parameters" << std::endl;
						usage();
						return false;
					}
					break;
				case 'l': // specify log file
					if (y == optlen - 1 && ++x < argc && argv[x][0] != '-')
						logfilen = argv[x];
					else
					{
						std::cout << "Invalid command line parameters" << std::endl;
						usage();
						return false;
					}
					break;
				default:
					std::cout << "Invalid command line parameters" << std::endl;
					usage();
					return false;
				}
			}
		}
	}

	return ret;
}

void usage()
{
	using namespace std;
	cout << "loggerd - Central Management Computer PubSub bus Logging service" << endl;
	cout << "Usage: loggerd [OPTIONS]" << endl;
	cout << "Options:" << endl;
	cout << "\t-h - help. Print this message and exit" << endl;
	cout << "\t     If this option is used any subsequent options are ignored" << endl;
	cout << "\t-d - debug. Sets logging level to DEBUG." << endl;
	cout << "\t-t - trace. Sets logging level to TRACE." << endl;
	cout << "\t-T - test. Sets logging level to TEST." << endl;
	cout << "\t-m - dump. Sets logging level to DUMP." << endl;
	cout << "\t-e - exe. Runs as executable rather than a daemon." << endl;
	cout << "\t-b <ip address> - bus address. Specifies the address of the psub server to connect to" << endl;
	cout << "\t     If this option is not used the default will be the local host 127.0.0.1" << endl;
	cout << "\t-l <log file> - log. Specifies the log file to produce." << endl;
	cout << endl;
	cout << "Multiple options can be grouped together e.g. -de sets logging level to debug and runs as an executable" << endl;
	cout << "Options that require a value (l, b) must be at the end of an option group" << endl;
	cout << "\te.g.  loggerd -el loggerd.log  will work but" << endl;
	cout << "\t      loggerd -le loggerd.log  will fail" << endl;
	cout << endl;
	cout << "Multiple option groups can be listed e.g.  -el loggerd.log -e" << endl;
	cout << endl;
	cout << "Note: When setting logging levels (-d, -t) the last option listed will" << endl;
	cout << "override any previously set logging levels" << endl;
	cout << "\te.g.  -dt ignores the 'd' and sets the logging level to TRACE" << endl;
	cout << "\t      -td ignores the 't' and sets the logging level to DEBUG" << endl;
}
