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
 */
#include <stdint.h>
#include <stddef.h>

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
#define HKEY_LOCAL_MACHINE   ((HKEY)(uintptr_t)0x80000002ULL)
#define KEY_QUERY_VALUE      0x0001U
#define REG_DWORD            4U
#define DIGCF_PRESENT        0x00000002U
#define DIGCF_DEVICEINTERFACE 0x00000010U
#define ERROR_INSUFFICIENT_BUFFER 122U

/* Network byte order IPv4 constant on little-endian ARM64 */
#define IPV4_HENGE_SHARE     0x0402000AU /* 10.0.2.4 */

#define POLL_MS              500U
#define VERIFY_MS            2000U
#define BACKOFF_FAILED_MS    3000U
#define BACKOFF_NOEFFECT_MS  10000U
#define LOG_LINES_MAX        200

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
   share can't be reached yet or does not answer 200. The host closes the connection after the reply. */
static int share_get(const char *req, int req_len, char *body, int body_cap) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;

    DWORD timeout_ms = 1500;
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

/* 1 laptop, 0 tablet, -1 if the share can't be reached yet or answers anything else. */
static int fetch_posture(void) {
    static const char req[] = "GET /.henge/posture HTTP/1.1\r\nHost: 10.0.2.4\r\nConnection: close\r\n\r\n";
    char body[64];
    int n = share_get(req, (int)sizeof(req) - 1, body, (int)sizeof(body));
    if (starts_with(body, n, "laptop")) return 1;
    if (starts_with(body, n, "tablet")) return 0;
    return -1;
}

/* 1 when the app asked Windows to hibernate (the host answers that once), else 0. */
static int fetch_hibernate(void) {
    static const char req[] = "GET /.henge/power HTTP/1.1\r\nHost: 10.0.2.4\r\nConnection: close\r\n\r\n";
    char body[64];
    int n = share_get(req, (int)sizeof(req) - 1, body, (int)sizeof(body));
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

void WinMainCRTStartup(void) {
    /* Single instance. Any failure to get the mutex (including access denied on one an elevated copy
       made) means another agent is already running. */
    HANDLE mutex = CreateMutexW(NULL, 0, u"Global\\WindowsiOSAgent");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);

    uint8_t *wsa = (uint8_t *)HeapAlloc(GetProcessHeap(), 8, 512);
    if (!wsa || WSAStartup(0x0202, wsa) != 0) ExitProcess(1);
    HeapFree(GetProcessHeap(), 0, wsa);

    log_line("started", "");
    uint64_t next_toggle_at = 0;
    int share_up = 0;
    ToggleResult last_failure = T_OK;
    DWORD last_failure_err = 0;

    for (;;) {
        int want = fetch_posture();
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
        }
        Sleep(POLL_MS);
    }
}
