/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Win32 Service support
   $Id: service.c 934 2006-06-24 13:40:39Z rabbi $ */


#include <windows.h>
#include <stdio.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>

#include "mix3.h"

#ifdef WIN32SERVICE

#define SVCNAME        "Mixmaster"
#define SVCDISPLAYNAME "Mixmaster Service"


/* internal variables */
static SERVICE_STATUS           ssStatus;
static SERVICE_STATUS_HANDLE    sshStatusHandle;
static BOOL                     not_service = FALSE;

static HANDLE hThread = NULL;
static HANDLE hMustTerminate = NULL;

/* internal function prototypes */
VOID WINAPI service_ctrl(DWORD ctrl_code);
VOID WINAPI service_main(DWORD argc, LPSTR *argv);
static DWORD service_run(void);
static void service_stop();
static int set_stdfiles();
static int install_service();
static int remove_service();
static int run_notservice(int argc, char **argv);
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type);
static char *GetLastErrorText();
static BOOL send_status(DWORD current_state, DWORD exit_code, DWORD wait_hint, DWORD id);
static void event_log(DWORD id, char *eventmsg);

int mix_main(int argc, char *argv[]);


int main(int argc, char *argv[])
{
    SERVICE_TABLE_ENTRY dispatchTable[] = {
	{SVCNAME, (LPSERVICE_MAIN_FUNCTION)service_main},
	{NULL,    NULL} };

    if ((argc > 1) && ((argv[1][0] == '-') && (argv[1][1] == '-'))) {
	if (!_stricmp("install-svc", argv[1]+2))
	    return install_service();
	else if (!_stricmp("remove-svc", argv[1]+2))
	    return remove_service();
	else if (_stricmp("run-svc", argv[1]+2) && !is_nt_service())
	    return run_notservice(argc, argv);
    } else if (!is_nt_service()) {
	return run_notservice(argc, argv);
    }
    printf("mix --install-svc   install the service\n");
    printf("mix --remove-svc    remove the service\n");
    printf("mix --run-svc       run as a service\n");
    printf("mix -h          view a summary of the command line options.\n");

    printf("\nStartServiceCtrlDispatcher being called.\n" );
    printf("This may take several seconds.  Please wait.\n" );
    if (!StartServiceCtrlDispatcher(dispatchTable)) {
	printf("Service not started: StartServiceCtrlDispatcher failed.\n" );
	event_log(1000, "Service not started: StartServiceCtrlDispatcher failed");
    }
    return 0;
} /* main */


VOID WINAPI service_main(DWORD argc, LPSTR *argv)
{
    DWORD err = 0;

    if (!(sshStatusHandle = RegisterServiceCtrlHandler(SVCNAME, service_ctrl)))
	return;

    ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ssStatus.dwServiceSpecificExitCode = 0;
    if (send_status(SERVICE_START_PENDING, NO_ERROR, 1000, 1020))
	err = service_run();

    send_status(SERVICE_STOPPED, err, 0, err ? 1030 : 30);
} /* service_main */


VOID WINAPI service_ctrl(DWORD ctrl_code)
{   /* Handle the requested control code. */
    if (ctrl_code == SERVICE_CONTROL_STOP || ctrl_code == SERVICE_CONTROL_SHUTDOWN)
	service_stop();
    else
	send_status(ssStatus.dwCurrentState, NO_ERROR, 0, 1040 + ctrl_code);
} /* service_ctrl */


static DWORD service_run(void)
{
    char filename[_MAX_PATH+1];
    char home[_MAX_PATH+1], *p;
    char *svc_argv[2] = {filename, "-D"};

    if (!hMustTerminate)
	hMustTerminate = CreateEvent(NULL, FALSE, FALSE, NULL);
    set_nt_exit_event(hMustTerminate);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
	    &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);

    GetModuleFileName(NULL , filename, _MAX_PATH);
    strcpy(home, filename);
    if (p = strrchr(home, '\\')) {
	*p = 0;
	chdir(home);
    }

    if (!set_stdfiles()) {
	event_log(1010, "stdin|stdout|stderr not created");
	return ERROR_SERVICE_NOT_ACTIVE;
    }

    send_status(SERVICE_RUNNING, NO_ERROR, 0, 1060);
    event_log(10, "Mixmaster Service started");

    mix_main(2, svc_argv);
    return 0;
} /* service_run */


static void service_stop(void)
{
    send_status(SERVICE_STOP_PENDING, NO_ERROR, 5000, 1070);
    if (hMustTerminate) {
	SetEvent(hMustTerminate);
	if (WaitForSingleObject(hThread, 4500) == WAIT_TIMEOUT) {
	    if (hThread) {
	        TerminateThread(hThread, 0);
	        event_log(1080, "Mixmaster Service terminated forcibly");
	    }
	} else
	    event_log(20, "Mixmaster Service stopped");
	CloseHandle(hMustTerminate);
	hMustTerminate = NULL;
    } else
	if (hThread)
	    TerminateThread(hThread, 0);
    if (hThread)
	CloseHandle(hThread);
    hThread = NULL;
    ssStatus.dwCurrentState = SERVICE_STOPPED;
} /* service_stop */


static int set_stdfiles()
{ /* needed for _popen() */
    static DWORD std_handles[]={STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    FILE *stdfile[]={stdin, stdout, stderr};
    HANDLE hStd;
    int fh, stf_fileno;
    FILE *fl;

    AllocConsole();
    for (stf_fileno=0; stf_fileno<=2; stf_fileno++) {
	hStd = GetStdHandle(std_handles[stf_fileno]);
	if (hStd == INVALID_HANDLE_VALUE)
	    return 0;
	fh = _open_osfhandle((long)std_handles[stf_fileno], (stf_fileno ? _O_WRONLY : _O_RDONLY ) | _O_BINARY);
	dup2(fh, stf_fileno);
	fl = _fdopen(stf_fileno, (stf_fileno ? "wcb" : "rcb" ));
	fflush(stdfile[stf_fileno]);
	memcpy(stdfile[stf_fileno], fl, sizeof(FILE));
    }
    return 1;
} /* set_stdfiles */


static BOOL send_status(DWORD current_state, DWORD exit_code, DWORD wait_hint, DWORD id)
{
    static DWORD dwCheckPoint = 1;
    BOOL ret_val;

    if (not_service)
	return TRUE;

    ssStatus.dwCurrentState = current_state;
    ssStatus.dwWin32ExitCode = exit_code;
    ssStatus.dwWaitHint = wait_hint;
    ssStatus.dwControlsAccepted = (current_state == SERVICE_START_PENDING) ?
	0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    ssStatus.dwCheckPoint = ((current_state == SERVICE_RUNNING) || (current_state == SERVICE_STOPPED)) ?
	0 : dwCheckPoint++;

    if (!(ret_val = SetServiceStatus(sshStatusHandle, &ssStatus)))
	event_log(id, "SetServiceStatus failed");
    return ret_val;
} /* send_status */


static void event_log(DWORD id, char *eventmsg)
{
    HANDLE  hEventSource;
    char    *pStrings[2] = {"", eventmsg};

    if (not_service)
	return;

    if (id > 1000)
	pStrings[0] = GetLastErrorText();

    if (!(hEventSource = RegisterEventSource(NULL, SVCNAME)))
	return;
    ReportEvent(hEventSource, (WORD)((id < 1000) ? EVENTLOG_SUCCESS : EVENTLOG_ERROR_TYPE),
	0, id, NULL, 2, 0, pStrings, NULL);
    DeregisterEventSource(hEventSource);
} /* event_log */


static int run_notservice(int argc, char ** argv)
{
    not_service = TRUE;
    return mix_main(argc, argv);
} /* run_notservice */


static int install_service()
{
    SC_HANDLE schService, schSCManager;
    char filename[_MAX_PATH+10];

    if (GetModuleFileName(NULL, filename, _MAX_PATH) == 0) {
	printf("Unable to install Mixmaster Service: %s\n", GetLastErrorText());
	return 1;
    }
    strcat(filename, " --run-svc");

    if (!(schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS))) {
	printf("OpenSCManager failed: %s\n", GetLastErrorText());
	return 1;
    }
    schService = CreateService(schSCManager, SVCNAME, SVCDISPLAYNAME,
	SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
	filename, NULL, NULL, NULL, NULL, NULL);

    if (schService) {
	printf("Mixmaster Service installed.\n");
	CloseServiceHandle(schService);
    } else {
	printf("CreateService failed: %s\n", GetLastErrorText());
    }

    CloseServiceHandle(schSCManager);
    return 0;
} /* install_service */


static int remove_service()
{
    SC_HANDLE schService, schSCManager;
    int ret_val = 0;

    if (!(schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS))) {
	printf("OpenSCManager failed: %s\n", GetLastErrorText());
	return 1;
    }
    if (!(schService = OpenService(schSCManager, SVCNAME, SERVICE_ALL_ACCESS))) {
	CloseServiceHandle(schSCManager);
	printf("OpenService failed: %s\n", GetLastErrorText());
	return 1;
    }
    /* try to stop the service */
    if (ControlService(schService, SERVICE_CONTROL_STOP, &ssStatus)) {
	printf("Stopping Mixmaster Service");
	do {
	    sleep(1);
	    printf(".");
	    QueryServiceStatus(schService, &ssStatus);
	} while (ssStatus.dwCurrentState != SERVICE_STOP_PENDING);

	if (ssStatus.dwCurrentState == SERVICE_STOPPED)
	    printf("\nMixmaster Service stopped.\n");
	else
	    printf("\n%Mixmaster Service failed to stop.\n");
    }

    /* now remove the service */
    if (!DeleteService(schService)) {
	ret_val = 1;
	printf("DeleteService failed: %s\n", GetLastErrorText());
    } else
	printf("Mixmaster Service removed.\n");

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return ret_val;
} /* remove_service */


static char *GetLastErrorText()
{
    static char error_buf[256];
    DWORD dwRet, err;
    LPSTR lpszTemp = NULL;

    dwRet = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY,
	                  NULL, err=GetLastError(), LANG_NEUTRAL, (LPSTR)&lpszTemp, 0, NULL);

    /* supplied buffer is not long enough */
    if (!dwRet || (256 < (long)dwRet+14))
	sprintf(error_buf, "Error (0x%x)", err);
    else {
	lpszTemp[lstrlen(lpszTemp)-2] = '\0';
	/* remove cr and newline character */
	sprintf(error_buf, "%s (0x%x)", lpszTemp, err);
    }

    if (lpszTemp)
	LocalFree((HLOCAL)lpszTemp);

    return error_buf;
} /* GetLastErrorText */

#endif /* WIN32SERVICE */
