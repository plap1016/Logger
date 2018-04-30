// RTACMC.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#include "../Logger/Log.h"
#include "../Logger/Logger_Dispatcher.h"

#include <comutil.h>
#include <windows.h>
#include <atldbcli.h>
#include <map>
#include <string>
#include <iostream>
#include <locale>
#include <codecvt>

HANDLE  hStopEvent;
LPTSTR  lpszServiceName = L"CCM2_Logger";
LPTSTR  lpszServiceDisplayName = L"CCM2 Logging Service";
SERVICE_STATUS_HANDLE   ssh;
bool g_exe(false);
Logging::LogFile logfile, *plogfile(&logfile);
std::wstring g_psubaddr(L"127.0.0.1");
std::string g_version;


Logger_Dispatcher* g_svc = nullptr;

bool parseCmdLine(int argc, TCHAR *argv[]);
void usage();
BOOL IsInstalled();
BOOL Install(const std::wstring& cfgf, const std::wstring& logf, wchar_t loglvl);
BOOL Uninstall();
std::string errorStr(DWORD errCode);
void  WINAPI  Service_Main(DWORD dwArgc, LPTSTR *lpszArgv);
DWORD WINAPI Service_Ctrl(DWORD dwCtrlCode, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext);
void  ErrorStopService(LPTSTR lpszAPI);
void  SetTheServiceStatus(DWORD dwCurrentState,DWORD dwWin32ExitCode,
						  DWORD dwCheckPoint,  DWORD dwWaitHint);

#define SVCCMD_LLSET_SEVERE		0x00000080  // 128
#define SVCCMD_LLSET_WARNING	0x00000081  // 129
#define SVCCMD_LLSET_INFO		0x00000082  // 130
#define SVCCMD_LLSET_DEBUG		0x00000083  // 131
#define SVCCMD_LLSET_DUMP		0x00000084  // 132
#define SVCCMD_LLSET_TRACE		0x00000085  // 133
#define SVCCMD_LLSET_TEST		0x00000086  // 134

#define SVCCMD_TEST_OFF			0x00000087  // 135
#define SVCCMD_TEST_ON			0x00000088  // 136

// Entry point for service. Calls StartServiceCtrlDispatcher
// and then blocks until the ServiceMain function returns.
void _tmain(int argc, TCHAR *argv[])
{
	USES_CONVERSION;

	if (!parseCmdLine(argc, argv))
		return;

	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "************************************ STARTUP ****************************************");
	LOGWTO(plogfile, Logging::LL_Info, Logging::LC_Service, _T("***** Executable: ") << argv[0]);
	LOGWTO(plogfile, Logging::LL_Info, Logging::LC_Service, _T("***** Process ID: ") << GetCurrentProcessId());
	DWORD versz(0);
	versz = GetFileVersionInfoSize(argv[0], NULL);
	void* verbuff = new char[versz];
	GetFileVersionInfo(argv[0], 0, versz, verbuff);
	VS_FIXEDFILEINFO* verp(NULL);
	unsigned int verplen(0);
	VerQueryValue(verbuff, _T("\\"), (void**)&verp, &verplen);
	std::ostringstream ver;
	ver << (verp->dwFileVersionMS >> 16) << '.' << (verp->dwFileVersionMS & 0xFFFF) << '.' << (verp->dwFileVersionLS >> 16) << '.' << (verp->dwFileVersionLS & 0xFFFF);
	g_version = ver.str();
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Version: " << g_version);
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "*************************************************************************************");
	delete[] verbuff;

	for (int x = 1; x < argc; ++x)
		LOGWTO(plogfile, Logging::LL_Info, Logging::LC_Service, _T("***** Command line parameter: ") << argv[x]);

	if (!g_exe)
	{
#if defined(_DEBUG) && defined(WIN32)
		Sleep(20000);
#endif
		SERVICE_TABLE_ENTRY ste[] =
		{{TEXT(""),(LPSERVICE_MAIN_FUNCTION)Service_Main}, {NULL, NULL}};

		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Starting as service");
		if (!StartServiceCtrlDispatcher(ste))
			LOGTO(plogfile, Logging::LL_Severe, Logging::LC_Service, "StartServiceCtrlDispatcher fail! Error code: " << errorStr(GetLastError()));
		else
			LOGTO(plogfile, Logging::LL_Debug, Logging::LC_Service, "StartServiceCtrlDispatcher returned!");
	}
	else
	{
		// Running as normal exe
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Starting as executable");
		// Create the event to signal the service to stop.
		hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
#if defined(_DEBUG) && defined(WIN32)
		g_exitEvent = hStopEvent;
#endif

		{
			Logger_Dispatcher svc(logfile, (LPCSTR)bstr_t(g_psubaddr.c_str()));
			g_svc = &svc;

		while(WaitForSingleObject(hStopEvent, 1000) != WAIT_OBJECT_0)
		{
			/***************************************************************/ 
			// Main loop
			/***************************************************************/ 
			;
		}
		}

		// Close the event handle and the thread handle.
		CloseHandle(hStopEvent);
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Stopping executable");
	}
}

// Called by the service control manager after the call to 
// StartServiceCtrlDispatcher.

void WINAPI Service_Main(DWORD dwArgc, LPTSTR *lpszArgv)
{
	// Obtain the name of the service.
	lpszServiceName = lpszArgv[0];

	// Register the service ctrl handler.
	ssh = RegisterServiceCtrlHandlerEx(lpszServiceName, (LPHANDLER_FUNCTION_EX)Service_Ctrl, nullptr);

	SetTheServiceStatus(SERVICE_START_PENDING, 0, 1, 10000);

	// Create the event to signal the service to stop.
	hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (hStopEvent == NULL)
		ErrorStopService(TEXT("CreateEvent"));

#if defined(_DEBUG) && defined(WIN32)
	g_exitEvent = hStopEvent;
#endif

	{
		Logger_Dispatcher svc(logfile, (LPCSTR)bstr_t(g_psubaddr.c_str()));
		g_svc = &svc;

	// The service has started.
	SetTheServiceStatus(SERVICE_RUNNING, 0, 0, 0);
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "SetTheServiceStatus, SERVICE_RUNNING");

	//
	// Main loop for the service.
	// 

	while(WaitForSingleObject(hStopEvent, 1000) != WAIT_OBJECT_0)
	{

		/***************************************************************/ 
		// Main loop for service.
		/***************************************************************/ 
	}
	}

	//
	// Graceful shutdown
	//

	// Close the event handle and the thread handle.
	if (!CloseHandle(hStopEvent))
		ErrorStopService(TEXT("CloseHandle"));

	// Stop the service.
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "SetTheServiceStatus, SERVICE_STOPPED");
	SetTheServiceStatus(SERVICE_STOPPED, NO_ERROR, 0, 0);
}

// Handles control signals from the service control manager.
//void WINAPI Service_Ctrl(DWORD dwCtrlCode)
DWORD WINAPI Service_Ctrl(DWORD dwCtrlCode, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
	DWORD ret = ERROR_CALL_NOT_IMPLEMENTED;
	DWORD dwState = SERVICE_RUNNING;

	switch(dwCtrlCode)
	{
	case SERVICE_CONTROL_STOP:
		dwState = SERVICE_STOP_PENDING;
		ret = NO_ERROR;
		break;

	case SERVICE_CONTROL_SHUTDOWN:
		dwState = SERVICE_STOP_PENDING;
		ret = NO_ERROR;
		break;

	case SERVICE_CONTROL_INTERROGATE:
		ret = NO_ERROR;
		break;

	//case SERVICE_CONTROL_POWEREVENT:
	//	if (dwEventType == PBT_APMRESUMEAUTOMATIC)
	//	{
	//		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "SERVICE_CONTROL_POWEREVENT - PBT_APMRESUMEAUTOMATIC");
	//		g_svc->start();
	//	}
	//	else if (dwEventType == PBT_APMSUSPEND)
	//	{
	//		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "SERVICE_CONTROL_POWEREVENT - PBT_APMSUSPEND");
	//		g_svc->stop();
	//	}

	//	ret = NO_ERROR;
	//	break;

	case SVCCMD_LLSET_SEVERE:
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Setting log level to SEVERE");
		plogfile->setLogLevel(Logging::LLSet_Severe);
		ret = NO_ERROR;
		break;
	case SVCCMD_LLSET_WARNING:
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Setting log level to WARNING");
		plogfile->setLogLevel(Logging::LLSet_Warning);
		ret = NO_ERROR;
		break;
	case SVCCMD_LLSET_INFO:
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Setting log level to INFO");
		plogfile->setLogLevel(Logging::LLSet_Info);
		ret = NO_ERROR;
		break;
	case SVCCMD_LLSET_DEBUG:
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Setting log level to DEBUG");
		plogfile->setLogLevel(Logging::LLSet_Debug);
		ret = NO_ERROR;
		break;
	case SVCCMD_LLSET_DUMP:
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Setting log level to DUMP");
		plogfile->setLogLevel(Logging::LLSet_Dump);
		ret = NO_ERROR;
		break;
	case SVCCMD_LLSET_TRACE:
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Setting log level to TRACE");
		plogfile->setLogLevel(Logging::LLSet_Trace);
		ret = NO_ERROR;
		break;
	case SVCCMD_LLSET_TEST:
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "Setting log level to TEST");
		plogfile->setLogLevel(Logging::LLSet_Test);
		ret = NO_ERROR;
		break;
	//case SVCCMD_TEST_ON: //136
	//	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "SERVICE_CONTROL Test event service resume");
	//	g_svc->start();
	//	ret = NO_ERROR;
	//	break;
	//case SVCCMD_TEST_OFF: //135
	//	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "SERVICE_CONTROL Test event service suspend");
	//	g_svc->stop();
	//	ret = NO_ERROR;
	//	break;
	default:
		break;
	}

	// Set the status of the service.
	SetTheServiceStatus(dwState, NO_ERROR, 0, 10000);
	LOGTO(plogfile, Logging::LL_Trace, Logging::LC_Service, "SetTheServiceStatus, Service_Ctrl function");

	// Tell service_main thread to stop.
	if ((dwCtrlCode == SERVICE_CONTROL_STOP) ||
		(dwCtrlCode == SERVICE_CONTROL_SHUTDOWN))
	{
		if (!SetEvent(hStopEvent))
			ErrorStopService(TEXT("SetEvent"));
		else
			LOGTO(plogfile, Logging::LL_Debug, Logging::LC_Service, "Signal service_main thread to exit");
	}
	return ret;
}

//  Wraps SetServiceStatus.
void SetTheServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode,
						 DWORD dwCheckPoint,   DWORD dwWaitHint)
{
	SERVICE_STATUS ss;  // Current status of the service.

	// Disable control requests until the service is started.
	if (dwCurrentState == SERVICE_START_PENDING)
		ss.dwControlsAccepted = 0;
	else
		ss.dwControlsAccepted =
		SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT;
	// Other flags include SERVICE_ACCEPT_PAUSE_CONTINUE
	// and SERVICE_ACCEPT_SHUTDOWN.

	// Initialize ss structure.
	ss.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
	ss.dwServiceSpecificExitCode = 0;
	ss.dwCurrentState            = dwCurrentState;
	ss.dwWin32ExitCode           = dwWin32ExitCode;
	ss.dwCheckPoint              = dwCheckPoint;
	ss.dwWaitHint                = dwWaitHint;

	// Send status of the service to the Service Controller.
	if (!SetServiceStatus(ssh, &ss))
		ErrorStopService(TEXT("SetServiceStatus"));
}

//  Handle API errors or other problems by ending the service and
//  displaying an error message to the debugger.

void ErrorStopService(LPTSTR lpszAPI)
{
	TCHAR   buffer[256]  = TEXT("");
	TCHAR   error[1024]  = TEXT("");
	LPVOID lpvMessageBuffer;

	wsprintf(buffer,TEXT("API = %s, "), lpszAPI);
	lstrcat(error, buffer);

	ZeroMemory(buffer, sizeof(buffer));
	wsprintf(buffer,TEXT("error code = %d, "), GetLastError());
	lstrcat(error, buffer);

	// Obtain the error string.
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpvMessageBuffer, 0, NULL);

	ZeroMemory((LPVOID)buffer, (DWORD)sizeof(buffer));
	wsprintf(buffer,TEXT("message = %s"), (TCHAR *)lpvMessageBuffer);
	lstrcat(error, buffer);

	// Free the buffer allocated by the system.
	LocalFree(lpvMessageBuffer);

	// Write the error string to the debugger.
	OutputDebugString(error);

	// If you have threads running, tell them to stop. Something went
	// wrong, and you need to stop them so you can inform the SCM.
	SetEvent(hStopEvent);

	// While waiting for the components to shut down.
	//while (true)
	//{
	//	SetTheServiceStatus(SERVICE_STOP_PENDING, 0, t, 3000);
	//}

	// Stop the service.
	SetTheServiceStatus(SERVICE_STOPPED, GetLastError(), 0, 0);
}


BOOL IsInstalled()
{
	BOOL bResult = FALSE;

	SC_HANDLE hSCM = ::OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCM != NULL)
	{
		SC_HANDLE hService = ::OpenService(hSCM, lpszServiceName, SERVICE_QUERY_CONFIG);
		if (hService != NULL)
		{
			bResult = TRUE;
			::CloseServiceHandle(hService);
		}
		::CloseServiceHandle(hSCM);
	}
	return bResult;
}

BOOL Install(const std::wstring& psubAddr, const std::wstring& logf, wchar_t loglvl)
{
	if (IsInstalled())
		return TRUE;

	// Get the executable file path
	TCHAR szFilePath[MAX_PATH + _ATL_QUOTES_SPACE];
	DWORD dwFLen = ::GetModuleFileName(NULL, szFilePath + 1, MAX_PATH);
	if( dwFLen == 0 || dwFLen == MAX_PATH )
		return FALSE;

	// Quote the FilePath before calling CreateService
	szFilePath[0] = _T('\"');
	szFilePath[dwFLen + 1] = _T('\"');
	szFilePath[dwFLen + 2] = 0;

	std::wstring svccmdln(szFilePath);
	
	switch (loglvl)
	{
	case 'd':
	case 'm':
	case 't':
		svccmdln += _T(" -");
		svccmdln += loglvl;
		svccmdln += _T(' ');
		break;
	default:
		break;
	}

	if (!psubAddr.empty())
	{
		svccmdln += _T(" -b \"");
		svccmdln += psubAddr + _T('\"');
	}

	if (!logf.empty())
	{
		svccmdln += _T(" -l \"");
		svccmdln += logf + _T('\"');
	}

	SC_HANDLE hSCM = ::OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM == NULL)
	{
		cout << "Could not open Service Manager" << endl;
		return FALSE;
	}

	SC_HANDLE hService = ::CreateService(
		hSCM, lpszServiceName, lpszServiceDisplayName,
		SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
		SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
		svccmdln.c_str(), NULL, NULL, L"LanmanWorkstation\0", NULL, NULL);

	if (hService == NULL)
	{
		::CloseServiceHandle(hSCM);
		cout << "Could not create service" << endl;
		return FALSE;
	}

	// Need to obtain shutdown privilege to set SC_ACTION_REBOOT in SCM
	HANDLE hToken;
	TOKEN_PRIVILEGES tkp;

	// Get a token for this process. 
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
		return(FALSE);

	// Get the LUID for the shutdown privilege. 
	LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);

	tkp.PrivilegeCount = 1;  // one privilege to set    
	tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	// Get the shutdown privilege for this process. 
	AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, (PTOKEN_PRIVILEGES)NULL, 0);

	if (GetLastError() != ERROR_SUCCESS)
		return FALSE;

	SERVICE_FAILURE_ACTIONS servFailActions;
	SC_ACTION failActions[3];

	failActions[0].Type = SC_ACTION_RESTART; //Failure action: Restart Service
	failActions[0].Delay = 5000; //number of seconds to wait before performing failure action, in milliseconds
	failActions[1].Type = SC_ACTION_RESTART;
	failActions[1].Delay = 5000;
	failActions[2].Type = SC_ACTION_RESTART;
	failActions[2].Delay = 10000;

	servFailActions.dwResetPeriod = 86400; // Reset Failures Counter, in Seconds = 1day
	servFailActions.lpCommand = NULL; //Command to perform due to service failure, not used
	servFailActions.lpRebootMsg = lpszServiceDisplayName; //Message during rebooting computer due to service failure
	servFailActions.cActions = 3; // Number of failure action to manage
	servFailActions.lpsaActions = failActions;

	ChangeServiceConfig2(hService, SERVICE_CONFIG_FAILURE_ACTIONS, &servFailActions); //Apply above settings

	::CloseServiceHandle(hService);
	::CloseServiceHandle(hSCM);

	cout << "Service created" << endl;
	return TRUE;
}

BOOL Uninstall()
{
	if (!IsInstalled())
		return TRUE;

	SC_HANDLE hSCM = ::OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCM == NULL)
	{
		cout << "Could not open Service Manager" << endl;
		return FALSE;
	}

	SC_HANDLE hService = ::OpenService(hSCM, lpszServiceName, SERVICE_STOP | DELETE);

	if (hService == NULL)
	{
		::CloseServiceHandle(hSCM);
		cout << "Could not open service" << endl;
		return FALSE;
	}
	SERVICE_STATUS status;
	BOOL bRet = ::ControlService(hService, SERVICE_CONTROL_STOP, &status);
	if (!bRet)
	{
		DWORD dwError = GetLastError();
		if (!((dwError == ERROR_SERVICE_NOT_ACTIVE) ||  (dwError == ERROR_SERVICE_CANNOT_ACCEPT_CTRL && status.dwCurrentState == SERVICE_STOP_PENDING)))
			cout << "Could not stop service" << endl;
	}


	BOOL bDelete = ::DeleteService(hService);
	::CloseServiceHandle(hService);
	::CloseServiceHandle(hSCM);

	if (bDelete)
	{
		cout << "Service deleted" << endl;
		return TRUE;
	}

	TCHAR szBuf[1024];
	lstrcpy(szBuf,  _T("Could not delete service"));
	MessageBox(NULL, szBuf, lpszServiceName, MB_OK);
	return FALSE;
}

std::string errorStr(DWORD errCode)
{
	LPVOID lpvMessageBuffer;

	// Obtain the error string.
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
				  NULL, errCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&lpvMessageBuffer, 0, NULL);

	std::string ret = std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes((LPTSTR)lpvMessageBuffer);

	// Free the buffer allocated by the system.
	LocalFree(lpvMessageBuffer);

	return std::move(ret);
}

bool parseCmdLine(int argc, TCHAR *argv[])
{
	bool ret(true), install(false);
	wchar_t ll(L'i');

	std::wstring logfile = L"CCM_Logger.log";
	
	plogfile->setLogLevel(Logging::LLSet_Info);
	for (int x = 1; x < argc; ++x)
	{
		if (argv[x][0] == '-' || argv[x][0] == '/')
		{
			// an option
			int optlen = wcslen(argv[x]);
			for (int y = 1; y < optlen; ++y)
			{
				switch (argv[x][y])
				{
				case L'h':
					usage();
					return false;
				case L'd':
					plogfile->setLogLevel(Logging::LLSet_Debug);
					ll = L'd';
					break;
				case L't':
					plogfile->setLogLevel(Logging::LLSet_Trace);
					ll = L't';
					break;
				case L'T':
					plogfile->setLogLevel(Logging::LLSet_Test);
					ll = L'T';
					break;
				case L'm':
					plogfile->setLogLevel(Logging::LLSet_Dump);
					ll = L'm';
					break;
				case L'e': // run as exe
					g_exe = true;
					break;
				case L'b':
					if (y == optlen - 1 && ++x < argc && argv[x][0] != L'-' && argv[x][0] != L'/')
						g_psubaddr = argv[x];
					else
					{
						std::cout << "Invalid command line parameters" << std::endl;
						usage();
						return false;
					}
					break;
				case L'r': // register service
					install = true;
					ret = false;
					break;
				case L'u': // deregister service
					Uninstall();
					return false;
				case L'l': // specify log file
					if (y == optlen - 1 && ++x < argc && argv[x][0] != _T('-') && argv[x][0] != _T('/'))
						logfile = argv[x];
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

	if (install)
		Install(g_psubaddr, logfile, ll);
	else if (!logfile.empty())
		plogfile->open((LPCSTR)bstr_t(logfile.c_str()));

	return ret;
}

void usage()
{
	using namespace std;
	cout << "IO_Service - Central Management Computer IO Service" << endl;
	cout << "Usage: IO_Service [OPTIONS]" << endl;
	cout << "Options:" << endl;
	cout << "\t-h - help. Print this message and exit" << endl;
	cout << "\t     If this option is used any subsequent options are ignored" << endl;
	cout << "\t-d - debug. Sets logging level to DEBUG." << endl;
	cout << "\t-m - dump. Sets logging level to DUMP." << endl;
	cout << "\t-t - trace. Sets logging level to TRACE." << endl;
	cout << "\t-T - test. Sets logging level to TEST." << endl;
	cout << "\t-e - exe. Runs as executable rather than a NT service." << endl;
	cout << "\t-b <ip address> - bus address. Specifies the address of the publish/subscribe server to connect to" << endl;
	cout << "\t     If this option is not used the default will be the local host 127.0.0.1" << endl;
	cout << "\t-r - register. Register the service and exit.  This does not start the service." << endl;
	cout << "\t     If this option is used option -e is ignored" << endl;
	cout << "\t     Other options are included in the registration as service start parameters." << endl;
	cout << "\t-u - unregister. Unregister the service and exit.  Stops the service if it is currently running." << endl;
	cout << "\t     If this option is used any other options are ignored." << endl;
	cout << "\t     If -r is also specified it will be ignored and the service will NOT be registered." << endl;
	cout << "\t-l <log file> - log. Specifies the log file to produce." << endl;
	cout << endl;
	cout << "Multiple options can be grouped together e.g. -de sets logging level to debug and runs as an executable" << endl;
	cout << "Options that require a value (-l) must be at the end of an option group" << endl;
	cout << "\te.g.  -el IO_Service.log  will work but" << endl;
	cout << "\t      -le IO_Service.log  will fail" << endl;
	cout << endl;
	cout << "Multiple option groups can be listed e.g.  -dl IO_Service.log -e" << endl;
	cout << endl;
	cout << "Note: When setting logging levels the last option listed will" << endl;
	cout << "override any previously set logging levels" << endl;
	cout << "\te.g.  -dt ignores the 'd' and sets the logging level to TRACE" << endl;
	cout << "\t      -td ignores the 't' and sets the logging level to DEBUG" << endl;
	cout << "The default logging level if not specified is INFO" << endl;
}
