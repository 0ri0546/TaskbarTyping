#pragma once

#include <windows.h>

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool Create(HINSTANCE hInstance);

    // Boucle de messages. Renvoie le code de sortie.
    // Si WatchProcess() a ete appele, se ferme quand ce process se termine.
    int Run();

    // Surveille un process (ex : Discord). Renvoie false uniquement si ce
    // process n'existe deja plus.
    bool WatchProcess(DWORD pid);

    // Nom de classe de la fenetre (pour retrouver une autre instance).
    static const wchar_t* WindowClassName();

    // Affiche 3 pastilles fixes (lancer l'exe avec --test).
    void ShowTest();

private:
    static LRESULT CALLBACK WndProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam
    );

    // Redimensionne la fenetre selon le nombre d'avatars
    // et la place juste au-dessus de la barre des taches.
    void Layout(int userCount);

    // Affiche / cache la fenetre selon la liste des users.
    void UpdateUsers();

    // Appele par le timer : expiration + animation des points.
    void OnTick();

    void Draw(HDC hdc, int width, int height);

private:
    HWND m_hwnd = nullptr;
    bool m_wsaStarted = false;
    ULONG_PTR m_gdiplusToken = 0;
    HANDLE m_watchedProcess = nullptr;
};
