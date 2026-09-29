#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <cwchar>

#include "OverlayWindow.hpp"

namespace {

// Sans ca, sur un ecran en 125/150 %, Windows "virtualise" les
// coordonnees et la fenetre peut atterrir au mauvais endroit.
void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (!user32)
        return;

    using SetContextFn = BOOL(WINAPI*)(HANDLE);

    auto setContext = reinterpret_cast<SetContextFn>(
        reinterpret_cast<void*>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext")
        )
    );

    // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (setContext && setContext(reinterpret_cast<HANDLE>(-4)))
        return;

    SetProcessDPIAware();
}

// Renvoie le pid passe apres "--pid", ou 0 s'il n'y en a pas.
DWORD ParsePidArgument() {
    int argc = 0;

    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (!argv)
        return 0;

    DWORD pid = 0;

    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--pid") == 0) {
            pid = static_cast<DWORD>(wcstoul(argv[i + 1], nullptr, 10));
            break;
        }
    }

    LocalFree(argv);

    return pid;
}

bool HasFlag(const wchar_t* flag) {
    int argc = 0;

    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (!argv)
        return false;

    bool found = false;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], flag) == 0) {
            found = true;
            break;
        }
    }

    LocalFree(argv);

    return found;
}

// Une seule instance a la fois. Si une autre tourne deja (ancien Discord,
// lancement manuel...), on lui demande de se fermer et on prend sa place.
// Renvoie nullptr si on n'a pas pu devenir l'instance principale.
HANDLE AcquireSingleInstance() {
    HANDLE mutex = CreateMutexW(
        nullptr,
        FALSE,
        L"Local\\DiscordTaskbarTypingOverlay"
    );

    if (!mutex)
        return nullptr;

    DWORD result = WaitForSingleObject(mutex, 0);

    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
        return mutex;

    HWND other = FindWindowW(OverlayWindow::WindowClassName(), nullptr);

    if (other)
        PostMessageW(other, WM_CLOSE, 0, 0);

    result = WaitForSingleObject(mutex, 5000);

    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
        return mutex;

    CloseHandle(mutex);

    return nullptr;
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int
) {
    EnableDpiAwareness();

    HANDLE singleInstance = AcquireSingleInstance();

    if (!singleInstance)
        return 0;

    OverlayWindow overlay;

    if (!overlay.Create(instance)) {
        MessageBoxW(
            nullptr,
            L"Impossible de démarrer l'overlay.\n"
            L"Vérifie qu'une autre application n'utilise pas déjà le port 27345.",
            L"Discord Taskbar Typing",
            MB_ICONERROR
        );

        return 1;
    }

    // Lance par le plugin : on suit le process principal de Discord
    // et on se ferme des qu'il se termine.
    const DWORD discordPid = ParsePidArgument();

    if (discordPid != 0 && !overlay.WatchProcess(discordPid))
        return 0;

    if (HasFlag(L"--test"))
        overlay.ShowTest();

    return overlay.Run();
}
