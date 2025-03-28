// ==WindhawkMod==
// @id              rdp-reconnect-please-wait-hang-fix
// @name            Fix RDP "Please wait" hang while reconnecting
// @description     Prevents rdpclip.exe (and optionally, nested mstsc.exe) from blocking the RDP reconnect
// @version         1.0
// @author          Roland Pihlakas
// @github          https://github.com/levitation
// @homepage        https://www.simplify.ee/
// @compilerOptions -lkernel32 -lwtsapi32
// @include         winlogon.exe
// @include         rdpclip.exe
// @include         mstsc.exe
// @include         vmconnect.exe
// @include         msrdc.exe
// @include         msrdcw.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Fix for RDP "Please wait" hang while reconnecting

RDP users sometimes encounter issues with an endless "Please Wait" message during RDP reconnection. This problem is caused by server-side `rdpclip.exe` blocking the reconnection for some reason.

The current mod prevents `rdpclip.exe` from blocking the RDP reconnect. 


## Installation

**Install the mod at the RDP _SERVER_ side.**

The mod needs to inject into the process `winlogon.exe` among other processes. In order for that to be possible, you need to enable injection to `winlogon.exe` under Windhawk's advanced settings. The details for this are as follows:

**This requires Windhawk to be installed, not just run as a portable version.** Portable version of Windhawk will have insufficient privileges to inject into `winlogon.exe`.

Before you start installing the current mod, you need to update Windhawk process inclusion list, accessible via `Windhawk -> Settings -> Advanced settings -> More advanced settings -> Process inclusion list`. Add the following row if not yet present there:

> `winlogon.exe`

Then click `"Save and restart Windhawk"` button.

After doing that you can proceed installing the mod itself.


## How it works

The mod works by automatically exiting the `rdpclip.exe` process **and** also restarting RDP-related services when RDP gets disconnected. This way RDP reconnection does not become hung. The RDP related services that will be restarted are `TermService` and `UmRdpService`.

`rdpclip.exe` is automatically re-started by Windows upon successful RDP reconnect. You do NOT need to manually add rdpclip.exe to Startup programs or anything like that.

Note that **after disconnection, there will be a brief amount of time during which RDP service is not available**. This is because the RPD service is being automatically restarted by the mod. This means you might not be able to reconnect **immediately**, but only after some seconds have passed. That is a tradeoff of using this mod. _If you consider it important to be able to reconnect immediately **and** do **not** have any issues with RDP reconnect becoming hung, then do not install this mod._

See also: [No Restart Needed: Fixing the dreaded “Please Wait” for Remote Desktop Connections](https://insomnyak.medium.com/no-restart-needed-fixing-the-dreaded-please-wait-for-remote-desktop-connections-3868785cf36)


## Nested mstsc.exe termination setting

The above referred blog post mentions also a need for killing nested `mstsc.exe` (RDP client) processes in the RDP server machine, but I have not encountered this problem myself, so I disabled nested `mstsc.exe` processes termination by default. If you think auto-terminating nested `mstsc.exe` (or related processes) in the RDP server would be helpful for you, then you can enable this option under current mod's settings. 

Note that when you enable nested `mstsc.exe` process auto-termination, then this rule will also apply to the following additional executables in the server, which duplicate `mstsc.exe` functionalities: `vmconnect.exe`, `msrdc.exe`, and `msrdcw.exe`.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- exitMstscAndRelatedProcesses: false
  $name: Exit nested RDP client ("mstsc.exe" and related) processes in the RDP server
  $description: When enabled, nested RDP client processes ("mstsc.exe", "vmconnect.exe", "msrdc.exe", and "msrdcw.exe") are also terminated in the RDP server upon RDP disconnect from the server
*/
// ==/WindhawkModSettings==



#pragma region Includes

#include <windowsx.h>
#include <wtsapi32.h>
#include <intrin.h>


#ifndef WH_MOD
#define WH_MOD
#include <mods_api.h>
#endif

#pragma endregion Includes



#pragma region Global variables

const int nMaxPathLength = MAX_PATH;

DWORD g_sessionId = -1;
bool g_isWinLogon = true;
bool g_isRdpClip = true;
volatile long g_rdpSessionHasBeenActive = 0;
volatile bool g_exitMstscAndRelatedProcesses = false;

HANDLE g_rdpMonitorThreadWithPolling = NULL;
HANDLE g_rdpMonitorThreadWithEvent = NULL;

HANDLE g_rdpMonitorThreadWithPollingStopSignal = NULL;
volatile bool g_exitMonitorThreadWithEvent = false;

#pragma endregion Global variables



#pragma region Terminal services restart function

bool RestartTerminalServices() {

    //UmRdpService depends on TermService, therefore need to stop and start that one too
    WCHAR szCmdLine[] = L"cmd.exe /C \"net stop UmRdpService & net stop TermService & net start TermService & net start UmRdpService\"";    


    STARTUPINFOW startupInfo = {};
    startupInfo.cb = { sizeof(STARTUPINFOW) };
    GetStartupInfoW(&startupInfo);

    PROCESS_INFORMATION processInformation = {};

    Wh_Log(L"Calling CreateProcessW");
    if (!CreateProcessW(
        /*lpApplicationName*/NULL,
        /*lpCommandLine*/szCmdLine,
        /*lpProcessAttributes*/NULL,
        /*lpThreadAttributes*/NULL,
        /*bInheritHandles*/FALSE,
        /*dwCreationFlags*/CREATE_NEW_CONSOLE,
        /*lpEnvironment*/NULL,
        /*lpCurrentDirectory*/NULL,
        &startupInfo,
        &processInformation
    )) {
        Wh_Log(L"CreateProcessW failed");
        return false;
    }

    //close process and thread handles
    Wh_Log(L"Calling CloseHandle");
    CloseHandle(processInformation.hProcess);
    Wh_Log(L"Calling CloseHandle");
    CloseHandle(processInformation.hThread);

    return true;
}

#pragma endregion Terminal services restart function



#pragma region RDP Connection state monitoring threads

//WTSWaitSystemEvent DOES NOT return with WTS_EVENT_DISCONNECT in case the disconnect happened before that function was called. I confirmed this in my own experiments and it is mentioned also here: https://stackoverflow.com/questions/30542847/race-condition-with-wts-session-logon-notification
//Therefore, there would be also race condition between calls to WTSQuerySessionInformationW and WTSWaitSystemEvent.
//Likewise, there is a race between sequential calls to WTSWaitSystemEvent in case some program triggers WTS_EVENT_FLUSH and then a WTS_EVENT_DISCONNECT happens shortly after and before WTSWaitSystemEvent is called again.
//These race conditions can be handled only by adding an extra polling thread. Lets use polling with a long interval to reduce CPU usage.

DWORD WINAPI RDPMonitorThreadFuncWithPolling(LPVOID param) {

    Wh_Log(L"RDPMonitorThreadFuncWithPolling enter");

    while (true) {

        bool sessionDisconnected = false;
        bool rdpSessionIsActive = false;

        WTS_CONNECTSTATE_CLASS* pConnectState = NULL;
        DWORD bytesReturned;
        if (
            WTSQuerySessionInformationW(
                WTS_CURRENT_SERVER_HANDLE,
                WTS_CURRENT_SESSION,
                WTSConnectState,
                (LPWSTR*)&pConnectState,
                &bytesReturned
            )
            && pConnectState
            && bytesReturned == sizeof(WTS_CONNECTSTATE_CLASS)
        ) {
            sessionDisconnected = (*pConnectState == WTSDisconnected);
            rdpSessionIsActive = (*pConnectState == WTSActive) && (g_sessionId != WTSGetActiveConsoleSessionId());  //NB! ignore console session active state

            Wh_Log(L"Session state: %u, disconnected: %ls, rdpActive: %ls", (int)(*pConnectState), sessionDisconnected ? L"Yes" : L"No", rdpSessionIsActive ? L"Yes" : L"No");
        }
        else {
            Wh_Log(L"WTSQuerySessionInformationW failed");
        }

        if (pConnectState)
            WTSFreeMemory(pConnectState);

        if (
            sessionDisconnected
            && g_rdpSessionHasBeenActive != 0
            && InterlockedExchange(&g_rdpSessionHasBeenActive, 0) != 0  //allow only one thread to signal disconnect
        ) {
            if (g_isWinLogon) {
                RestartTerminalServices();
            }
            else {
                ExitProcess(0);
                return FALSE;
            }            
        }
        else if (rdpSessionIsActive) {
            g_rdpSessionHasBeenActive = 1;
        }


        //WTSQuerySessionInformationW consumes about 1 million CPU cycles per call.
        //Therefore lets use polling with a long interval to reduce CPU usage.
        if (WaitForSingleObject(g_rdpMonitorThreadWithPollingStopSignal, 60 * 1000) != WAIT_TIMEOUT) {
            Wh_Log(L"Shutting down RDPMonitorThreadFuncWithPolling");
            return FALSE;
        }
    }   //while (true)
}

DWORD WINAPI RDPMonitorThreadFuncWithEvent(LPVOID param) {

    Wh_Log(L"RDPMonitorThreadFuncWithEvent enter");

    while (!g_exitMonitorThreadWithEvent) {

        bool sessionDisconnected = false;
        bool rdpSessionIsActive = false;
        DWORD eventFlags;
        if (WTSWaitSystemEvent(
            WTS_CURRENT_SERVER_HANDLE,
            WTS_EVENT_DISCONNECT | WTS_EVENT_STATECHANGE,
            &eventFlags
        )) {
            sessionDisconnected = (eventFlags & WTS_EVENT_DISCONNECT) != 0;
            
            bool stateChange = (eventFlags & WTS_EVENT_STATECHANGE) != 0;
            if (
                !sessionDisconnected
                && stateChange
                && g_sessionId != WTSGetActiveConsoleSessionId()    //NB! ignore console session active state
            ) {
                //need to query WTSQuerySessionInformationW since WTSWaitSystemEvent does not return detailed connect state directly

                WTS_CONNECTSTATE_CLASS* pConnectState = NULL;
                DWORD bytesReturned;
                if (
                    WTSQuerySessionInformationW(
                        WTS_CURRENT_SERVER_HANDLE,
                        WTS_CURRENT_SESSION,
                        WTSConnectState,
                        (LPWSTR*)&pConnectState,
                        &bytesReturned
                    )
                    && pConnectState
                    && bytesReturned == sizeof(WTS_CONNECTSTATE_CLASS)
                ) {
                    rdpSessionIsActive = (*pConnectState == WTSActive);  

                    Wh_Log(L"Session state: %u, disconnected: %ls, rdpActive: %ls", (int)(*pConnectState), sessionDisconnected ? L"Yes" : L"No", rdpSessionIsActive ? L"Yes" : L"No");
                }
                else {
                    Wh_Log(L"WTSQuerySessionInformationW failed");
                }
            }

            Wh_Log(L"Session eventFlags: 0x%X, disconnected: %ls, rdpActive: %ls", eventFlags, sessionDisconnected ? L"Yes" : L"No", rdpSessionIsActive ? L"Yes" : L"No");

            if (
                sessionDisconnected
                && g_rdpSessionHasBeenActive != 0
                && InterlockedExchange(&g_rdpSessionHasBeenActive, 0) != 0  //allow only one thread to signal disconnect
            ) {
                if (g_isWinLogon) {
                    RestartTerminalServices();
                }
                else {
                    ExitProcess(0);
                    return FALSE;
                }                
            }
            else if (rdpSessionIsActive) {
                g_rdpSessionHasBeenActive = 1;
            }
        }
        else {
            Wh_Log(L"WTSWaitSystemEvent failed");
        }
    }   //while (!g_exitMonitorThreadWithEvent)

    Wh_Log(L"Shutting down RDPMonitorThreadFuncWithEvent");
    return FALSE;
}

void ExitMonitoringThreads() {

    if (g_rdpMonitorThreadWithPolling) {    //signal polling thread
                
        SetEvent(g_rdpMonitorThreadWithPollingStopSignal);

        WaitForSingleObject(g_rdpMonitorThreadWithPolling, INFINITE);
        CloseHandle(g_rdpMonitorThreadWithPolling);
        g_rdpMonitorThreadWithPolling = NULL;
    }

    if (g_rdpMonitorThreadWithEvent) {  //signal event thread
        
        g_exitMonitorThreadWithEvent = true;

        //wake the event monitoring thread
        DWORD eventFlags;
        if (!WTSWaitSystemEvent(
            WTS_CURRENT_SERVER_HANDLE,
            WTS_EVENT_FLUSH,    //causes all pending WTSWaitSystemEvent calls on the specified RD Session Host server handle to return with WTS_EVENT_NONE eventFlags
            &eventFlags
        )) {
            Wh_Log(L"WTSWaitSystemEvent with WTS_EVENT_FLUSH failed");
        }

        WaitForSingleObject(g_rdpMonitorThreadWithEvent, INFINITE);
        CloseHandle(g_rdpMonitorThreadWithEvent);
        g_rdpMonitorThreadWithEvent = NULL;
    }

    if (g_rdpMonitorThreadWithPollingStopSignal) {

        CloseHandle(g_rdpMonitorThreadWithPollingStopSignal);
        g_rdpMonitorThreadWithPollingStopSignal = NULL;
    }
}

BOOL StartMonitoringThreads() {

    if (!g_rdpMonitorThreadWithPollingStopSignal) {

        g_rdpMonitorThreadWithPollingStopSignal = CreateEventW(
            /*lpEventAttributes = */NULL,           // default security attributes
            /*bManualReset = */TRUE,				// manual-reset event
            /*bInitialState = */FALSE,              // initial state is nonsignaled
            /*lpName = */NULL						// object name
        );

        if (!g_rdpMonitorThreadWithPollingStopSignal) {
            Wh_Log(L"CreateEvent failed");
            return FALSE;
        }
    }

    g_exitMonitorThreadWithEvent = false;


    if (!g_rdpMonitorThreadWithPolling) {

        g_rdpMonitorThreadWithPolling = CreateThread(
            /*lpThreadAttributes = */NULL,
            /*dwStackSize = */0,
            RDPMonitorThreadFuncWithPolling,
            /*lpParameter = */NULL,
            /*dwCreationFlags = */0,        //start the thread immediately
            /*lpThreadId = */NULL
        );

        if (g_rdpMonitorThreadWithPolling) {
            Wh_Log(L"RDPMonitorThreadWithPolling created");
        }
        else {
            Wh_Log(L"CreateThread failed");
            ExitMonitoringThreads();
            return FALSE;
        }
    }


    if (!g_rdpMonitorThreadWithEvent) {

        g_rdpMonitorThreadWithEvent = CreateThread(
            /*lpThreadAttributes = */NULL,
            /*dwStackSize = */0,
            RDPMonitorThreadFuncWithEvent,
            /*lpParameter = */NULL,
            /*dwCreationFlags = */0,        //start the thread immediately
            /*lpThreadId = */NULL
        );

        if (g_rdpMonitorThreadWithEvent) {
            Wh_Log(L"RDPMonitorThreadWithEvent created");
        }
        else {
            Wh_Log(L"CreateThread failed");
            ExitMonitoringThreads();
            return FALSE;
        }
    }
    
    
    return TRUE;
}

#pragma endregion RDP Connection state monitoring threads



#pragma region Mod entrypoints - Init, SettingsChanged, Uninit

void DetectRdpClipProcess() {

    WCHAR programPath[nMaxPathLength + 1];
    DWORD dwSize = ARRAYSIZE(programPath);
    if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, programPath, &dwSize)) {
        *programPath = L'\0';   //then assume it is mstsc.exe-like process
    }

    size_t programPathLen = wcslen(programPath);


    PCWSTR winLogonProcessName = L"\\winlogon.exe";
    size_t winLogonNameLen = wcslen(winLogonProcessName);
    if (
        programPathLen > winLogonNameLen
        && wcsicmp(&programPath[programPathLen - winLogonNameLen], winLogonProcessName) == 0     //match end of path (this includes file name)
    ) {
        Wh_Log(L"Running in winlogon.exe process");
        g_isWinLogon = true;
    }
    else {
        g_isWinLogon = false;

        PCWSTR rdpClipProcessName = L"\\rdpclip.exe";
        size_t rdpClipNameLen = wcslen(rdpClipProcessName);
        if (
            programPathLen > rdpClipNameLen
            && wcsicmp(&programPath[programPathLen - rdpClipNameLen], rdpClipProcessName) == 0     //match end of path (this includes file name)
        ) {
            Wh_Log(L"Running in rdpclip.exe process");
            g_isRdpClip = true;
        }
        else {
            Wh_Log(L"Running in mstsc.exe or related process");
            g_isRdpClip = false;
        }
    }
}

bool InitSessionIdVar() {

    DWORD sessionId;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
        Wh_Log(L"ProcessIdToSessionId failed");
        return false;
    }
    else {
        g_sessionId = sessionId;
        return true;
    }
}

void LoadSettings() {

    g_exitMstscAndRelatedProcesses = Wh_GetIntSetting(L"exitMstscAndRelatedProcesses") != 0;

    Wh_Log(L"g_exitMstscAndRelatedProcesses: %u", g_exitMstscAndRelatedProcesses);
}

void Wh_ModSettingsChanged() {
    
    Wh_Log(L"SettingsChanged");

    LoadSettings();

    if (     //for rdpclip.exe and winlogon.exe the monitoring thread is started during Wh_ModInit() and then runs permanently
        !g_isRdpClip 
        && !g_isWinLogon
    ) {
        if (g_exitMstscAndRelatedProcesses) {

            StartMonitoringThreads();
        }
        else {
            ExitMonitoringThreads();
        }
    }
}

BOOL Wh_ModInit() {

    Wh_Log(L"Init");

    LoadSettings();

    if (!InitSessionIdVar())
        return FALSE;

    DetectRdpClipProcess();

    if (
        g_isRdpClip
        || g_isWinLogon
        || g_exitMstscAndRelatedProcesses
    ) {
        return StartMonitoringThreads();
    }
    else {
        return TRUE;
    }
}

void Wh_ModUninit() {

    Wh_Log(L"Uniniting...");

    ExitMonitoringThreads();

    Wh_Log(L"Uninit complete");
}

#pragma endregion Mod entrypoints - Init, SettingsChanged, Uninit
