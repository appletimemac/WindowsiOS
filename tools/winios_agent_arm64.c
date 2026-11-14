/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pure native AArch64 Win32 posture agent (`WinIOSAgent.exe`), windowless.
 *
 * Every 500 ms it asks the host which posture the device is in (`GET http://10.0.2.4/.henge/posture`,
 * "laptop" or "tablet") and compares it with HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl\
 * ConvertibleSlateMode (1 laptop, 0 slate). When they differ it toggles Windows' inbox "GPIO Laptop or
 * Slate Indicator" driver (ACPI PNP0C60, see devices/acpi.c) through Microsoft's injection interface:
 * open the GUID_GPIOBUTTONS_LAPTOPSLATE_INTERFACE device, WriteFile one zero byte, which flips the state.
 * The driver updates ConvertibleSlateMode itself, which is how the agent sees the toggle land.
 *
 * It must run elevated: the indicator device's ACL gives administrators write access only through an
 * elevated token, so a normal-token copy fails CreateFile with error 5 (logged to %TEMP%\winios_agent.log).
 * That is why the installer registers it as a scheduled task with highest privileges.
 *
 * It also asks `GET http://10.0.2.4/.henge/power` each round; "hibernate" (the app's Hibernate action, answered
 * once) runs shutdown /h, so Windows saves its session and powers off, and the next boot resumes it.
 *
 * The posture reply may end in " scale=NNN", the scale the app wants Windows at, in percent. Windows takes its scale
 * at sign-in, so the agent writes it down for the next one as the system-wide custom scale (LogPixels with
 * Win8DpiScaling 1 under HKCU\Control Panel\Desktop, and under HKU\.DEFAULT for the sign-in screen), and with every
 * poll it tells the app the scale this session runs at ("?scale=NNN"), which is what the app sizes the picture for.
 * The display's own scale (Settings, Display, Scale) would change at once, but Windows only offers a display the
 * scales that leave 600 units on its shorter side (Windows 11, read through DisplayConfigGetDeviceInfo: 1378x954
 * goes up to 150 percent, 1378x899 to 125, 673x867 and 1378x480 only 100). That holds the small pictures, a folded
 * phone's outer display and the half above the keyboard, at 100 percent. The custom scale has no such limit.
 *
 * A second thread tells the app when to raise the iOS keyboard. It long-polls `GET /.henge/focus-wait`, which the
 * host answers when the user tapped the picture (with the tap's pixel) or typed Enter. It then asks UI Automation
 * (IUIAutomation::GetFocusedElement) whether the focused element takes text and whether the tap was inside it, and
 * reports that at `GET /.henge/focus?seq=..&editable=..&inside=..`, looking again for a moment after a tap. The process is per-monitor DPI aware so UI
 * Automation rectangles are physical pixels, the same pixels as the guest framebuffer the host's tap is in.
 * If UI Automation can't start, the thread logs it and retries; the posture loop is never affected.
 */
#include <stdint.h>
#include <stddef.h>

#include "winios_agent_scale.h"

typedef void *HANDLE;
typedef void *HKEY;
typedef void *HDEVINFO;
typedef uint16_t WCHAR;
typedef uint32_t DWORD;
typedef int32_t BOOL;
typedef int32_t LONG;
typedef uintptr_t SOCKET;

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_SOCKET       ((SOCKET)(~0ULL))
#define AF_INET              2
#define SOCK_STREAM          1
#define IPPROTO_TCP          6
#define SOL_SOCKET           0xFFFF
#define SO_SNDTIMEO          0x1005
#define SO_RCVTIMEO          0x1006
#define GENERIC_WRITE        0x40000000U
#define OPEN_EXISTING        3U
#define FILE_APPEND_DATA     0x0004U
#define OPEN_ALWAYS          4U
#define FILE_ATTRIBUTE_NORMAL 0x0080U
#define FILE_SHARE_READ      0x0001U
#define FILE_SHARE_WRITE     0x0002U
#define ERROR_ALREADY_EXISTS 183U
#define HKEY_CURRENT_USER    ((HKEY)(uintptr_t)0x80000001ULL)
#define HKEY_LOCAL_MACHINE   ((HKEY)(uintptr_t)0x80000002ULL)
#define HKEY_USERS           ((HKEY)(uintptr_t)0x80000003ULL)
#define KEY_QUERY_VALUE      0x0001U
#define KEY_SET_VALUE        0x0002U
#define REG_DWORD            4U
#define DIGCF_PRESENT        0x00000002U
#define DIGCF_DEVICEINTERFACE 0x00000010U
#define ERROR_INSUFFICIENT_BUFFER 122U
#define RPC_E_CHANGED_MODE   ((int32_t)0x80010106)
#define CLSCTX_INPROC_SERVER 1U
#define COINIT_MULTITHREADED 0U
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)(intptr_t)-4)

/* Network byte order IPv4 constant on little-endian ARM64 */
#define IPV4_HENGE_SHARE     0x0402000AU /* 10.0.2.4 */

#define POLL_MS              500U
#define VERIFY_MS            2000U
#define BACKOFF_FAILED_MS    3000U
#define BACKOFF_NOEFFECT_MS  10000U
#define LOG_LINES_MAX        200
#define SCALE_CHECK_ROUNDS   4      /* posture rounds between looks at the scale in the registry (about 2 s) */
#define SCALE_LOGS_MAX       20
#define FOCUS_WAIT_RECV_MS   12000U /* longer than the host's 8 s long poll */
/* After a tap the agent looks at 200, 600 and 1400 ms until it finds text under the tap: a field that has to
   open first (the taskbar's search box) takes the focus late. The last look stays well inside the host's 2 s
   limit for an event, so the next tap is never dropped for waiting behind this one. */
static const DWORD kFocusLooksMs[] = {200U, 400U, 800U};
#define FOCUS_RETRY_MS       2000U
#define UIA_RETRY_MS         30000U
#define TAP_SLOP_PX          8

typedef struct {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t  d4[8];
} Guid;

typedef struct {
    DWORD     cbSize;
    Guid      InterfaceClassGuid;
    DWORD     Flags;
    uintptr_t Reserved;
} SpDeviceInterfaceData;

typedef struct {
    DWORD cbSize;
    WCHAR DevicePath[1];
} SpDeviceInterfaceDetailDataW;

_Static_assert(sizeof(Guid) == 16, "GUID layout");
_Static_assert(sizeof(SpDeviceInterfaceData) == 32, "SP_DEVICE_INTERFACE_DATA on 64-bit Windows");
_Static_assert(sizeof(SpDeviceInterfaceDetailDataW) == 8, "SP_DEVICE_INTERFACE_DETAIL_DATA_W on 64-bit Windows");

typedef struct {
    int16_t  sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} SockAddrIn;

/* kernel32.dll */
__declspec(dllimport) DWORD  GetTempPathW(DWORD nBufferLength, WCHAR *lpBuffer);
__declspec(dllimport) HANDLE CreateFileW(const WCHAR *lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                         void *lpSecurityAttributes, DWORD dwCreationDisposition,
                                         DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
__declspec(dllimport) BOOL   WriteFile(HANDLE hFile, const void *lpBuffer, DWORD nBytes,
                                       DWORD *lpWritten, void *lpOverlapped);
__declspec(dllimport) BOOL   CloseHandle(HANDLE hObject);
__declspec(dllimport) HANDLE CreateMutexW(void *lpMutexAttributes, BOOL bInitialOwner, const WCHAR *lpName);
__declspec(dllimport) DWORD  GetLastError(void);
__declspec(dllimport) void   Sleep(DWORD dwMilliseconds);
__declspec(dllimport) uint64_t GetTickCount64(void);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void  *HeapAlloc(HANDLE hHeap, DWORD dwFlags, size_t dwBytes);
__declspec(dllimport) BOOL   HeapFree(HANDLE hHeap, DWORD dwFlags, void *lpMem);
__declspec(dllimport) void   ExitProcess(DWORD uExitCode);
__declspec(dllimport) HANDLE CreateThread(void *lpThreadAttributes, size_t dwStackSize,
                                          DWORD (*lpStartAddress)(void *), void *lpParameter,
                                          DWORD dwCreationFlags, DWORD *lpThreadId);

typedef struct {
    DWORD    cb;
    WCHAR   *lpReserved, *lpDesktop, *lpTitle;
    DWORD    dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    uint16_t wShowWindow, cbReserved2;
    uint8_t *lpReserved2;
    HANDLE   hStdInput, hStdOutput, hStdError;
} StartupInfoW;
typedef struct {
    HANDLE hProcess, hThread;
    DWORD  dwProcessId, dwThreadId;
} ProcessInformation;
_Static_assert(sizeof(StartupInfoW) == 104, "STARTUPINFOW on 64-bit Windows");
_Static_assert(sizeof(ProcessInformation) == 24, "PROCESS_INFORMATION on 64-bit Windows");
#define CREATE_NO_WINDOW 0x08000000U
__declspec(dllimport) BOOL CreateProcessW(const WCHAR *lpApplicationName, WCHAR *lpCommandLine, void *lpProcessAttributes,
                                          void *lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
                                          void *lpEnvironment, const WCHAR *lpCurrentDirectory,
                                          StartupInfoW *lpStartupInfo, ProcessInformation *lpProcessInformation);

/* advapi32.dll */
__declspec(dllimport) LONG RegOpenKeyExW(HKEY hKey, const WCHAR *lpSubKey, DWORD ulOptions,
                                         DWORD samDesired, HKEY *phkResult);
__declspec(dllimport) LONG RegQueryValueExW(HKEY hKey, const WCHAR *lpValueName, DWORD *lpReserved,
                                            DWORD *lpType, void *lpData, DWORD *lpcbData);
__declspec(dllimport) LONG RegSetValueExW(HKEY hKey, const WCHAR *lpValueName, DWORD Reserved, DWORD dwType,
                                          const void *lpData, DWORD cbData);
__declspec(dllimport) LONG RegCloseKey(HKEY hKey);

/* setupapi.dll */
__declspec(dllimport) HDEVINFO SetupDiGetClassDevsW(const Guid *ClassGuid, const WCHAR *Enumerator,
                                                    void *hwndParent, DWORD Flags);
__declspec(dllimport) BOOL SetupDiEnumDeviceInterfaces(HDEVINFO DeviceInfoSet, void *DeviceInfoData,
                                                       const Guid *InterfaceClassGuid, DWORD MemberIndex,
                                                       SpDeviceInterfaceData *DeviceInterfaceData);
__declspec(dllimport) BOOL SetupDiGetDeviceInterfaceDetailW(HDEVINFO DeviceInfoSet,
                                                            SpDeviceInterfaceData *DeviceInterfaceData,
                                                            SpDeviceInterfaceDetailDataW *DeviceInterfaceDetailData,
                                                            DWORD DeviceInterfaceDetailDataSize,
                                                            DWORD *RequiredSize, void *DeviceInfoData);
__declspec(dllimport) BOOL SetupDiDestroyDeviceInfoList(HDEVINFO DeviceInfoSet);

/* ws2_32.dll */
__declspec(dllimport) int    WSAStartup(uint16_t wVersionRequested, void *lpWSAData);
__declspec(dllimport) SOCKET socket(int af, int type, int protocol);
__declspec(dllimport) int    setsockopt(SOCKET s, int level, int optname, const void *optval, int optlen);
__declspec(dllimport) int    connect(SOCKET s, const SockAddrIn *name, int namelen);
__declspec(dllimport) int    send(SOCKET s, const char *buf, int len, int flags);
__declspec(dllimport) int    recv(SOCKET s, char *buf, int len, int flags);
__declspec(dllimport) int    closesocket(SOCKET s);

/* user32.dll */
__declspec(dllimport) BOOL SetProcessDpiAwarenessContext(HANDLE value);
__declspec(dllimport) uint32_t GetDpiForSystem(void);

/* ole32.dll */
__declspec(dllimport) int32_t CoInitializeEx(void *pvReserved, DWORD dwCoInit);
__declspec(dllimport) int32_t CoCreateInstance(const Guid *rclsid, void *pUnkOuter, DWORD dwClsContext,
                                               const Guid *riid, void **ppv);

/* oleaut32.dll */
typedef struct {
    uint16_t vt, reserved1, reserved2, reserved3;
    union { int16_t boolVal; int32_t lVal; void *parray; void *punkVal; uint8_t pad[16]; } u; /* 16: VARIANT's BRECORD arm */
} Variant;
typedef struct {
    uint32_t cElements;
    int32_t  lLbound;
} SafeArrayBound;
typedef struct {
    uint16_t       cDims, fFeatures;
    uint32_t       cbElements, cLocks;
    void          *pvData;
    SafeArrayBound rgsabound[1];
} SafeArray;
_Static_assert(sizeof(Variant) == 24, "VARIANT on 64-bit Windows");
_Static_assert(offsetof(Variant, u) == 8, "VARIANT value offset");
_Static_assert(sizeof(SafeArray) == 32, "SAFEARRAY with one dimension on 64-bit Windows");
_Static_assert(offsetof(SafeArray, pvData) == 16, "SAFEARRAY pvData offset on 64-bit Windows");
#define VT_I4    3
#define VT_R8    5
#define VT_BOOL  11
#define VT_UNKNOWN 13
#define VT_ARRAY 0x2000
__declspec(dllimport) int32_t VariantClear(Variant *pvarg);
__declspec(dllimport) int32_t SafeArrayAccessData(SafeArray *psa, void **ppvData);
__declspec(dllimport) int32_t SafeArrayUnaccessData(SafeArray *psa);

/* GUID_GPIOBUTTONS_LAPTOPSLATE_INTERFACE {317fc439-3f77-41c8-b09e-08ad63272aa3} */
static const Guid kLaptopSlateInterface = {
    0x317fc439U, 0x3f77U, 0x41c8U, {0xb0, 0x9e, 0x08, 0xad, 0x63, 0x27, 0x2a, 0xa3}
};

static void mem_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static uint16_t htons_u16(uint16_t v) {
    return (uint16_t)(((v >> 8) & 0xFFU) | ((v & 0xFFU) << 8));
}

static void u32_to_dec(uint32_t v, char *out) {
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v > 0 && n < 15);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static size_t append_str(char *out, size_t at, size_t cap, const char *s) {
    while (*s && at + 1 < cap) out[at++] = *s++;
    out[at] = 0;
    return at;
}

static size_t append_u64(char *out, size_t at, size_t cap, uint64_t v) {
    char num[24];
    int n = 0;
    do {
        num[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v > 0 && n < 23);
    num[n] = 0;
    for (int i = 0; i < n / 2; i++) {
        char t = num[i];
        num[i] = num[n - 1 - i];
        num[n - 1 - i] = t;
    }
    return append_str(out, at, cap, num);
}

/* Logs state changes only, and stops after LOG_LINES_MAX lines so a stuck loop can't fill the disk. */
static void log_line(const char *msg1, const char *msg2) {
    static int lines;
    if (lines >= LOG_LINES_MAX) return;
    lines++;

    WCHAR path[520];
    DWORD n = GetTempPathW(500, path);
    if (n == 0 || n > 480) return;
    const WCHAR *fn = u"winios_agent.log";
    for (size_t i = 0; fn[i] && n < 518; i++) path[n++] = fn[i];
    path[n] = 0;

    HANDLE hf = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    if (msg1 && msg1[0]) WriteFile(hf, msg1, (DWORD)str_len(msg1), &wr, NULL);
    if (msg2 && msg2[0]) WriteFile(hf, msg2, (DWORD)str_len(msg2), &wr, NULL);
    WriteFile(hf, "\r\n", 2, &wr, NULL);
    CloseHandle(hf);
}

static int send_all(SOCKET s, const char *buf, int len) {
    int off = 0;
    while (off < len) {
        int n = send(s, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

/* GET http://10.0.2.4<path>. Returns the body length (the body is copied to body, NUL-terminated) or -1 if the
   share can't be reached yet or does not answer 200. The host closes the connection after the reply. timeout_ms
   bounds each send and receive. */
static int share_get(const char *req, int req_len, char *body, int body_cap, DWORD timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;

    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    SockAddrIn sa;
    mem_zero(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons_u16(80);
    sa.sin_addr = IPV4_HENGE_SHARE;
    if (connect(s, &sa, sizeof(sa)) != 0) {
        closesocket(s);
        return -1;
    }
    if (send_all(s, req, req_len) != 0) {
        closesocket(s);
        return -1;
    }

    char resp[512];
    int have = 0;
    while (have < (int)sizeof(resp) - 1) {
        int n = recv(s, resp + have, (int)sizeof(resp) - 1 - have, 0);
        if (n <= 0) break;
        have += n;
    }
    closesocket(s);
    resp[have] = 0;

    /* "HTTP/1.1 200 ..." then the body after the blank line */
    int sp = 0;
    while (sp < have && resp[sp] != ' ') sp++;
    if (sp + 4 > have || resp[sp + 1] != '2' || resp[sp + 2] != '0' || resp[sp + 3] != '0') return -1;
    for (int i = 0; i + 4 <= have; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            int left = have - (i + 4);
            if (left > body_cap - 1) left = body_cap - 1;
            for (int k = 0; k < left; k++) body[k] = resp[i + 4 + k];
            body[left] = 0;
            return left;
        }
    }
    return -1;
}

static int starts_with(const char *s, int len, const char *word) {
    int n = (int)str_len(word);
    if (len < n) return 0;
    for (int i = 0; i < n; i++)
        if (s[i] != word[i]) return 0;
    return 1;
}

/* 1 laptop, 0 tablet, -1 if the share can't be reached yet or answers anything else. The request says what scale
   this session runs at (Windows fixed it at sign-in, before the agent started; 96 dots per inch is 100 percent).
   The reply is the posture word, then " scale=NNN" when the app wants a scale: *wanted_scale gets it, or 0. */
static int fetch_posture(int *wanted_scale) {
    static char req[112];
    static size_t req_len;
    if (!req_len) {
        req_len = append_str(req, 0, sizeof(req), "GET /.henge/posture?scale=");
        req_len = append_u64(req, req_len, sizeof(req), agent_scale_percent(GetDpiForSystem()));
        req_len = append_str(req, req_len, sizeof(req), " HTTP/1.1\r\nHost: 10.0.2.4\r\nConnection: close\r\n\r\n");
    }
    char body[64];
    int n = share_get(req, (int)req_len, body, (int)sizeof(body), 1500);
    int laptop = starts_with(body, n, "laptop");
    if (!laptop && !starts_with(body, n, "tablet")) return -1;
    *wanted_scale = agent_scale_parse(body + 6, n - 6); /* both words are six letters */
    return laptop;
}

/* 1 when the app asked Windows to hibernate (the host answers that once), else 0. */
static int fetch_hibernate(void) {
    static const char req[] = "GET /.henge/power HTTP/1.1\r\nHost: 10.0.2.4\r\nConnection: close\r\n\r\n";
    char body[64];
    int n = share_get(req, (int)sizeof(req) - 1, body, (int)sizeof(body), 1500);
    return starts_with(body, n, "hibernate");
}

static void hibernate(void) {
    WCHAR cmd[] = u"C:\\Windows\\System32\\shutdown.exe /h";
    StartupInfoW si;
    ProcessInformation pi;
    mem_zero(&si, sizeof(si));
    mem_zero(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, 0, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        char num[16];
        u32_to_dec(GetLastError(), num);
        log_line("hibernate_failed ", num);
        return;
    }
    log_line("hibernate", "");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* ConvertibleSlateMode as Windows sees it: 1 laptop, 0 slate. The value is absent until the driver
   has loaded, and the driver starts in slate, so absent reads as 0. */
static int read_slate_mode(void) {
    HKEY hk = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, u"SYSTEM\\CurrentControlSet\\Control\\PriorityControl", 0,
                      KEY_QUERY_VALUE, &hk) != 0 || !hk) return 0;
    DWORD type = 0, data = 0, size = sizeof(data);
    LONG rc = RegQueryValueExW(hk, u"ConvertibleSlateMode", NULL, &type, &data, &size);
    RegCloseKey(hk);
    if (rc != 0 || type != REG_DWORD) return 0;
    return data ? 1 : 0;
}

typedef enum { T_OK, T_NO_DEVICE_SET, T_NO_INTERFACE, T_NO_DETAIL, T_OPEN, T_WRITE } ToggleResult;

static const char *toggle_result_name(ToggleResult r) {
    switch (r) {
    case T_NO_DEVICE_SET: return "toggle_failed SetupDiGetClassDevs ";
    case T_NO_INTERFACE:  return "toggle_failed no_indicator_interface ";
    case T_NO_DETAIL:     return "toggle_failed interface_detail ";
    case T_OPEN:          return "toggle_failed CreateFile ";
    case T_WRITE:         return "toggle_failed WriteFile ";
    default:              return "";
    }
}

/* One toggle of the indicator. The handle is exclusive, so it is held only for the write. On failure
   *err has the Win32 error of the step that failed (5 is access denied, 32 is another process holding it). */
static ToggleResult toggle_indicator(DWORD *err) {
    HDEVINFO set = SetupDiGetClassDevsW(&kLaptopSlateInterface, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return T_NO_DEVICE_SET;
    }

    ToggleResult rc = T_NO_INTERFACE;
    SpDeviceInterfaceData did;
    mem_zero(&did, sizeof(did));
    did.cbSize = sizeof(did);
    if (!SetupDiEnumDeviceInterfaces(set, NULL, &kLaptopSlateInterface, 0, &did)) {
        *err = GetLastError();
    } else {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &did, NULL, 0, &need, NULL);
        *err = GetLastError();
        rc = T_NO_DETAIL;
        if (need >= sizeof(SpDeviceInterfaceDetailDataW) && *err == ERROR_INSUFFICIENT_BUFFER) {
            SpDeviceInterfaceDetailDataW *detail =
                (SpDeviceInterfaceDetailDataW *)HeapAlloc(GetProcessHeap(), 8 /*HEAP_ZERO_MEMORY*/, need);
            if (detail) {
                detail->cbSize = sizeof(SpDeviceInterfaceDetailDataW);
                if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, NULL, NULL)) {
                    *err = GetLastError();
                } else {
                    HANDLE h = CreateFileW(detail->DevicePath, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                    if (h == INVALID_HANDLE_VALUE) {
                        *err = GetLastError();
                        rc = T_OPEN;
                    } else {
                        uint8_t buffer = 0;
                        DWORD written = 0;
                        if (WriteFile(h, &buffer, sizeof(buffer), &written, NULL)) {
                            rc = T_OK;
                        } else {
                            *err = GetLastError();
                            rc = T_WRITE;
                        }
                        CloseHandle(h);
                    }
                }
                HeapFree(GetProcessHeap(), 0, detail);
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return rc;
}

/* ---- text focus ---- */

/* CLSID_CUIAutomation {ff48dba4-60ef-4201-aa87-54103eef594e} and IID_IUIAutomation {30cbe57d-d9d0-452a-ab13-7ac5ac4825ee},
   both from the CUIAutomation coclass and IUIAutomation interface in Wine's include/uiautomationclient.idl and in
   mingw-w64's generated uiautomationclient.h (DEFINE_GUID lines). */
static const Guid kClsidUIAutomation = {
    0xff48dba4U, 0x60efU, 0x4201U, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}
};
static const Guid kIidUIAutomation = {
    0x30cbe57dU, 0xd9d0U, 0x452aU, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}
};

/* vtable slots, counted from the method order in Wine's include/uiautomationclient.idl and checked against the
   Vtbl structs in mingw-w64's uiautomationclient.h (IUIAutomationVtbl, IUIAutomationElementVtbl).
   Every COM interface starts QueryInterface 0, AddRef 1, Release 2.
   IUIAutomation: CompareElements 3, CompareRuntimeIds 4, GetRootElement 5, ElementFromHandle 6,
     ElementFromPoint 7, GetFocusedElement 8.
   IUIAutomationElement: SetFocus 3, GetRuntimeId 4, FindFirst 5, FindAll 6, FindFirstBuildCache 7,
     FindAllBuildCache 8, BuildUpdatedCache 9, GetCurrentPropertyValue 10. */
#define COM_RELEASE                 2
#define UIA_GET_FOCUSED_ELEMENT     8
#define UIAE_GET_CURRENT_PROP_VALUE 10
/* Same sources. IUIAutomationElement goes on: GetCurrentPropertyValueEx 11, GetCachedPropertyValue 12,
   GetCachedPropertyValueEx 13, GetCurrentPatternAs 14.
   IUIAutomationTextPattern: RangeFromPoint 3, RangeFromChild 4, GetSelection 5, GetVisibleRanges 6,
     get_DocumentRange 7.
   IUIAutomationTextRange: Clone 3, Compare 4, CompareEndpoints 5, ExpandToEnclosingUnit 6, FindAttribute 7,
     FindText 8, GetAttributeValue 9. */
#define UIAE_GET_CURRENT_PATTERN_AS 14
#define UIATP_GET_DOCUMENT_RANGE    7
#define UIATR_GET_ATTRIBUTE_VALUE   9
/* IUIAutomation, much further on: ... CheckNotSupported 53, get_ReservedNotSupportedValue 54,
   get_ReservedMixedAttributeValue 55, ElementFromIAccessible 56. */
#define UIA_GET_RESERVED_MIXED      55
/* Pattern and text attribute ids, and IID_IUIAutomationTextPattern {32eba289-3583-42c9-9c59-3b6d9a1e9b6a},
   from the same IDL. */
#define UIA_TextPatternId         10014
#define UIA_IsReadOnlyAttributeId 40015
static const Guid kIidUIAutomationTextPattern = {
    0x32eba289U, 0x3583U, 0x42c9U, {0x9c, 0x59, 0x3b, 0x6d, 0x9a, 0x1e, 0x9b, 0x6a}
};

/* Property ids (UIAutomationClient.h, learn.microsoft.com .../winauto/uiauto-automation-element-propids) */
#define UIA_BoundingRectanglePropertyId       30001
#define UIA_ControlTypePropertyId             30003
#define UIA_IsEnabledPropertyId               30010
#define UIA_IsTextPatternAvailablePropertyId  30040
#define UIA_IsValuePatternAvailablePropertyId 30043
#define UIA_ValueIsReadOnlyPropertyId         30046
/* Control type ids (.../winauto/uiauto-controltype-ids) */
#define UIA_ComboBoxControlTypeId 50003
#define UIA_EditControlTypeId     50004
#define UIA_TextControlTypeId     50020
#define UIA_CustomControlTypeId   50025
#define UIA_GroupControlTypeId    50026
#define UIA_DocumentControlTypeId 50030
#define UIA_PaneControlTypeId     50033

typedef struct { void **vtbl; } ComObject;

static void com_release(void *obj) {
    if (obj) ((uint32_t (*)(void *))((ComObject *)obj)->vtbl[COM_RELEASE])(obj);
}

/* 1 only for a VT_BOOL true; anything else, including a failed call or another type, is 0. */
static int prop_bool(void *el, int id) {
    Variant v;
    mem_zero(&v, sizeof(v));
    int32_t hr = ((int32_t (*)(void *, int, Variant *))((ComObject *)el)->vtbl[UIAE_GET_CURRENT_PROP_VALUE])(el, id, &v);
    int r = hr >= 0 && v.vt == VT_BOOL && v.u.boolVal != 0;
    VariantClear(&v);
    return r;
}

/* The VT_I4 value of a property, or -1. */
static int32_t prop_int(void *el, int id) {
    Variant v;
    mem_zero(&v, sizeof(v));
    int32_t hr = ((int32_t (*)(void *, int, Variant *))((ComObject *)el)->vtbl[UIAE_GET_CURRENT_PROP_VALUE])(el, id, &v);
    int32_t r = hr >= 0 && v.vt == VT_I4 ? v.u.lVal : -1;
    VariantClear(&v);
    return r;
}

/* 1 when (x, y) is inside the element's BoundingRectangle (VT_R8 | VT_ARRAY: left, top, width, height in
   physical screen pixels) grown by TAP_SLOP_PX on each side. */
static int point_in_element(void *el, int x, int y) {
    Variant v;
    mem_zero(&v, sizeof(v));
    int32_t hr = ((int32_t (*)(void *, int, Variant *))((ComObject *)el)->vtbl[UIAE_GET_CURRENT_PROP_VALUE])(el, UIA_BoundingRectanglePropertyId, &v);
    int inside = 0;
    SafeArray *sa = (SafeArray *)v.u.parray;
    if (hr >= 0 && v.vt == (VT_R8 | VT_ARRAY) && sa && sa->cDims == 1 && sa->cbElements == 8 &&
        sa->rgsabound[0].cElements == 4) {
        double *r = NULL;
        if (SafeArrayAccessData(sa, (void **)&r) >= 0 && r) {
            double left = r[0], top = r[1], right = r[0] + r[2], bottom = r[1] + r[3];
            inside = x >= left - TAP_SLOP_PX && x <= right + TAP_SLOP_PX &&
                     y >= top - TAP_SLOP_PX && y <= bottom + TAP_SLOP_PX;
            SafeArrayUnaccessData(sa);
        }
    }
    VariantClear(&v);
    return inside;
}

/* What a text range answers for an attribute that differs along it: UI Automation's one "mixed" object, compared
   by identity. Fetched once (focus_thread); NULL if that failed, and then nothing counts as mixed. */
static void *uia_mixed_value;

/* 1 when the element's TextPattern says its text can be changed: the IsReadOnly attribute of the document range
   is false, or mixed (an editor with a fixed piece inside it, like a name chip in a message box). A web page's
   body and any viewer answer true there. An attribute the provider does not have counts as read-only. */
static int text_range_writable(void *el) {
    void *pattern = NULL, *range = NULL;
    int writable = 0;
    int32_t hr = ((int32_t (*)(void *, int, const Guid *, void **))((ComObject *)el)->vtbl[UIAE_GET_CURRENT_PATTERN_AS])(
        el, UIA_TextPatternId, &kIidUIAutomationTextPattern, &pattern);
    if (hr < 0 || !pattern) return 0;
    hr = ((int32_t (*)(void *, void **))((ComObject *)pattern)->vtbl[UIATP_GET_DOCUMENT_RANGE])(pattern, &range);
    if (hr >= 0 && range) {
        Variant v;
        mem_zero(&v, sizeof(v));
        hr = ((int32_t (*)(void *, int, Variant *))((ComObject *)range)->vtbl[UIATR_GET_ATTRIBUTE_VALUE])(
            range, UIA_IsReadOnlyAttributeId, &v);
        writable = hr >= 0 && ((v.vt == VT_BOOL && v.u.boolVal == 0) ||
                               (v.vt == VT_UNKNOWN && uia_mixed_value && v.u.punkVal == uia_mixed_value));
        VariantClear(&v);
        com_release(range);
    }
    com_release(pattern);
    return writable;
}

/* Whether the focused element takes text (*editable) and, for a tap at (x, y) (x < 0 for a look), whether the
   tap was inside it (*inside). Anything unexpected is "not editable". */
static void evaluate_focus(void *uia, int x, int y, int *editable, int *inside) {
    *editable = 0;
    *inside = 0;
    void *el = NULL;
    int32_t hr = ((int32_t (*)(void *, void **))((ComObject *)uia)->vtbl[UIA_GET_FOCUSED_ELEMENT])(uia, &el);
    if (hr < 0 || !el) return;

    /* IsKeyboardFocusable is not asked: the element has the focus, and the edit of a Win32 combo box (the
       Run box) answers false there while it does. */
    int ok = prop_bool(el, UIA_IsEnabledPropertyId);
    if (ok && prop_bool(el, UIA_IsValuePatternAvailablePropertyId) && prop_bool(el, UIA_ValueIsReadOnlyPropertyId)) ok = 0;
    if (ok) {
        int32_t type = prop_int(el, UIA_ControlTypePropertyId);
        if (type == UIA_EditControlTypeId) {
            *editable = 1;
        } else if (type == UIA_DocumentControlTypeId || type == UIA_TextControlTypeId || type == UIA_CustomControlTypeId ||
                   type == UIA_GroupControlTypeId || type == UIA_PaneControlTypeId || type == UIA_ComboBoxControlTypeId) {
            *editable = prop_bool(el, UIA_IsTextPatternAvailablePropertyId) && text_range_writable(el);
        }
    }
    if (*editable && x >= 0) *inside = point_in_element(el, x, y);
    com_release(el);
}

/* "<seq> <kind> <x> <y>" as focus-wait answers. Returns 0 and fills the outputs, or -1. kind: 0 none, 1 tap, 2 look. */
static int parse_focus_event(const char *s, uint64_t *seq, int *kind, int *x, int *y) {
    uint64_t v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 19) v = v * 10U + (uint64_t)(*s++ - '0'), digits++;
    if (!digits || *s != ' ') return -1;
    s++;
    if (starts_with(s, (int)str_len(s), "tap ")) *kind = 1, s += 4;
    else if (starts_with(s, (int)str_len(s), "look ")) *kind = 2, s += 5;
    else if (starts_with(s, (int)str_len(s), "none ")) *kind = 0, s += 5;
    else return -1;
    int *out[2] = {x, y};
    for (int i = 0; i < 2; i++) {
        int neg = *s == '-';
        if (neg) s++;
        int n = 0, got = 0;
        while (*s >= '0' && *s <= '9' && got < 9) n = n * 10 + (*s++ - '0'), got++;
        if (!got) return -1;
        *out[i] = neg ? -n : n;
        if (i == 0) {
            if (*s != ' ') return -1;
            s++;
        }
    }
    *seq = v;
    return 0;
}

static void focus_report(uint64_t seq, int editable, int inside) {
    char req[192];
    size_t at = append_str(req, 0, sizeof(req), "GET /.henge/focus?seq=");
    at = append_u64(req, at, sizeof(req), seq);
    at = append_str(req, at, sizeof(req), editable ? "&editable=1" : "&editable=0");
    at = append_str(req, at, sizeof(req), inside ? "&inside=1" : "&inside=0");
    at = append_str(req, at, sizeof(req), " HTTP/1.1\r\nHost: 10.0.2.4\r\nConnection: close\r\n\r\n");
    char body[16];
    share_get(req, (int)at, body, (int)sizeof(body), 1500);
}

/* The host answers a report with 204 and no body, which share_get reports as -1 (it wants a blank line then a
   body); that is fine, the answer is not used. */

static DWORD focus_thread(void *arg) {
    (void)arg;
    void *uia = NULL;
    int com_ok = 0, logged = 0;
    for (;;) {
        int32_t hr = com_ok ? 0 : CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (hr >= 0 || hr == RPC_E_CHANGED_MODE) {
            com_ok = 1;
            hr = CoCreateInstance(&kClsidUIAutomation, NULL, CLSCTX_INPROC_SERVER, &kIidUIAutomation, &uia);
            if (hr >= 0 && uia) break;
            uia = NULL;
        }
        if (!logged) {
            char hex[12] = "0x";
            for (int i = 0; i < 8; i++) hex[2 + i] = "0123456789abcdef"[((uint32_t)hr >> (28 - 4 * i)) & 15U];
            hex[10] = 0;
            log_line("focus_uia_failed ", hex);
            logged = 1;
        }
        Sleep(UIA_RETRY_MS);
    }

    ((int32_t (*)(void *, void **))((ComObject *)uia)->vtbl[UIA_GET_RESERVED_MIXED])(uia, &uia_mixed_value);

    uint64_t after = 0;
    int have_after = 0, ready = 0;
    for (;;) {
        char req[160];
        size_t at = append_str(req, 0, sizeof(req), "GET /.henge/focus-wait");
        if (have_after) {
            at = append_str(req, at, sizeof(req), "?after=");
            at = append_u64(req, at, sizeof(req), after);
        }
        at = append_str(req, at, sizeof(req), " HTTP/1.1\r\nHost: 10.0.2.4\r\nConnection: close\r\n\r\n");
        char body[64];
        int n = share_get(req, (int)at, body, (int)sizeof(body), FOCUS_WAIT_RECV_MS);
        uint64_t seq = 0;
        int kind = 0, x = -1, y = -1;
        if (n < 0 || parse_focus_event(body, &seq, &kind, &x, &y) != 0) {
            Sleep(FOCUS_RETRY_MS);
            continue;
        }
        after = seq;
        have_after = 1;
        if (!ready) {
            log_line("focus_ready", "");
            ready = 1;
        }
        if (kind == 0) continue;

        int looks = kind == 1 ? (int)(sizeof(kFocusLooksMs) / sizeof(kFocusLooksMs[0])) : 1;
        int sent = 0, sent_editable = 0, sent_inside = 0;
        for (int i = 0; i < looks; i++) {
            Sleep(kFocusLooksMs[i]);
            int editable, inside;
            evaluate_focus(uia, kind == 1 ? x : -1, y, &editable, &inside);
            if (!sent || editable != sent_editable || inside != sent_inside) {
                focus_report(seq, editable, inside);
                sent = 1;
                sent_editable = editable;
                sent_inside = inside;
            }
            if (editable && inside) break;
        }
    }
}

/* ---- display scale ---- */

/* Makes percent the custom scale under one Control Panel\Desktop key: 1 if it wrote it, 0 if it was there already,
   -1 if it could not. */
static int scale_write(HKEY root, const WCHAR *subkey, int percent) {
    HKEY hk = NULL;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hk) != 0 || !hk) return -1;
    const DWORD dpi = agent_scale_dpi(percent), on = 1;
    DWORD type = 0, data = 0, size = sizeof(data);
    int there = RegQueryValueExW(hk, u"LogPixels", NULL, &type, &data, &size) == 0 && type == REG_DWORD && data == dpi;
    type = 0, data = 0, size = sizeof(data);
    there = there && RegQueryValueExW(hk, u"Win8DpiScaling", NULL, &type, &data, &size) == 0 && type == REG_DWORD &&
            data == on;
    int result = 0;
    if (!there) {
        LONG a = RegSetValueExW(hk, u"LogPixels", 0, REG_DWORD, &dpi, sizeof(dpi));
        LONG b = RegSetValueExW(hk, u"Win8DpiScaling", 0, REG_DWORD, &on, sizeof(on));
        result = a == 0 && b == 0 ? 1 : -1;
    }
    RegCloseKey(hk);
    return result;
}

/* One posture round's share of the scale: writes down what the app wants when it first says or changes it, then
   looks again every SCALE_CHECK_ROUNDS rounds and puts it back if something else changed it (the Tools installer
   writes 150 percent). With nothing from the app, an app from before the setting, Windows' scale is left alone. */
static void scale_round(int wanted) {
    static int checked_for, rounds, writes_logged, failure_logged;
    if (!wanted) return;
    if (wanted == checked_for && ++rounds < SCALE_CHECK_ROUNDS) return;
    rounds = 0;
    checked_for = wanted;

    int wrote = scale_write(HKEY_CURRENT_USER, u"Control Panel\\Desktop", wanted);
    /* The sign-in screen's copy; nothing depends on it. */
    scale_write(HKEY_USERS, u".DEFAULT\\Control Panel\\Desktop", wanted);
    if (wrote > 0 && writes_logged < SCALE_LOGS_MAX) {
        char num[16];
        u32_to_dec((uint32_t)wanted, num);
        log_line("scale_written ", num);
        writes_logged++;
    } else if (wrote < 0 && !failure_logged) {
        log_line("scale_write_failed", "");
        failure_logged = 1;
    }
}

void WinMainCRTStartup(void) {
    /* Before any window or COM use: UI Automation then reports physical pixels. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Single instance. Any failure to get the mutex (including access denied on one an elevated copy
       made) means another agent is already running. */
    HANDLE mutex = CreateMutexW(NULL, 0, u"Global\\WindowsiOSAgent");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);

    uint8_t *wsa = (uint8_t *)HeapAlloc(GetProcessHeap(), 8, 512);
    if (!wsa || WSAStartup(0x0202, wsa) != 0) ExitProcess(1);
    HeapFree(GetProcessHeap(), 0, wsa);

    log_line("started", "");
    if (!CreateThread(NULL, 0, focus_thread, NULL, 0, NULL)) log_line("focus_thread_failed", "");
    uint64_t next_toggle_at = 0;
    int share_up = 0;
    ToggleResult last_failure = T_OK;
    DWORD last_failure_err = 0;

    for (;;) {
        int scale = 0;
        int want = fetch_posture(&scale);
        if (want < 0) {
            if (share_up) log_line("share_unreachable", "");
            share_up = 0;
        } else {
            if (!share_up) log_line(want ? "share_up posture=laptop" : "share_up posture=tablet", "");
            share_up = 1;
            if (fetch_hibernate()) hibernate();

            int have = read_slate_mode();
            uint64_t now = GetTickCount64();
            if (have != want && now >= next_toggle_at) {
                DWORD err = 0;
                ToggleResult tr = toggle_indicator(&err);
                if (tr != T_OK) {
                    if (tr != last_failure || err != last_failure_err) {
                        char num[16];
                        u32_to_dec(err, num);
                        log_line(toggle_result_name(tr), num);
                        last_failure = tr;
                        last_failure_err = err;
                    }
                    next_toggle_at = now + BACKOFF_FAILED_MS;
                } else {
                    last_failure = T_OK;
                    /* Wait for the driver to publish the new state; if it never does, don't toggle back
                       and forth on a stale reading. */
                    int landed = 0;
                    for (DWORD waited = 0; waited < VERIFY_MS && !landed; waited += 100) {
                        Sleep(100);
                        landed = read_slate_mode() == want;
                    }
                    log_line(landed ? "toggled" : "toggled_but_registry_unchanged", "");
                    next_toggle_at = landed ? 0 : GetTickCount64() + BACKOFF_NOEFFECT_MS;
                }
            }
            scale_round(scale);
        }
        Sleep(POLL_MS);
    }
}
