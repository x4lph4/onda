/// Onda launcher — root-folder stub exe.
/// Resolves the real binary relative to its own location so the repo
/// folder can move without breaking the shortcut:
///     <repo>\Onda.exe  →  <repo>\build\src\onda_artefacts\{Debug,Release}\Onda.exe
/// FFmpeg DLLs load from the artefact dir automatically; the app data
/// (onda.db, config) lives in %APPDATA%\Onda regardless of cwd.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev,
                    LPWSTR cmd, int show) {
    (void)inst; (void)prev; (void)cmd; (void)show;

    WCHAR target[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, target, MAX_PATH * 2);
    if (n == 0 || n == MAX_PATH * 2) return 1;
    WCHAR* slash = wcsrchr(target, L'\\');
    if (!slash) return 1;
    *slash = L'\0';

    const WCHAR* configs[] = { L"Debug", L"Release" };
    WCHAR exe[MAX_PATH * 2];
    const WCHAR* found = NULL;
    for (int i = 0; i < 2; ++i) {
        _snwprintf_s(exe, MAX_PATH * 2, _TRUNCATE,
                     L"%s\\build\\src\\onda_artefacts\\%s\\Onda.exe",
                     target, configs[i]);
        if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
            found = exe;
            break;
        }
    }
    if (!found) {
        MessageBoxW(NULL,
            L"Onda.exe no encontrado.\n\n"
            L"Compila primero:  build_devin.bat onda\n"
            L"(se esperaba en build\\src\\onda_artefacts\\Debug o Release)",
            L"Onda", MB_ICONERROR | MB_OK);
        return 1;
    }

    HINSTANCE r = ShellExecuteW(NULL, L"open", found, NULL, NULL,
                                SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        MessageBoxW(NULL, L"No se pudo lanzar Onda.exe",
                    L"Onda", MB_ICONERROR | MB_OK);
        return 1;
    }
    return 0;
}
