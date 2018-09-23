#include <stdio.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "Logging/Log.h"
#include "Logger/Logger_Dispatcher.h"
#include "Task/lock.h"

#define DAEMON_NAME "loggerd"

namespace Logging
{
	const uint32_t LC_Service = 0x0200;
	template <> const char* getLCStr<LC_Service  >() { return "Service "; }
}

void daemonShutdown();
void signal_handler(int sig);
void daemonize(char *rundir, char *pidfile);
void usage();
bool parseCmdLine(int argc, char *argv[]);

std::string g_psubaddr("127.0.0.1");
std::string g_version = "1.1.8";
//std::string g_name;
bool g_exe(false);

int pidFilehandle;
std::string logfilen = DAEMON_NAME ".log";
Logging::LogFile logfile, *plogfile(&logfile);
VEvent stopEvent;

void signal_handler(int sig)
{
	switch(sig)
	{
	case SIGHUP:
		syslog(LOG_WARNING, "Received SIGHUP signal.");
		break;
	case SIGINT:
	case SIGTERM:
		syslog(LOG_INFO, "Daemon exiting");
		daemonShutdown();
		exit(EXIT_SUCCESS);
		break;
	default:
		syslog(LOG_WARNING, "Unhandled signal %s", strsignal(sig));
		break;
	}
}

void daemonShutdown()
{
	stopEvent.set();
	close(pidFilehandle);
}

void daemonize(const char *rundir, const char *pidfile)
{
	int pid, sid, i;
	char str[10];
	struct sigaction newSigAction;
	sigset_t newSigSet;

	/* Check if parent process id is set */
	if(getppid() == 1)
	{
		/* PPID exists, therefore we are already a daemon */
		return;
	}

	/* Set signal mask - signals we want to block */
	sigemptyset(&newSigSet);
	sigaddset(&newSigSet, SIGCHLD);  /* ignore child - i.e. we don't need to wait for it */
	sigaddset(&newSigSet, SIGTSTP);  /* ignore Tty stop signals */
	sigaddset(&newSigSet, SIGTTOU);  /* ignore Tty background writes */
	sigaddset(&newSigSet, SIGTTIN);  /* ignore Tty background reads */
	sigprocmask(SIG_BLOCK, &newSigSet, NULL);   /* Block the above specified signals */

	/* Set up a signal handler */
	newSigAction.sa_handler = signal_handler;
	sigemptyset(&newSigAction.sa_mask);
	newSigAction.sa_flags = 0;

	/* Signals to handle */
	sigaction(SIGHUP, &newSigAction, NULL);     /* catch hangup signal */
	sigaction(SIGTERM, &newSigAction, NULL);    /* catch term signal */
	sigaction(SIGINT, &newSigAction, NULL);     /* catch interrupt signal */


	/* Fork*/
	pid = fork();

	if(pid < 0)
	{
		/* Could not fork */
		exit(EXIT_FAILURE);
	}

	if(pid > 0)
	{
		/* Child created ok, so exit parent process */
		printf("Child process created: %d\n", pid);
		exit(EXIT_SUCCESS);
	}

	/* Child continues */

	umask(027); /* Set file permissions 750 */

	/* Get a new process group */
	sid = setsid();

	if(sid < 0)
		exit(EXIT_FAILURE);

	/* close all descriptors */
	for(i = getdtablesize(); i >= 0; --i)
		close(i);

	/* Route I/O connections */

	/* Open STDIN */
	i = open("/dev/null", O_RDWR);

	/* STDOUT */
	dup(i);

	/* STDERR */
	dup(i);

	chdir(rundir); /* change running directory */

	/* Ensure only one copy */
	pidFilehandle = open(pidfile, O_RDWR | O_CREAT, 0600);

	if(pidFilehandle == -1)
	{
		/* Couldn't open lock file */
		syslog(LOG_INFO, "Could not open PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}

	/* Try to lock file */
	if(lockf(pidFilehandle, F_TLOCK, 0) == -1)
	{
		/* Couldn't get lock on lock file */
		syslog(LOG_INFO, "Could not lock PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}


	/* Get and format PID */
	sprintf(str, "%d\n", getpid());

	/* write pid to lockfile */
	write(pidFilehandle, str, strlen(str));
}

int main(int argc, char* argv[])
{
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
		daemonize("/tmp/", "/tmp/loggerd.pid");

		syslog(LOG_INFO, "Daemon running");
	}

	if (!logfilen.empty())
		logfile.open(logfilen);

	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "************************************ STARTUP ****************************************");
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Executable: " << argv[0]);
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Version: " << g_version);
	for (int x = 1; x < argc; ++x)
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Command line parameter: " << argv[x]);
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "*************************************************************************************");

	Logger_Dispatcher disp(logfile, g_psubaddr);

	while(!stopEvent.timedwait(1000))
	{
		//syslog(LOG_INFO, "daemon says hello");
		//boost::this_thread::sleep_for(boost::chrono::seconds(1));
	}
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

	//g_name = "PSubPClient_";
	//g_name += argv[1];

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
					if (y == optlen - 1 && ++x < argc && argv[x][0] != L'-')
						g_psubaddr = argv[x];
					else
					{
						std::cout << "Invalid command line parameters" << std::endl;
						usage();
						return false;
					}
					break;
				case 'l': // specify log file
					if (y == optlen - 1 && ++x < argc && argv[x][0] != L'-')
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

