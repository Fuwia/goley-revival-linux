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

#define GOLEY_MAGIC 0x4E435250u      /* "NCRP" */
#define GOLEY_3DES_MAGIC 0x33444553u /* "3DES" */

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
    OBJ_PROV     = 1,
    OBJ_KEY      = 2,
    OBJ_3DES_ALG = 3,
    OBJ_3DES_KEY = 4
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

static void copy_mem(void *dst, const void *src, SIZE_T len) {
    volatile BYTE *d = (volatile BYTE *)dst;
    const volatile BYTE *s = (const volatile BYTE *)src;
    while (len--) {
        *d++ = *s++;
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

/* =========================================================================
 * Self-Contained 3DES-CBC Fallback & IAT Hook for `revival_launcher.exe`
 *
 * Why this exists:
 *   `revival_launcher.exe` (GameLauncher::startGame) encrypts the `NMRunEnv_ENUM`
 *   launch token using `bcrypt.dll` (`BCryptOpenAlgorithmProvider(L"3DES")`,
 *   `BCryptSetProperty(L"ChainingMode", L"ChainingModeCBC")`,
 *   `BCryptGenerateSymmetricKey`, `BCryptEncrypt`).
 *   On Wine/Proton setups where `libgnutls.so.30` is missing (optional dep on
 *   Arch Linux), `bcrypt.so` mismatches the prefix `bcrypt.dll`, or system
 *   GnuTLS priority disables legacy 3DES, `BCrypt` fails and causes:
 *     "GameLauncher: cannot start the game, preparing the launch data failed"
 *   By hooking `revival_launcher.exe`'s IAT for `bcrypt.dll` and handling
 *   `3DES` in pure C, launch data preparation succeeds on all Wine/Proton builds.
 * ========================================================================= */

static BOOL str_eq_i_a(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return FALSE;
    }
    return *a == *b;
}

static BOOL str_eq_a(const char *a, const char *b) {
    while (*a && *b) {
        if (*a++ != *b++) return FALSE;
    }
    return *a == *b;
}

static void patch_iat_slot(void **slot, void *replacement) {
    DWORD oldProt = 0;
    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldProt)) {
        *slot = replacement;
        VirtualProtect(slot, sizeof(void *), oldProt, &oldProt);
    }
}

#ifdef _WIN64

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef struct {
    DWORD magic;
    DWORD type;
    u64 k1[16];
    u64 k2[16];
    u64 k3[16];
} Goley3DesObject;

static const u8 DES_IP[64] = {
    58,50,42,34,26,18,10, 2, 60,52,44,36,28,20,12, 4,
    62,54,46,38,30,22,14, 6, 64,56,48,40,32,24,16, 8,
    57,49,41,33,25,17, 9, 1, 59,51,43,35,27,19,11, 3,
    61,53,45,37,29,21,13, 5, 63,55,47,39,31,23,15, 7
};
static const u8 DES_FP[64] = {
    40, 8,48,16,56,24,64,32, 39, 7,47,15,55,23,63,31,
    38, 6,46,14,54,22,62,30, 37, 5,45,13,53,21,61,29,
    36, 4,44,12,52,20,60,28, 35, 3,43,11,51,19,59,27,
    34, 2,42,10,50,18,58,26, 33, 1,41, 9,49,17,57,25
};
static const u8 DES_PC1[56] = {
    57,49,41,33,25,17, 9, 1,58,50,42,34,26,18,
    10, 2,59,51,43,35,27,19,11, 3,60,52,44,36,
    63,55,47,39,31,23,15, 7,62,54,46,38,30,22,
    14, 6,61,53,45,37,29,21,13, 5,28,20,12, 4
};
static const u8 DES_PC2[48] = {
    14,17,11,24, 1, 5, 3,28,15, 6,21,10,
    23,19,12, 4,26, 8,16, 7,27,20,13, 2,
    41,52,31,37,47,55,30,40,51,45,33,48,
    44,49,39,56,34,53,46,42,50,36,29,32
};
static const u8 DES_SHIFTS[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const u8 DES_E[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9,
     8, 9,10,11,12,13,12,13,14,15,16,17,
    16,17,18,19,20,21,20,21,22,23,24,25,
    24,25,26,27,28,29,28,29,30,31,32, 1
};
static const u8 DES_P[32] = {
    16, 7,20,21,29,12,28,17, 1,15,23,26, 5,18,31,10,
     2, 8,24,14,32,27, 3, 9,19,13,30, 6,22,11, 4,25
};
static const u8 DES_S[8][64] = {
    {14, 4,13, 1, 2,15,11, 8, 3,10, 6,12, 5, 9, 0, 7, 0,15, 7, 4,14, 2,13, 1,10, 6,12,11, 9, 5, 3, 8, 4, 1,14, 8,13, 6, 2,11,15,12, 9, 7, 3,10, 5, 0,15,12, 8, 2, 4, 9, 1, 7, 5,11, 3,14,10, 0, 6,13},
    {15, 1, 8,14, 6,11, 3, 4, 9, 7, 2,13,12, 0, 5,10, 3,13, 4, 7,15, 2, 8,14,12, 0, 1,10, 6, 9,11, 5, 0,14, 7,11,10, 4,13, 1, 5, 8,12, 6, 9, 3, 2,15,13, 8,10, 1, 3,15, 4, 2,11, 6, 7,12, 0, 5,14, 9},
    {10, 0, 9,14, 6, 3,15, 5, 1,13, 12, 7,11, 4, 2, 8,13, 7, 0, 9, 3, 4, 6,10, 2, 8, 5,14,12,11,15, 1,13, 6, 4, 9, 8,15, 3, 0,11, 1, 2,12, 5,10,14, 7, 1,10,13, 0, 6, 9, 8, 7, 4,15,14, 3,11, 5, 2,12},
    { 7,13,14, 3, 0, 6, 9,10, 1, 2, 8, 5,11,12, 4,15,13, 8,11, 5, 6,15, 0, 3, 4, 7, 2,12, 1,10,14, 9,10, 6, 9, 0,12,11, 7,13,15, 1, 3,14, 5, 2, 8, 4, 3,15, 0, 6,10, 1,13, 8, 9, 4, 5,11,12, 7, 2,14},
    { 2,12, 4, 1, 7,10,11, 6, 8, 5, 3,15,13, 0,14, 9,14,11, 2,12, 4, 7,13, 1, 5, 0,15,10, 3, 9, 8, 6, 4, 2, 1,11,10,13, 7, 8,15, 9,12, 5, 6, 3, 0,14,11, 8,12, 7, 1,14, 2,13, 6,15, 0, 9,10, 4, 5, 3},
    {12, 1,10,15, 9, 2, 6, 8, 0,13, 3, 4,14, 7, 5,11,10,15, 4, 2, 7,12, 9, 5, 6, 1,13,14, 0,11, 3, 8, 9,14,15, 5, 2, 8,12, 3, 7, 0, 4,10, 1,13,11, 6, 4, 3, 2,12, 9, 5,15,10,11,14, 1, 7, 6, 0, 8,13},
    { 4,11, 2,14,15, 0, 8,13, 3,12, 9, 7, 5,10, 6, 1,13, 0,11, 7, 4, 9, 1,10,14, 3, 5,12, 2,15, 8, 6, 1, 4,11,13,12, 3, 7,14,10,15, 6, 8, 0, 5, 9, 2, 6,11,13, 8, 1, 4,10, 7, 9, 5, 0,15,14, 2, 3,12},
    {13, 2, 8, 4, 6,15,11, 1,10, 9, 3,14, 5, 0,12, 7, 1,15,13, 8,10, 3, 7, 4,12, 5, 6,11, 0,14, 9, 2, 7,11, 4, 1, 9,12,14, 2, 0, 6,10,13,15, 3, 5, 8, 2, 1,14, 7, 4,10, 8,13,15,12, 9, 0, 3, 5, 6,11}
};

static u64 des_permute(u64 in, const u8 *table, int n_in, int n_out) {
    u64 out = 0;
    int i;
    for (i = 0; i < n_out; i++) {
        out = (out << 1) | ((in >> (n_in - table[i])) & 1ULL);
    }
    return out;
}

static void des_key_schedule(const u8 key8[8], u64 subkeys[16]) {
    u64 k = 0;
    u64 pc1;
    u32 c, d;
    int i;
    for (i = 0; i < 8; i++) k = (k << 8) | key8[i];
    pc1 = des_permute(k, DES_PC1, 64, 56);
    c = (u32)((pc1 >> 28) & 0x0FFFFFFFULL);
    d = (u32)(pc1 & 0x0FFFFFFFULL);
    for (i = 0; i < 16; i++) {
        int s = DES_SHIFTS[i];
        u64 cd;
        c = ((c << s) | (c >> (28 - s))) & 0x0FFFFFFFULL;
        d = ((d << s) | (d >> (28 - s))) & 0x0FFFFFFFULL;
        cd = (((u64)c) << 28) | (u64)d;
        subkeys[i] = des_permute(cd, DES_PC2, 56, 48);
    }
}

static u64 des_crypt_block(u64 block, const u64 subkeys[16], int decrypt) {
    u64 ip = des_permute(block, DES_IP, 64, 64);
    u32 l = (u32)(ip >> 32);
    u32 r = (u32)(ip & 0xFFFFFFFFULL);
    int round;
    for (round = 0; round < 16; round++) {
        u64 k = subkeys[decrypt ? (15 - round) : round];
        u64 er = des_permute((u64)r, DES_E, 32, 48) ^ k;
        u32 s_out = 0;
        u32 f, next_l;
        int box;
        for (box = 0; box < 8; box++) {
            u8 six = (u8)((er >> (42 - 6 * box)) & 0x3F);
            u8 row = (u8)(((six & 0x20) >> 4) | (six & 0x01));
            u8 col = (u8)((six >> 1) & 0x0F);
            s_out = (s_out << 4) | DES_S[box][row * 16 + col];
        }
        f = (u32)des_permute((u64)s_out, DES_P, 32, 32);
        next_l = r;
        r = l ^ f;
        l = next_l;
    }
    return des_permute((((u64)r) << 32) | (u64)l, DES_FP, 64, 64);
}

static NTSTATUS WINAPI hook_BCryptOpenAlgorithmProvider(
    BCRYPT_ALG_HANDLE *phAlgorithm,
    LPCWSTR pszAlgId,
    LPCWSTR pszImplementation,
    ULONG dwFlags)
{
    if (phAlgorithm && pszAlgId && lstrcmpW(pszAlgId, L"3DES") == 0) {
        Goley3DesObject *obj = (Goley3DesObject *)HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(Goley3DesObject)
        );
        if (!obj) return (NTSTATUS)0xC0000017L; /* STATUS_NO_MEMORY */
        obj->magic = GOLEY_3DES_MAGIC;
        obj->type = OBJ_3DES_ALG;
        *phAlgorithm = (BCRYPT_ALG_HANDLE)obj;
        return 0;
    }
    return BCryptOpenAlgorithmProvider(phAlgorithm, pszAlgId, pszImplementation, dwFlags);
}

static NTSTATUS WINAPI hook_BCryptSetProperty(
    BCRYPT_HANDLE hObject,
    LPCWSTR pszProperty,
    PUCHAR pbInput,
    ULONG cbInput,
    ULONG dwFlags)
{
    Goley3DesObject *obj = (Goley3DesObject *)hObject;
    if (obj && obj->magic == GOLEY_3DES_MAGIC) {
        return 0;
    }
    return BCryptSetProperty(hObject, pszProperty, pbInput, cbInput, dwFlags);
}

static NTSTATUS WINAPI hook_BCryptGenerateSymmetricKey(
    BCRYPT_ALG_HANDLE hAlgorithm,
    BCRYPT_KEY_HANDLE *phKey,
    PUCHAR pbKeyObject,
    ULONG cbKeyObject,
    PUCHAR pbSecret,
    ULONG cbSecret,
    ULONG dwFlags)
{
    Goley3DesObject *alg = (Goley3DesObject *)hAlgorithm;
    if (alg && alg->magic == GOLEY_3DES_MAGIC) {
        Goley3DesObject *keyObj;
        if (!phKey || !pbSecret || cbSecret < 16) {
            return (NTSTATUS)0xC000000DL; /* STATUS_INVALID_PARAMETER */
        }
        keyObj = (Goley3DesObject *)HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(Goley3DesObject)
        );
        if (!keyObj) return (NTSTATUS)0xC0000017L;
        keyObj->magic = GOLEY_3DES_MAGIC;
        keyObj->type = OBJ_3DES_KEY;
        des_key_schedule(pbSecret, keyObj->k1);
        des_key_schedule(pbSecret + 8, keyObj->k2);
        des_key_schedule((cbSecret >= 24) ? (pbSecret + 16) : pbSecret, keyObj->k3);
        *phKey = (BCRYPT_KEY_HANDLE)keyObj;
        return 0;
    }
    return BCryptGenerateSymmetricKey(hAlgorithm, phKey, pbKeyObject, cbKeyObject, pbSecret, cbSecret, dwFlags);
}

static NTSTATUS WINAPI hook_BCryptEncrypt(
    BCRYPT_KEY_HANDLE hKey,
    PUCHAR pbInput,
    ULONG cbInput,
    VOID *pPaddingInfo,
    PUCHAR pbIV,
    ULONG cbIV,
    PUCHAR pbOutput,
    ULONG cbOutput,
    ULONG *pcbResult,
    ULONG dwFlags)
{
    Goley3DesObject *keyObj = (Goley3DesObject *)hKey;
    if (keyObj && keyObj->magic == GOLEY_3DES_MAGIC) {
        u64 prev = 0;
        ULONG off;
        int i;
        if (keyObj->type != OBJ_3DES_KEY || !pcbResult || (cbInput & 7) != 0) {
            return (NTSTATUS)0xC000000DL;
        }
        *pcbResult = cbInput;
        if (!pbOutput) return 0;
        if (cbOutput < cbInput) return (NTSTATUS)0xC0000023L; /* STATUS_BUFFER_TOO_SMALL */
        if (pbIV && cbIV >= 8) {
            for (i = 0; i < 8; i++) prev = (prev << 8) | pbIV[i];
        }
        for (off = 0; off < cbInput; off += 8) {
            u64 blk = 0;
            u64 tmp;
            for (i = 0; i < 8; i++) blk = (blk << 8) | pbInput[off + i];
            blk ^= prev;
            blk = des_crypt_block(blk, keyObj->k1, 0);
            blk = des_crypt_block(blk, keyObj->k2, 1);
            blk = des_crypt_block(blk, keyObj->k3, 0);
            prev = blk;
            tmp = blk;
            for (i = 7; i >= 0; i--) {
                pbOutput[off + i] = (u8)(tmp & 0xFF);
                tmp >>= 8;
            }
        }
        if (pbIV && cbIV >= 8) {
            u64 tmp = prev;
            for (i = 7; i >= 0; i--) {
                pbIV[i] = (u8)(tmp & 0xFF);
                tmp >>= 8;
            }
        }
        return 0;
    }
    return BCryptEncrypt(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, pcbResult, dwFlags);
}

static NTSTATUS WINAPI hook_BCryptDestroyKey(BCRYPT_KEY_HANDLE hKey) {
    Goley3DesObject *keyObj = (Goley3DesObject *)hKey;
    if (keyObj && keyObj->magic == GOLEY_3DES_MAGIC) {
        zero_mem(keyObj, sizeof(Goley3DesObject));
        HeapFree(GetProcessHeap(), 0, keyObj);
        return 0;
    }
    return BCryptDestroyKey(hKey);
}

static NTSTATUS WINAPI hook_BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags) {
    Goley3DesObject *alg = (Goley3DesObject *)hAlgorithm;
    if (alg && alg->magic == GOLEY_3DES_MAGIC) {
        zero_mem(alg, sizeof(Goley3DesObject));
        HeapFree(GetProcessHeap(), 0, alg);
        return 0;
    }
    return BCryptCloseAlgorithmProvider(hAlgorithm, dwFlags);
}

static volatile LONG g_bcryptHooked = 0;

static void install_bcrypt_3des_hooks(void) {
    HMODULE hExe;
    BYTE *base;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    DWORD impRva;
    IMAGE_IMPORT_DESCRIPTOR *imp;

    if (InterlockedCompareExchange(&g_bcryptHooked, 1, 0) != 0) return;

    hExe = GetModuleHandleW(NULL);
    if (!hExe) return;
    base = (BYTE *)hExe;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRva) return;

    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + impRva);
    for (; imp->Name != 0; imp++) {
        const char *dllName = (const char *)(base + imp->Name);
        if (str_eq_i_a(dllName, "bcrypt.dll") && imp->OriginalFirstThunk && imp->FirstThunk) {
            IMAGE_THUNK_DATA *orig = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
            IMAGE_THUNK_DATA *iat  = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
            for (; orig->u1.AddressOfData != 0; orig++, iat++) {
                if (!(orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
                    const char *fn = (const char *)ibn->Name;
                    if (str_eq_a(fn, "BCryptOpenAlgorithmProvider")) {
                        patch_iat_slot((void **)&iat->u1.Function, (void *)&hook_BCryptOpenAlgorithmProvider);
                    } else if (str_eq_a(fn, "BCryptSetProperty")) {
                        patch_iat_slot((void **)&iat->u1.Function, (void *)&hook_BCryptSetProperty);
                    } else if (str_eq_a(fn, "BCryptGenerateSymmetricKey")) {
                        patch_iat_slot((void **)&iat->u1.Function, (void *)&hook_BCryptGenerateSymmetricKey);
                    } else if (str_eq_a(fn, "BCryptEncrypt")) {
                        patch_iat_slot((void **)&iat->u1.Function, (void *)&hook_BCryptEncrypt);
                    } else if (str_eq_a(fn, "BCryptDestroyKey")) {
                        patch_iat_slot((void **)&iat->u1.Function, (void *)&hook_BCryptDestroyKey);
                    } else if (str_eq_a(fn, "BCryptCloseAlgorithmProvider")) {
                        patch_iat_slot((void **)&iat->u1.Function, (void *)&hook_BCryptCloseAlgorithmProvider);
                    }
                }
            }
        }
    }
}

static void install_game_display_patches(void) {}

#else /* !_WIN64: 32-bit BinaryTrU.exe Fullscreen Resolution & Scaling Fix */

static void install_bcrypt_3des_hooks(void) {}

/*
 * Why Fullscreen Lower Resolutions Failed in BinaryTrU.exe:
 *   1. `revival_client.dll` hooks `Option.ini` (`0x00d324a9`) and
 *      `Resolution_ResetTestPane` (`0x00d37440`) to rewrite `FullScreen = 1`
 *      (Exclusive Fullscreen) to `FullScreen = 2` (Borderless Fullscreen
 *      `WS_POPUP` `0x96000000`, `D3DPRESENT_PARAMETERS.Windowed = TRUE`).
 *   2. However, in `BinaryTrU.exe`'s `ApplyDisplaySettings` (`0x00d375b0`),
 *      the `mode == 2` branch at `0x00d37867` executes:
 *        mov edi, [ebx+0x8c] ; overwrite target height with desktop height!
 *        mov esi, [ebx+0x88] ; overwrite target width with desktop width!
 *      which forces the D3D9 backbuffer and `Option->ScreenWidth/Height` back
 *      to the monitor's native desktop resolution (`1920x1080` / `1920x1200`).
 *   3. In `WndProc`, `WM_SIZE` (`0x00d38697`, `0x00d386e5`) and `WM_EXITSIZEMOVE`
 *      (`0x00d38f8b`) call `0x00d37550` with `GetClientRect` (`1920x1080`),
 *      which also overwrites `Option->ScreenWidth/Height` when the borderless
 *      window is resized to cover the screen.
 *   4. When rendering a lower backbuffer (e.g. `1280x720`) inside a fullscreen
 *      borderless window (`1920x1080`), D3D9 `Present(NULL, NULL, NULL, NULL)`
 *      stretches the backbuffer across the fullscreen window, but `0x00d36ae0`
 *      only scaled mouse coordinates when the window was `< 1024x768`.
 *   5. On Wayland/XWayland setups where XRandR only reports a single native
 *      resolution, `EnumDisplaySettingsW/ExW` needs to expose standard 16:9,
 *      16:10, and 4:3 modes (`>= 1024x768`) up to the native monitor size.
 */

static BOOL mem_eq(const BYTE *a, const BYTE *b, SIZE_T len) {
    SIZE_T i;
    for (i = 0; i < len; i++) {
        if (a[i] != b[i]) return FALSE;
    }
    return TRUE;
}

/* No CRT, allocation, or symbol loading in the exception path.  Wine maps Z:
 * to /, so this remains usable even when the game terminates before BugTrap. */
static HANDLE g_displayLog = INVALID_HANDLE_VALUE;

static void display_log(const char *event, const DWORD *values, SIZE_T count) {
    static const char hex[] = "0123456789abcdef";
    char line[512];
    SIZE_T n = 0, i;
    DWORD written;
    if (g_displayLog == INVALID_HANDLE_VALUE) return;
    while (*event && n < 80) line[n++] = *event++;
    for (i = 0; i < count && n + 10 < sizeof(line); i++) {
        int shift;
        line[n++] = ' ';
        for (shift = 28; shift >= 0; shift -= 4)
            line[n++] = hex[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
    WriteFile(g_displayLog, line, (DWORD)n, &written, NULL);
}

static LONG CALLBACK display_exception(EXCEPTION_POINTERS *ex) {
    CONTEXT *c = ex->ContextRecord;
    DWORD code = ex->ExceptionRecord->ExceptionCode;
    DWORD values[14];
    MEMORY_BASIC_INFORMATION mbi;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != 0xc0000374u && code != 0xc0000409u) return EXCEPTION_CONTINUE_SEARCH;
    values[0] = GetCurrentThreadId(); values[1] = code;
    values[2] = c->Eip; values[3] = c->Esp; values[4] = c->Ebp;
    values[5] = c->Eax; values[6] = c->Ebx; values[7] = c->Ecx;
    values[8] = c->Edx; values[9] = c->Esi; values[10] = c->Edi;
    values[11] = ex->ExceptionRecord->ExceptionInformation[0];
    values[12] = ex->ExceptionRecord->ExceptionInformation[1];
    values[13] = VirtualQuery((void *)c->Eip, &mbi, sizeof(mbi))
        ? (DWORD)(DWORD_PTR)mbi.AllocationBase : 0;
    display_log("exception tid code eip esp ebp eax ebx ecx edx esi edi access addr module", values, 14);
    if (VirtualQuery((void *)c->Esp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        SIZE_T count = ((DWORD_PTR)mbi.BaseAddress + mbi.RegionSize - c->Esp) / 4;
        if (count > 32) count = 32;
        display_log("stack", (DWORD *)c->Esp, count);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_display_diagnostics(void) {
    DWORD values[2] = { GetCurrentProcessId(), GetCurrentThreadId() };
    g_displayLog = CreateFileA("Z:\\tmp\\goley_display.log", FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    AddVectoredExceptionHandler(1, display_exception);
    display_log("display shim start pid tid", values, 2);
}

static volatile LONG g_displayResetCount;
static HRESULT WINAPI traced_d3d_reset(void *device, DWORD *params) {
    typedef HRESULT (WINAPI *ResetFn)(void *, DWORD *);
    ResetFn reset = (ResetFn)(*(void ***)device)[16];
    DWORD values[4];
    HRESULT hr;
    values[0] = InterlockedIncrement(&g_displayResetCount);
    values[1] = GetCurrentThreadId();
    display_log("reset begin seq tid", values, 2);
    display_log("present parameters", params, 14);
    hr = reset(device, params);
    values[2] = hr;
    display_log("reset end seq tid hr", values, 3);
    return hr;
}

static BOOL patch_code_bytes(BYTE *addr, const BYTE *expected, const BYTE *replacement, SIZE_T len) {
    DWORD oldProt = 0;
    SIZE_T i;
    if (expected && !mem_eq(addr, expected, len)) {
        return FALSE;
    }
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return FALSE;
    }
    for (i = 0; i < len; i++) {
        addr[i] = replacement[i];
    }
    VirtualProtect(addr, len, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return TRUE;
}

static void compute_render_and_window_dims(
    BYTE *winObj,
    int *outRenW,
    int *outRenH,
    int *outWinW,
    int *outWinH)
{
    int renW = 1024, renH = 768;
    int winW = 0, winH = 0;
    DWORD_PTR app = *(DWORD_PTR *)0x012baa04u;

    if (app) {
        DWORD_PTR ren = *(DWORD_PTR *)(app + 0x04);
        DWORD_PTR opt = *(DWORD_PTR *)(app + 0x1c);
        if (ren) {
            int rw = *(int *)(ren + 0x10);
            int rh = *(int *)(ren + 0x14);
            if (rw > 0 && rh > 0) {
                renW = rw;
                renH = rh;
            }
        } else if (opt) {
            int ow = *(int *)(opt + 0x08);
            int oh = *(int *)(opt + 0x0c);
            if (ow > 0 && oh > 0) {
                renW = ow;
                renH = oh;
            }
        }
        if (renW < 1024) renW = 1024;
        if (renH < 768)  renH = 768;

        if (winObj) {
            HWND hwnd = *(HWND *)(winObj + 0x04);
            RECT rc;
            zero_mem(&rc, sizeof(rc));
            if (hwnd && GetClientRect(hwnd, &rc) &&
                (rc.right - rc.left) > 0 && (rc.bottom - rc.top) > 0) {
                winW = rc.right - rc.left;
                winH = rc.bottom - rc.top;
            } else {
                int mode = opt ? *(int *)(opt + 0x14) : 0;
                if (mode == 2) {
                    winW = *(int *)(winObj + 0x88);
                    winH = *(int *)(winObj + 0x8c);
                } else {
                    winW = *(int *)(winObj + 0xc8);
                    winH = *(int *)(winObj + 0xcc);
                }
            }
        }
    }

    if (winW <= 0) winW = renW;
    if (winH <= 0) winH = renH;
    *outRenW = renW;
    *outRenH = renH;
    *outWinW = winW;
    *outWinH = winH;
}

static void WINAPI goley_scale_mouse_coords(
    BYTE *winObj,
    float *outX,
    float *outY,
    int mouseX,
    int mouseY)
{
    int renW, renH, winW, winH;
    compute_render_and_window_dims(winObj, &renW, &renH, &winW, &winH);
    if (outX) *outX = (float)mouseX * (float)renW / (float)winW;
    if (outY) *outY = (float)mouseY * (float)renH / (float)winH;
}

static void WINAPI goley_scale_wheel_event(
    BYTE *winObj,
    BYTE *mouseEvent)
{
    int renW, renH, winW, winH;
    float *px;
    float *py;
    if (!mouseEvent) return;
    px = (float *)(mouseEvent + 0x08);
    py = (float *)(mouseEvent + 0x0c);
    compute_render_and_window_dims(winObj, &renW, &renH, &winW, &winH);
    *px = (*px) * (float)renW / (float)winW;
    *py = (*py) * (float)renH / (float)winH;
}

typedef struct {
    DWORD w;
    DWORD h;
} GoleyStdMode;

static const GoleyStdMode GOLEY_STD_MODES[] = {
    { 1024,  768 },
    { 1152,  864 },
    { 1280,  720 },
    { 1280,  768 },
    { 1280,  800 },
    { 1280,  960 },
    { 1280, 1024 },
    { 1360,  768 },
    { 1366,  768 },
    { 1440,  900 },
    { 1600,  900 },
    { 1600, 1024 },
    { 1600, 1200 },
    { 1680, 1050 },
    { 1920, 1080 },
    { 1920, 1200 },
    { 2560, 1440 },
    { 2560, 1600 },
    { 3840, 2160 }
};

#define GOLEY_STD_MODE_COUNT (sizeof(GOLEY_STD_MODES) / sizeof(GOLEY_STD_MODES[0]))

static DWORD g_enumRealCount = 0;
static BOOL g_hasTemplateMode = FALSE;
static DEVMODEW g_templateMode;

static BOOL fill_synthetic_devmode(LPCWSTR lpszDeviceName, DWORD extraIdx, DEVMODEW *lpDevMode) {
    DWORD maxW = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    DWORD maxH = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    DWORD matchIdx = 0;
    SIZE_T i;

    if (!lpDevMode) return FALSE;
    if (maxW < 1024 || maxH < 768) {
        maxW = 1920;
        maxH = 1080;
    }

    for (i = 0; i < GOLEY_STD_MODE_COUNT; i++) {
        if (GOLEY_STD_MODES[i].w <= maxW && GOLEY_STD_MODES[i].h <= maxH) {
            if (matchIdx == extraIdx) {
                if (!g_hasTemplateMode) {
                    zero_mem(&g_templateMode, sizeof(g_templateMode));
                    g_templateMode.dmSize = sizeof(g_templateMode);
                    if (EnumDisplaySettingsW(lpszDeviceName, ENUM_CURRENT_SETTINGS, &g_templateMode)) {
                        g_hasTemplateMode = TRUE;
                    }
                }
                if (g_hasTemplateMode) {
                    copy_mem(lpDevMode, &g_templateMode, sizeof(*lpDevMode));
                } else {
                    zero_mem(lpDevMode, sizeof(*lpDevMode));
                    lpDevMode->dmSize = sizeof(*lpDevMode);
                }
                lpDevMode->dmPelsWidth = GOLEY_STD_MODES[i].w;
                lpDevMode->dmPelsHeight = GOLEY_STD_MODES[i].h;
                lpDevMode->dmBitsPerPel = 32;
                lpDevMode->dmDisplayFlags = 0;
                if (lpDevMode->dmDisplayFrequency < 50) {
                    lpDevMode->dmDisplayFrequency = 60;
                }
                lpDevMode->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                                      DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
                return TRUE;
            }
            matchIdx++;
        }
    }
    return FALSE;
}

static BOOL WINAPI hook_EnumDisplaySettingsW(
    LPCWSTR lpszDeviceName,
    DWORD iModeNum,
    DEVMODEW *lpDevMode)
{
    BOOL ok;
    if (iModeNum == (DWORD)-1 || iModeNum == (DWORD)-2) {
        return EnumDisplaySettingsW(lpszDeviceName, iModeNum, lpDevMode);
    }
    if (iModeNum == 0) {
        g_enumRealCount = 0;
    }
    ok = EnumDisplaySettingsW(lpszDeviceName, iModeNum, lpDevMode);
    if (ok) {
        if (iModeNum + 1 > g_enumRealCount) {
            g_enumRealCount = iModeNum + 1;
        }
        if (!g_hasTemplateMode && lpDevMode && lpDevMode->dmBitsPerPel == 32) {
            copy_mem(&g_templateMode, lpDevMode, sizeof(g_templateMode));
            g_hasTemplateMode = TRUE;
        }
        return TRUE;
    }
    if (iModeNum >= g_enumRealCount) {
        return fill_synthetic_devmode(lpszDeviceName, iModeNum - g_enumRealCount, lpDevMode);
    }
    return FALSE;
}

static BOOL WINAPI hook_EnumDisplaySettingsExW(
    LPCWSTR lpszDeviceName,
    DWORD iModeNum,
    DEVMODEW *lpDevMode,
    DWORD dwFlags)
{
    BOOL ok;
    if (iModeNum == (DWORD)-1 || iModeNum == (DWORD)-2) {
        return EnumDisplaySettingsExW(lpszDeviceName, iModeNum, lpDevMode, dwFlags);
    }
    if (iModeNum == 0) {
        g_enumRealCount = 0;
    }
    ok = EnumDisplaySettingsExW(lpszDeviceName, iModeNum, lpDevMode, dwFlags);
    if (ok) {
        if (iModeNum + 1 > g_enumRealCount) {
            g_enumRealCount = iModeNum + 1;
        }
        if (!g_hasTemplateMode && lpDevMode && lpDevMode->dmBitsPerPel == 32) {
            copy_mem(&g_templateMode, lpDevMode, sizeof(g_templateMode));
            g_hasTemplateMode = TRUE;
        }
        return TRUE;
    }
    if (iModeNum >= g_enumRealCount) {
        return fill_synthetic_devmode(lpszDeviceName, iModeNum - g_enumRealCount, lpDevMode);
    }
    return FALSE;
}

static void hook_module_user32_enum_display(HMODULE hMod) {
    BYTE *base;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    DWORD impRva;
    IMAGE_IMPORT_DESCRIPTOR *imp;

    if (!hMod) return;
    base = (BYTE *)hMod;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRva) return;

    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + impRva);
    for (; imp->Name != 0; imp++) {
        const char *dllName = (const char *)(base + imp->Name);
        if (str_eq_i_a(dllName, "user32.dll") && imp->OriginalFirstThunk && imp->FirstThunk) {
            IMAGE_THUNK_DATA *orig = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
            IMAGE_THUNK_DATA *iat  = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
            for (; orig->u1.AddressOfData != 0; orig++, iat++) {
                if (!(orig->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
                    const char *fn = (const char *)ibn->Name;
                    if (str_eq_a(fn, "EnumDisplaySettingsW")) {
                        patch_iat_slot((void **)&iat->u1.Function, (void *)&hook_EnumDisplaySettingsW);
                    } else if (str_eq_a(fn, "EnumDisplaySettingsExW")) {
                        patch_iat_slot((void **)&iat->u1.Function, (void *)&hook_EnumDisplaySettingsExW);
                    }
                }
            }
        }
    }
}

static volatile LONG g_gamePatched = 0;

static void install_game_display_patches(void) {
    WCHAR exePath[MAX_PATH];

    /* Always ensure d3d9.dll / wined3d.dll IATs are hooked if loaded */
    hook_module_user32_enum_display(GetModuleHandleW(L"d3d9.dll"));
    hook_module_user32_enum_display(GetModuleHandleW(L"wined3d.dll"));

    if (InterlockedCompareExchange(&g_gamePatched, 1, 0) != 0) return;

    zero_mem(exePath, sizeof(exePath));
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0 ||
        !str_ends_with_i_w(exePath, L"BinaryTrU.exe")) {
        return;
    }

    install_display_diagnostics();
    {
        WCHAR trace[2];
        static const BYTE expected[6] = {0x8b, 0x42, 0x40, 0x51, 0xff, 0xd0};
        /* Keep the following TEST EAX,EAX and its failure branch intact. */
        BYTE replacement[6] = {0x51, 0xe8, 0, 0, 0, 0};
        *(DWORD *)(replacement + 2) = (DWORD)(DWORD_PTR)&traced_d3d_reset - 0x009ab674u;
        if (GetEnvironmentVariableW(L"GOLEY_DISPLAY_TRACE", trace, 2) == 1 && trace[0] == L'1')
            patch_code_bytes((BYTE *)0x009ab66eu, expected, replacement, sizeof(replacement));
    }

    /*
     * 1. Patch ApplyDisplaySettings (0x00d375b0):
     *    - 0x00d37867: NOP out `mov edi, [ebx+0x8c]; mov esi, [ebx+0x88]` (12 bytes)
     *      so mode 2 (Borderless Fullscreen) keeps the user's selected resolution
     *      for the D3D9 backbuffer while the popup window covers the full monitor.
     *    - 0x00d37667: `mov BYTE PTR [ebp-0x42], 0` so UpdateWindowPos always runs.
     *    - 0x00d3797b: `mov BYTE PTR [ebp-0x41], 1` so UpdateWindowPos always runs
     *      at startup when Option.ini already has FullScreen=1 (-> 2).
     *    - 0x00d37a59: `cmp edi, 2` (was `cmp edi, 3`) so RendererMgr->is_windowed = 1.
     */
    {
        static const BYTE exp_d37867[12] = {
            0x8B, 0xBB, 0x8C, 0x00, 0x00, 0x00,
            0x8B, 0xB3, 0x88, 0x00, 0x00, 0x00
        };
        static const BYTE nop12[12] = {
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90
        };
        static const BYTE exp_d37667[4] = { 0xC6, 0x45, 0xBE, 0x01 };
        static const BYTE rep_d37667[4] = { 0xC6, 0x45, 0xBE, 0x00 };
        static const BYTE exp_d3797b[4] = { 0xC6, 0x45, 0xBF, 0x00 };
        static const BYTE rep_d3797b[4] = { 0xC6, 0x45, 0xBF, 0x01 };
        static const BYTE exp_d37a59[3] = { 0x83, 0xFF, 0x03 };
        static const BYTE rep_d37a59[3] = { 0x83, 0xFF, 0x02 };

        patch_code_bytes((BYTE *)0x00d37867u, exp_d37867, nop12, 12);
        patch_code_bytes((BYTE *)0x00d37667u, exp_d37667, rep_d37667, 4);
        patch_code_bytes((BYTE *)0x00d3797bu, exp_d3797b, rep_d3797b, 4);
        patch_code_bytes((BYTE *)0x00d37a59u, exp_d37a59, rep_d37a59, 3);
    }

    /*
     * 2. Patch WndProc WM_SIZE / WM_EXITSIZEMOVE calls to 0x00d37550:
     *    Prevents window manager resize notifications on the fullscreen borderless
     *    window from overwriting Option->ScreenWidth/Height with 1920x1080.
     */
    {
        static const BYTE exp_d38696[6] = { 0x53, 0xE8, 0xB4, 0xEE, 0xFF, 0xFF };
        static const BYTE exp_d386e4[6] = { 0x53, 0xE8, 0x66, 0xEE, 0xFF, 0xFF };
        static const BYTE exp_d38f8a[6] = { 0x53, 0xE8, 0xC0, 0xE5, 0xFF, 0xFF };
        static const BYTE nop6[6]       = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

        patch_code_bytes((BYTE *)0x00d38696u, exp_d38696, nop6, 6);
        patch_code_bytes((BYTE *)0x00d386e4u, exp_d386e4, nop6, 6);
        patch_code_bytes((BYTE *)0x00d38f8au, exp_d38f8a, nop6, 6);
    }

    /*
     * 3. Patch Mouse Coordinate Scaling in 0x00d36ae0 & WM_MOUSEWHEEL (0x00d38be5):
     *    Maps window client coordinates (e.g. 1920x1080) to D3D9 backbuffer render
     *    coordinates (e.g. 1280x720) so all Scaleform UI clicks, hovers, drags,
     *    and scroll wheel events align with the stretched fullscreen image.
     */
    {
        static const BYTE exp_d36ae0[25] = {
            0x55, 0x8B, 0xEC, 0x51, 0xA1, 0x04, 0xAA, 0x2B, 0x01,
            0x8B, 0x40, 0x1C, 0x8B, 0x40, 0x14, 0x85, 0xC0,
            0x0F, 0x84, 0x80, 0x00, 0x00, 0x00, 0x83, 0xF8
        };
        /* This space belongs to the original mouse function, which becomes
         * unreachable after replacing its entry. Verify it before reusing it. */
        static const BYTE exp_d36b00[18] = {
            0x1A, 0xDB, 0x45, 0x0C, 0xD9, 0x1E, 0x8B, 0xE5, 0x5D,
            0xC2, 0x08, 0x00, 0x83, 0xF8, 0x02, 0x0F, 0x85, 0xE2
        };
        static const BYTE exp_d38be5[5] = {
            0xE9, 0x1F, 0xFF, 0xFF, 0xFF
        };
        if (mem_eq((const BYTE *)0x00d36ae0u, exp_d36ae0, sizeof(exp_d36ae0)) &&
            mem_eq((const BYTE *)0x00d36b00u, exp_d36b00, sizeof(exp_d36b00)) &&
            mem_eq((const BYTE *)0x00d38be5u, exp_d38be5, sizeof(exp_d38be5))) {
            static const BYTE tpl_thunk1[] = {
                0x51,                         /* preserve ecx (custom game ABI) */
                0x52,                         /* preserve edx (custom game ABI) */
                0xFF, 0x74, 0x24, 0x10,       /* push DWORD PTR [esp+16] (mouseY) */
                0xFF, 0x74, 0x24, 0x10,       /* push DWORD PTR [esp+16] (mouseX) */
                0x56,                         /* push esi (outY) */
                0x52,                         /* push edx (outX) */
                0x51,                         /* push ecx (winObj) */
                0xB8, 0x00, 0x00, 0x00, 0x00, /* mov eax, goley_scale_mouse_coords */
                0xFF, 0xD0,                   /* call eax */
                0x5A,                         /* restore edx */
                0x59,                         /* restore ecx */
                0xC2, 0x08, 0x00              /* ret 8 */
            };
            static const BYTE tpl_thunk2[] = {
                0x51,                         /* push ecx (ScaleformUIMgr*) */
                0x52,                         /* push edx (&MouseEvent) */
                0x52,                         /* push edx (arg2: &MouseEvent) */
                0x53,                         /* push ebx (arg1: winObj) */
                0xB8, 0x00, 0x00, 0x00, 0x00, /* mov eax, goley_scale_wheel_event */
                0xFF, 0xD0,                   /* call eax */
                0x5A,                         /* pop edx */
                0x59,                         /* pop ecx */
                0xE9, 0x00, 0x00, 0x00, 0x00  /* jmp 0x00d38b09 */
            };
            BYTE thunk1[sizeof(tpl_thunk1)];
            BYTE thunk2[sizeof(tpl_thunk2)];
            BYTE jmpWheel[5];

            copy_mem(thunk1, tpl_thunk1, sizeof(thunk1));
            copy_mem(thunk2, tpl_thunk2, sizeof(thunk2));
            jmpWheel[0] = 0xE9;

            *(DWORD *)(thunk1 + 14)  = (DWORD)(DWORD_PTR)&goley_scale_mouse_coords;
            *(DWORD *)(thunk2 + 5)   = (DWORD)(DWORD_PTR)&goley_scale_wheel_event;
            /* E9 is at +13, its rel32 starts at +14, and the next instruction
             * is at +18.  v0.1.7 declared 19 bytes and wrote at +15, producing
             * a jump to 0x00f36112 and an AV on the first WM_MOUSEWHEEL. */
            *(DWORD *)(thunk2 + sizeof(thunk2) - sizeof(DWORD)) =
                (DWORD)(0x00d38b09u - (0x00d36b00u + sizeof(thunk2)));
            *(DWORD *)(jmpWheel + 1) = (DWORD)(0x00d36b00u - (0x00d38be5u + 5u));

            patch_code_bytes((BYTE *)0x00d36ae0u, exp_d36ae0, thunk1, sizeof(thunk1));
            patch_code_bytes((BYTE *)0x00d36b00u, exp_d36b00, thunk2, sizeof(thunk2));
            patch_code_bytes((BYTE *)0x00d38be5u, exp_d38be5, jmpWheel, sizeof(jmpWheel));
        }
    }
}

#endif

static volatile LONG g_bridgeStarted = 0;

static void ensure_bridge_thread_started(void) {
    WCHAR headless[16];
    WCHAR exePath[MAX_PATH];
    install_bcrypt_3des_hooks();
    install_game_display_patches();
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
