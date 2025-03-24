// ==WindhawkMod==
// @id              rdp-reconnect-please-wait-hang-fix
// @name            Fix RDP "Please wait" hang while reconnecting
// @description     Prevents rdpclip.exe (and optionally, nested mstsc.exe) from blocking the RDP reconnect
// @version         1.0
// @author          Roland Pihlakas
// @github          https://github.com/levitation
// @homepage        https://www.simplify.ee/
// @compilerOptions -lkernel32 -lwtsapi32
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

**Install the mod at the RDP _SERVER_ side.**


## How it works

The mod works by automatically exiting the `rdpclip.exe` process when RDP gets disconnected. This way RDP reconnection does not become hung. 

`rdpclip.exe` is automatically re-started by Windows upon successful RDP reconnect. You do NOT need to manually add rdpclip.exe to Startup programs or anything like that.

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


#ifndef WH_MOD
#define WH_MOD
#include <mods_api.h>
#endif

#pragma endregion Includes



#pragma region Global variables

const int nMaxPathLength = MAX_PATH;

bool g_isRdpClip = true;
bool g_exitMstscAndRelatedProcesses = false;

HANDLE g_rdpMonitorThreadWithPolling = NULL;
HANDLE g_rdpMonitorThreadWithEvent = NULL;

HANDLE g_rdpMonitorThreadWithPollingStopSignal = NULL;
volatile bool g_exitMonitorThreadWithEvent = false;

#pragma endregion Global variables



#pragma region RDP Connection state monitoring threads

//WTSWaitSystemEvent DOES NOT return with WTS_EVENT_DISCONNECT in case the disconnect happened before that function was called. I confirmed this in my own experiments and it is mentioned also here: https://stackoverflow.com/questions/30542847/race-condition-with-wts-session-logon-notification
//Therefore, there would be also race condition between calls to WTSQuerySessionInformationW and WTSWaitSystemEvent.
//Likewise, there is a race between sequential calls to WTSWaitSystemEvent in case some program triggers WTS_EVENT_FLUSH and then a WTS_EVENT_DISCONNECT happens shortly after and before WTSWaitSystemEvent is called again.
//These race conditions can be handled only by adding an extra polling thread. Lets use polling with a long interval to reduce CPU usage.

DWORD WINAPI RDPMonitorThreadFuncWithPolling(LPVOID param) {

    Wh_Log(L"RDPMonitorThreadFuncWithPolling enter");

    while (true) {

        bool sessionDisconnected = false;
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
            Wh_Log(L"Session state: %u, disconnected: %ls", (int)(*pConnectState), sessionDisconnected ? L"Yes" : L"No");
        }
        else {
            Wh_Log(L"WTSQuerySessionInformationW failed");
        }

        if (pConnectState)
            WTSFreeMemory(pConnectState);

        if (sessionDisconnected) {
            ExitProcess(0);
            return FALSE;
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
        DWORD eventFlags;
        if (WTSWaitSystemEvent(
            WTS_CURRENT_SERVER_HANDLE,
            WTS_EVENT_DISCONNECT,
            &eventFlags
        )) {
            sessionDisconnected = (eventFlags & WTS_EVENT_DISCONNECT) != 0;
            Wh_Log(L"Session eventFlags: 0x%X, disconnected: %ls", eventFlags, sessionDisconnected ? L"Yes" : L"No");

            if (sessionDisconnected) {
                ExitProcess(0);
                return FALSE;
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
        *programPath = L'\0';
    }

    size_t programPathLen = wcslen(programPath);

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

void LoadSettings() {

    g_exitMstscAndRelatedProcesses = Wh_GetIntSetting(L"exitMstscAndRelatedProcesses") != 0;

    Wh_Log(L"g_exitMstscAndRelatedProcesses: %u", g_exitMstscAndRelatedProcesses);
}

void Wh_ModSettingsChanged() {
    
    Wh_Log(L"SettingsChanged");

    LoadSettings();

    if (!g_isRdpClip) {     //for rdpclip.exe the monitoring thread is started during Wh_ModInit() and then runs permanently

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

    DetectRdpClipProcess();

    if (
        g_isRdpClip
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
