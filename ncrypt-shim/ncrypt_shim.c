/*
 * goley-revival-linux — Open-Source Windows CNG (ncrypt.dll) ECDSA_P256 Shim
 * & Optional Headless Launcher Bridge for Linux (Wine / Proton / UMU)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Why this exists:
 *   1. Goley Revival (`revival_launcher.exe` x64 & `revival_client.dll` x86) uses
 *      Windows CNG `ncrypt.dll` to create, persist, export, and sign session/device
 *      challenges with an `ECDSA_P256` keypair.
 *   2. Upstream Wine and Proton's built-in `ncrypt.dll` return `0x80090029`
 *      (`NTE_NOT_SUPPORTED`) for `ECDSA_P256` and do not persist keys across
 *      processes (`NCryptOpenKey` stub), causing login to fail with
 *      "Sunucuya ulaşılamadı. Lütfen tekrar deneyin".
 *   3. Wine's `bcrypt.dll` *does* implement `BCRYPT_ECDSA_P256_ALGORITHM` cleanly!
 *      This shim bridges `NCrypt*` calls to `BCrypt*` and persists the ECC key blob
 *      at `%LOCALAPPDATA%\Revival\device_key.bin` so both 64-bit and 32-bit game
 *      processes share the same persistent device key.
 *   4. When `GOLEY_HEADLESS=1` is set by `goley-revival-linux`, a lightweight
 *      Win32 thread cloaks `revival_launcher.exe`'s Qt6 window (0 alpha, hidden
 *      from taskbar/Wayland/X11, and `SW_HIDE` once the game launches so Qt6 uses
 *      0% background CPU) and executes IPC commands (`LOGIN`, `PLAY`, `LOGOUT`,
 *      `SLEEP_UI`, `WAKE_UI`, `QUIT`) on behalf of the native GTK4/Libadwaita UI.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>

#ifdef GOLEY_NOSTDLIB
void __main(void) {}
#endif

#define GOLEY_MAGIC 0x4E435250u /* "NCRP" */

#ifndef WS_EX_LAYERED
#define WS_EX_LAYERED 0x00080000L
#endif
#ifndef WS_EX_TOOLWINDOW
#define WS_EX_TOOLWINDOW 0x00000080L
#endif
#ifndef WS_EX_APPWINDOW
#define WS_EX_APPWINDOW 0x00040000L
#endif
#ifndef WS_EX_NOACTIVATE
#define WS_EX_NOACTIVATE 0x08000000L
#endif
#ifndef LWA_ALPHA
#define LWA_ALPHA 0x00000002
#endif

enum ObjType {
    OBJ_PROV = 1,
    OBJ_KEY  = 2
};

typedef struct {
    DWORD magic;
    DWORD type;
    BCRYPT_ALG_HANDLE hAlg;
    BCRYPT_KEY_HANDLE hKey;
    WCHAR keyName[128];
    BOOL finalized;
} GoleyNCryptObject;

static void zero_mem(void *ptr, SIZE_T len) {
    volatile BYTE *p = (volatile BYTE *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

static BOOL str_ends_with_i_w(const WCHAR *str, const WCHAR *suffix) {
    int slen = lstrlenW(str);
    int xlen = lstrlenW(suffix);
    int i;
    if (slen < xlen) return FALSE;
    for (i = 0; i < xlen; i++) {
        WCHAR a = str[slen - xlen + i];
        WCHAR b = suffix[i];
        if (a >= L'A' && a <= L'Z') a = (WCHAR)(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = (WCHAR)(b - L'A' + L'a');
        if (a != b) return FALSE;
    }
    return TRUE;
}

static BOOL str_starts_with_a(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return FALSE;
        str++;
        prefix++;
    }
    return TRUE;
}

static void get_revival_local_dir(WCHAR *outDir) {
    WCHAR appdata[MAX_PATH];
    zero_mem(appdata, sizeof(appdata));
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH)) {
        if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
            lstrcpyW(appdata, L"C:\\");
        }
    }
    wsprintfW(outDir, L"%ls\\Revival", appdata);
    CreateDirectoryW(outDir, NULL);
}

static void get_key_path(WCHAR *outPath) {
    WCHAR dir[MAX_PATH];
    get_revival_local_dir(dir);
    wsprintfW(outPath, L"%ls\\device_key.bin", dir);
}

/* =========================================================================
 * CNG (ncrypt.dll) API Implementation
 * ========================================================================= */

static void ensure_bridge_thread_started(void);

SECURITY_STATUS WINAPI NCryptOpenStorageProvider(
    NCRYPT_PROV_HANDLE *phProvider,
    const WCHAR *pszProviderName,
    DWORD dwFlags)
{
    GoleyNCryptObject *obj;
    (void)pszProviderName;
    (void)dwFlags;
    if (!phProvider) return (SECURITY_STATUS)0x80090027; /* NTE_INVALID_PARAMETER */

    ensure_bridge_thread_started();

    obj = (GoleyNCryptObject *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GoleyNCryptObject));
    if (!obj) return (SECURITY_STATUS)0x8009000E; /* NTE_NO_MEMORY */
    obj->magic = GOLEY_MAGIC;
    obj->type = OBJ_PROV;
    *phProvider = (NCRYPT_PROV_HANDLE)obj;
    return 0;
}

SECURITY_STATUS WINAPI NCryptCreatePersistedKey(
    NCRYPT_PROV_HANDLE hProvider,
    NCRYPT_KEY_HANDLE *phKey,
    const WCHAR *pszAlgId,
    const WCHAR *pszKeyName,
    DWORD dwLegacyKeySpec,
    DWORD dwFlags)
{
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    GoleyNCryptObject *obj;
    NTSTATUS status;
    (void)hProvider;
    (void)pszAlgId;
    (void)dwLegacyKeySpec;
    (void)dwFlags;

    if (!phKey) return (SECURITY_STATUS)0x80090027;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0);
    if (status != 0) return (SECURITY_STATUS)0x80090029; /* NTE_NOT_SUPPORTED */

    status = BCryptGenerateKeyPair(hAlg, &hKey, 256, 0);
    if (status != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return (SECURITY_STATUS)0x80090029;
    }

    obj = (GoleyNCryptObject *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GoleyNCryptObject));
    if (!obj) {
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return (SECURITY_STATUS)0x8009000E;
    }
    obj->magic = GOLEY_MAGIC;
    obj->type = OBJ_KEY;
    obj->hAlg = hAlg;
    obj->hKey = hKey;
    if (pszKeyName) lstrcpynW(obj->keyName, pszKeyName, 128);
    obj->finalized = FALSE;

    *phKey = (NCRYPT_KEY_HANDLE)obj;
    return 0;
}

SECURITY_STATUS WINAPI NCryptOpenKey(
    NCRYPT_PROV_HANDLE hProvider,
    NCRYPT_KEY_HANDLE *phKey,
    const WCHAR *pszKeyName,
    DWORD dwLegacyKeySpec,
    DWORD dwFlags)
{
    WCHAR keyPath[MAX_PATH];
    HANDLE hFile;
    DWORD fileSize, readBytes = 0;
    BYTE *buf;
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    GoleyNCryptObject *obj;
    NTSTATUS status;
    (void)hProvider;
    (void)dwLegacyKeySpec;
    (void)dwFlags;

    if (!phKey) return (SECURITY_STATUS)0x80090027;
    get_key_path(keyPath);

    hFile = CreateFileW(keyPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return (SECURITY_STATUS)0x80090016; /* NTE_BAD_KEYSET */
    }

    fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0 || fileSize > 4096) {
        CloseHandle(hFile);
        return (SECURITY_STATUS)0x80090016;
    }

    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, fileSize);
    if (!buf) {
        CloseHandle(hFile);
        return (SECURITY_STATUS)0x8009000E;
    }
    ReadFile(hFile, buf, fileSize, &readBytes, NULL);
    CloseHandle(hFile);

    if (readBytes != fileSize) {
        HeapFree(GetProcessHeap(), 0, buf);
        return (SECURITY_STATUS)0x80090016;
    }

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0);
    if (status != 0) {
        HeapFree(GetProcessHeap(), 0, buf);
        return (SECURITY_STATUS)0x80090029;
    }

    status = BCryptImportKeyPair(hAlg, NULL, BCRYPT_ECCPRIVATE_BLOB, &hKey, buf, fileSize, 0);
    HeapFree(GetProcessHeap(), 0, buf);

    if (status != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return (SECURITY_STATUS)0x80090016;
    }

    obj = (GoleyNCryptObject *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GoleyNCryptObject));
    if (!obj) {
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return (SECURITY_STATUS)0x8009000E;
    }
    obj->magic = GOLEY_MAGIC;
    obj->type = OBJ_KEY;
    obj->hAlg = hAlg;
    obj->hKey = hKey;
    if (pszKeyName) lstrcpynW(obj->keyName, pszKeyName, 128);
    obj->finalized = TRUE;

    *phKey = (NCRYPT_KEY_HANDLE)obj;
    return 0;
}

SECURITY_STATUS WINAPI NCryptSetProperty(
    NCRYPT_HANDLE hObject,
    const WCHAR *pszProperty,
    BYTE *pbInput,
    DWORD cbInput,
    DWORD dwFlags)
{
    (void)hObject;
    (void)pszProperty;
    (void)pbInput;
    (void)cbInput;
    (void)dwFlags;
    return 0;
}

SECURITY_STATUS WINAPI NCryptGetProperty(
    NCRYPT_HANDLE hObject,
    const WCHAR *pszProperty,
    BYTE *pbOutput,
    DWORD cbOutput,
    DWORD *pcbResult,
    DWORD dwFlags)
{
    (void)hObject;
    (void)pszProperty;
    (void)pbOutput;
    (void)cbOutput;
    (void)dwFlags;
    if (pcbResult) *pcbResult = 0;
    return (SECURITY_STATUS)0x80090029;
}

SECURITY_STATUS WINAPI NCryptFinalizeKey(NCRYPT_KEY_HANDLE hKey, DWORD dwFlags) {
    GoleyNCryptObject *obj = (GoleyNCryptObject *)hKey;
    NTSTATUS status;
    ULONG blobSize = 0;
    (void)dwFlags;

    if (!obj || obj->magic != GOLEY_MAGIC || obj->type != OBJ_KEY)
        return (SECURITY_STATUS)0x80090027;

    status = BCryptFinalizeKeyPair(obj->hKey, 0);
    if (status != 0) return (SECURITY_STATUS)0x80090029;
    obj->finalized = TRUE;

    status = BCryptExportKey(obj->hKey, NULL, BCRYPT_ECCPRIVATE_BLOB, NULL, 0, &blobSize, 0);
    if (status == 0 && blobSize > 0) {
        BYTE *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, blobSize);
        if (buf) {
            status = BCryptExportKey(obj->hKey, NULL, BCRYPT_ECCPRIVATE_BLOB, buf, blobSize, &blobSize, 0);
            if (status == 0) {
                WCHAR keyPath[MAX_PATH];
                HANDLE hFile;
                get_key_path(keyPath);
                hFile = CreateFileW(keyPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(hFile, buf, blobSize, &written, NULL);
                    CloseHandle(hFile);
                }
            }
            HeapFree(GetProcessHeap(), 0, buf);
        }
    }
    return 0;
}

SECURITY_STATUS WINAPI NCryptExportKey(
    NCRYPT_KEY_HANDLE hKey,
    NCRYPT_KEY_HANDLE hExportKey,
    const WCHAR *pszBlobType,
    NCryptBufferDesc *pParameterList,
    BYTE *pbOutput,
    DWORD cbOutput,
    DWORD *pcbResult,
    DWORD dwFlags)
{
    GoleyNCryptObject *obj = (GoleyNCryptObject *)hKey;
    ULONG res = 0;
    NTSTATUS status;
    (void)hExportKey;
    (void)pszBlobType;
    (void)pParameterList;
    (void)dwFlags;

    if (!obj || obj->magic != GOLEY_MAGIC || obj->type != OBJ_KEY)
        return (SECURITY_STATUS)0x80090027;

    if (!pbOutput || cbOutput == 0) {
        status = BCryptExportKey(obj->hKey, NULL, BCRYPT_ECCPUBLIC_BLOB, NULL, 0, &res, 0);
        if (pcbResult) *pcbResult = res;
        return (status == 0) ? 0 : (SECURITY_STATUS)0x80090029;
    }

    status = BCryptExportKey(obj->hKey, NULL, BCRYPT_ECCPUBLIC_BLOB, pbOutput, cbOutput, &res, 0);
    if (pcbResult) *pcbResult = res;
    return (status == 0) ? 0 : (SECURITY_STATUS)0x80090029;
}

SECURITY_STATUS WINAPI NCryptSignHash(
    NCRYPT_KEY_HANDLE hKey,
    void *pPaddingInfo,
    BYTE *pbHashValue,
    DWORD cbHashValue,
    BYTE *pbSignature,
    DWORD cbSignature,
    DWORD *pcbResult,
    DWORD dwFlags)
{
    GoleyNCryptObject *obj = (GoleyNCryptObject *)hKey;
    ULONG res = 0;
    NTSTATUS status;
    (void)pPaddingInfo;
    (void)dwFlags;

    if (!obj || obj->magic != GOLEY_MAGIC || obj->type != OBJ_KEY)
        return (SECURITY_STATUS)0x80090027;

    if (!pbSignature || cbSignature == 0) {
        status = BCryptSignHash(obj->hKey, NULL, pbHashValue, cbHashValue, NULL, 0, &res, 0);
        if (pcbResult) *pcbResult = res;
        return (status == 0) ? 0 : (SECURITY_STATUS)0x80090029;
    }

    status = BCryptSignHash(obj->hKey, NULL, pbHashValue, cbHashValue, pbSignature, cbSignature, &res, 0);
    if (pcbResult) *pcbResult = res;
    return (status == 0) ? 0 : (SECURITY_STATUS)0x80090029;
}

SECURITY_STATUS WINAPI NCryptFreeObject(NCRYPT_HANDLE hObject) {
    GoleyNCryptObject *obj = (GoleyNCryptObject *)hObject;
    if (!obj || obj->magic != GOLEY_MAGIC)
        return (SECURITY_STATUS)0x80090027;

    if (obj->type == OBJ_KEY) {
        if (obj->hKey) BCryptDestroyKey(obj->hKey);
        if (obj->hAlg) BCryptCloseAlgorithmProvider(obj->hAlg, 0);
    }
    obj->magic = 0;
    HeapFree(GetProcessHeap(), 0, obj);
    return 0;
}

SECURITY_STATUS WINAPI NCryptImportKey(
    NCRYPT_PROV_HANDLE hProvider,
    NCRYPT_KEY_HANDLE hImportKey,
    const WCHAR *pszBlobType,
    NCryptBufferDesc *pParameterList,
    NCRYPT_KEY_HANDLE *phKey,
    BYTE *pbData,
    DWORD cbData,
    DWORD dwFlags)
{
    (void)hProvider; (void)hImportKey; (void)pszBlobType;
    (void)pParameterList; (void)phKey; (void)pbData;
    (void)cbData; (void)dwFlags;
    return (SECURITY_STATUS)0x80090029;
}

SECURITY_STATUS WINAPI NCryptDeleteKey(NCRYPT_KEY_HANDLE hKey, DWORD dwFlags) {
    WCHAR keyPath[MAX_PATH];
    (void)dwFlags;
    get_key_path(keyPath);
    DeleteFileW(keyPath);
    return NCryptFreeObject(hKey);
}

/* =========================================================================
 * Headless Bridge for `revival_launcher.exe` (activated via GOLEY_HEADLESS=1)
 * ========================================================================= */

typedef struct {
    DWORD pid;
    HWND found;
} FindLauncherCtx;

static BOOL CALLBACK enum_launcher_windows_cb(HWND hwnd, LPARAM lParam) {
    FindLauncherCtx *ctx = (FindLauncherCtx *)lParam;
    DWORD wndPid = 0;
    WCHAR title[128];
    WCHAR cls[128];
    RECT rc;

    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return TRUE;

    GetWindowThreadProcessId(hwnd, &wndPid);
    if (wndPid != ctx->pid) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    zero_mem(title, sizeof(title));
    zero_mem(cls, sizeof(cls));
    GetWindowTextW(hwnd, title, 127);
    GetClassNameW(hwnd, cls, 127);

    if (lstrcmpiW(title, L"Revival") == 0 ||
        (cls[0] == L'Q' && cls[1] == L't')) {
        zero_mem(&rc, sizeof(rc));
        GetClientRect(hwnd, &rc);
        if ((rc.right - rc.left) >= 400 && (rc.bottom - rc.top) >= 300) {
            ctx->found = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND find_launcher_hwnd(void) {
    FindLauncherCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.found = NULL;
    EnumWindows(enum_launcher_windows_cb, (LPARAM)&ctx);
    return ctx.found;
}

static HWND g_lastCloakedHwnd = NULL;

static void cloak_launcher_hwnd(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (g_lastCloakedHwnd == hwnd) return;

    SetWindowPos(
        hwnd,
        HWND_BOTTOM,
        -32000,
        -32000,
        0,
        0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER
    );
    g_lastCloakedHwnd = hwnd;
}

static void write_bridge_status(const char *msg) {
    WCHAR dir[MAX_PATH];
    WCHAR path[MAX_PATH];
    HANDLE hFile;
    get_revival_local_dir(dir);
    wsprintfW(path, L"%ls\\goley_bridge_status.txt", dir);
    hFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, msg, (DWORD)lstrlenA(msg), &written, NULL);
        CloseHandle(hFile);
    }
}

static void focus_qwindow_internal(HWND hwnd) {
    SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
}

static void click_client_point(HWND hwnd, int x, int y) {
    LPARAM lp = MAKELPARAM(x, y);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    Sleep(30);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, lp);
}

static void clear_focused_textfield(HWND hwnd) {
    int i;
    PostMessageW(hwnd, WM_KEYDOWN, VK_END, 0);
    PostMessageW(hwnd, WM_KEYUP, VK_END, 0);
    for (i = 0; i < 64; i++) {
        PostMessageW(hwnd, WM_KEYDOWN, VK_BACK, 0);
        PostMessageW(hwnd, WM_KEYUP, VK_BACK, 0);
    }
    for (i = 0; i < 16; i++) {
        PostMessageW(hwnd, WM_KEYDOWN, VK_DELETE, 0);
        PostMessageW(hwnd, WM_KEYUP, VK_DELETE, 0);
    }
}

static void type_utf8_text(HWND hwnd, const char *utf8) {
    WCHAR wbuf[512];
    int n, i;
    zero_mem(wbuf, sizeof(wbuf));
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, 511);
    if (n <= 0) return;
    for (i = 0; i < n && wbuf[i] != 0; i++) {
        PostMessageW(hwnd, WM_CHAR, (WPARAM)wbuf[i], 0);
    }
}

static void get_launcher_dims(HWND hwnd, int *outW, int *outH) {
    RECT rc;
    zero_mem(&rc, sizeof(rc));
    if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) > 400 && (rc.bottom - rc.top) > 300) {
        *outW = rc.right - rc.left;
        *outH = rc.bottom - rc.top;
    } else {
        *outW = 988;
        *outH = 603;
    }
}

static DWORD WINAPI headless_bridge_thread(LPVOID param) {
    WCHAR allowWin[16];
    BOOL hideWindow = TRUE;
    BOOL uiSleeping = FALSE;
    WCHAR dir[MAX_PATH];
    WCHAR cmdPath[MAX_PATH];
    DWORD tick = 0;
    (void)param;

    /* Yield briefly before polling IPC */
    Sleep(30);

    zero_mem(allowWin, sizeof(allowWin));
    if (GetEnvironmentVariableW(L"GOLEY_ALLOW_WINDOW", allowWin, 15) > 0 && allowWin[0] == L'1') {
        hideWindow = FALSE;
    }

    get_revival_local_dir(dir);
    wsprintfW(cmdPath, L"%ls\\goley_bridge_cmd.txt", dir);
    write_bridge_status("BRIDGE_READY\n");

    for (;;) {
        HWND hwnd = find_launcher_hwnd();
        if (hwnd && hideWindow) {
            if (uiSleeping) {
                if (IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_HIDE);
                }
            } else {
                cloak_launcher_hwnd(hwnd);
            }
        }

        /* Check for IPC commands every ~50ms */
        if ((tick % 2) == 0) {
            HANDLE hCmd = CreateFileW(
                cmdPath,
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL,
                OPEN_EXISTING,
                0,
                NULL
            );
            if (hCmd != INVALID_HANDLE_VALUE) {
                char buf[1024];
                DWORD rd = 0;
                zero_mem(buf, sizeof(buf));
                ReadFile(hCmd, buf, sizeof(buf) - 1, &rd, NULL);
                CloseHandle(hCmd);
                DeleteFileW(cmdPath);

                /* Strip trailing CR/LF */
                while (rd > 0 && (buf[rd - 1] == '\n' || buf[rd - 1] == '\r')) {
                    buf[--rd] = 0;
                }

                if (str_starts_with_a(buf, "QUIT")) {
                    write_bridge_status("ACK_QUIT\n");
                    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    Sleep(100);
                    ExitProcess(0);
                } else if (str_starts_with_a(buf, "SLEEP_UI")) {
                    uiSleeping = TRUE;
                    g_lastCloakedHwnd = NULL;
                    if (hwnd && hideWindow) ShowWindow(hwnd, SW_HIDE);
                    write_bridge_status("ACK_SLEEP_UI\n");
                } else if (str_starts_with_a(buf, "WAKE_UI")) {
                    uiSleeping = FALSE;
                    g_lastCloakedHwnd = NULL;
                    if (hwnd && hideWindow) {
                        ShowWindow(hwnd, SW_SHOWNA);
                        cloak_launcher_hwnd(hwnd);
                    }
                    write_bridge_status("ACK_WAKE_UI\n");
                } else if (str_starts_with_a(buf, "PLAY") && hwnd) {
                    int w, h;
                    if (uiSleeping) {
                        uiSleeping = FALSE;
                        g_lastCloakedHwnd = NULL;
                        ShowWindow(hwnd, SW_SHOWNA);
                        if (hideWindow) cloak_launcher_hwnd(hwnd);
                        Sleep(80);
                    }
                    get_launcher_dims(hwnd, &w, &h);
                    focus_qwindow_internal(hwnd);
                    Sleep(20);
                    /* StartButton center in Main.qml footer actionSlot */
                    click_client_point(hwnd, w - 100, h - 66);
                    write_bridge_status("ACK_PLAY\n");
                } else if (str_starts_with_a(buf, "LOGIN\t") && hwnd) {
                    /* Format: LOGIN\t<username>\t<password> */
                    char *user = buf + 6;
                    char *tab = user;
                    char *pass = NULL;
                    int w, h, yField;

                    while (*tab && *tab != '\t') tab++;
                    if (*tab == '\t') {
                        *tab = 0;
                        pass = tab + 1;
                    }

                    if (pass) {
                        if (uiSleeping) {
                            uiSleeping = FALSE;
                            g_lastCloakedHwnd = NULL;
                            ShowWindow(hwnd, SW_SHOWNA);
                            if (hideWindow) cloak_launcher_hwnd(hwnd);
                            Sleep(80);
                        }
                        get_launcher_dims(hwnd, &w, &h);
                        yField = h - 80;
                        focus_qwindow_internal(hwnd);
                        Sleep(25);

                        /* 1. Fill userField (x = w - 390, y = h - 80) */
                        click_client_point(hwnd, w - 390, yField);
                        Sleep(45);
                        clear_focused_textfield(hwnd);
                        Sleep(30);
                        type_utf8_text(hwnd, user);
                        Sleep(45);

                        /* 2. Fill passField (x = w - 226, y = h - 80) */
                        click_client_point(hwnd, w - 226, yField);
                        Sleep(45);
                        clear_focused_textfield(hwnd);
                        Sleep(30);
                        type_utf8_text(hwnd, pass);
                        Sleep(45);

                        /* 3. Submit via Enter + fallback click on submitBtn (x = w - 92, y = h - 80) */
                        PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
                        PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0);
                        Sleep(80);
                        click_client_point(hwnd, w - 92, yField);
                        write_bridge_status("ACK_LOGIN\n");
                    }
                }
            }
        }

        tick++;
        Sleep(50);
    }
    return 0;
}

static volatile LONG g_bridgeStarted = 0;

static void ensure_bridge_thread_started(void) {
    WCHAR headless[16];
    WCHAR exePath[MAX_PATH];
    if (InterlockedCompareExchange(&g_bridgeStarted, 1, 0) != 0) return;

    zero_mem(headless, sizeof(headless));
    zero_mem(exePath, sizeof(exePath));
    if (GetEnvironmentVariableW(L"GOLEY_HEADLESS", headless, 15) > 0 && headless[0] == L'1') {
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH) > 0 &&
            str_ends_with_i_w(exePath, L"revival_launcher.exe")) {
            HANDLE hThread = CreateThread(NULL, 0, headless_bridge_thread, NULL, 0, NULL);
            if (hThread) CloseHandle(hThread);
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        ensure_bridge_thread_started();
    }
    return TRUE;
}
