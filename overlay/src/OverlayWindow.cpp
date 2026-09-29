#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// winsock2.h AVANT windows.h (sinon conflit avec winsock.h)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <winhttp.h>

#include "OverlayWindow.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// gdiplus.h a besoin de min/max, et NOMINMAX les retire.
using std::min;
using std::max;

#include <gdiplus.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr wchar_t WINDOW_CLASS_NAME[] = L"DiscordTaskbarTypingOverlay";
constexpr wchar_t WINDOW_TITLE[] = L"Discord Taskbar Typing";

constexpr unsigned short SERVER_PORT = 27345;
constexpr size_t MAX_REQUEST_SIZE = 64 * 1024;

// Un user disparait s'il n'a pas ete "rafraichi" depuis ce delai.
// (Discord renvoie TYPING_START toutes les ~8-10 s tant que la personne ecrit)
constexpr ULONGLONG TYPING_TIMEOUT_MS = 15000;

constexpr UINT WM_UPDATE_TYPING = WM_APP + 1;
constexpr UINT WM_AVATAR_READY = WM_APP + 2;
constexpr UINT_PTR TIMER_ID = 1;
constexpr UINT TIMER_INTERVAL_MS = 40;

// Layout
constexpr int MAX_USERS = 3;
constexpr int AVATAR_SIZE = 56;
constexpr int AVATAR_SPACING = 14;
constexpr int PAD_X = 16;
constexpr int PAD_TOP = 12;
constexpr int DOTS_AREA_HEIGHT = 14;
constexpr int PAD_BOTTOM = 10;
constexpr int TASKBAR_MARGIN = 8;

constexpr double PI = 3.14159265358979323846;

// Avatars
constexpr size_t MAX_AVATAR_BYTES = 2 * 1024 * 1024;
constexpr ULONGLONG AVATAR_RETRY_MS = 60000;

struct TypingUser {
    std::string userId;
    std::string username;
    std::string avatarUrl;
    ULONGLONG lastTyping = 0;
};

std::mutex g_usersMutex;
std::vector<TypingUser> g_users;

std::atomic<SOCKET> g_listenSocket{INVALID_SOCKET};

// Mode --test : pastilles fixes, pas d'expiration.
std::atomic<bool> g_testMode{false};

// ===== JSON =====

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

uint32_t ParseHex4(const std::string& s, size_t pos) {
    return static_cast<uint32_t>(
        std::strtoul(s.substr(pos, 4).c_str(), nullptr, 16)
    );
}

// Cherche "key": "valeur" entre [start, end) et renvoie la valeur decodee.
std::string ExtractString(
    const std::string& json,
    const std::string& key,
    size_t start,
    size_t end
) {
    const std::string needle = "\"" + key + "\"";

    size_t pos = json.find(needle, start);

    if (pos == std::string::npos || pos >= end)
        return {};

    pos += needle.size();

    while (pos < end && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;

    if (pos >= end || json[pos] != ':')
        return {};

    ++pos;

    while (pos < end && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;

    // null, nombre, etc. : on ignore
    if (pos >= end || json[pos] != '"')
        return {};

    ++pos;

    std::string out;

    while (pos < json.size()) {
        const char c = json[pos];

        if (c == '"')
            return out;

        if (c == '\\' && pos + 1 < json.size()) {
            const char n = json[++pos];

            switch (n) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
                if (pos + 4 < json.size()) {
                    uint32_t cp = ParseHex4(json, pos + 1);
                    pos += 4;

                    // paire de surrogates (emojis, etc.)
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        pos + 6 < json.size() &&
                        json[pos + 1] == '\\' &&
                        json[pos + 2] == 'u') {
                        const uint32_t low = ParseHex4(json, pos + 3);

                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            pos += 6;
                        }
                    }

                    AppendUtf8(out, cp);
                }
                break;
            default:
                // \" \\ \/
                out += n;
                break;
            }
        }
        else {
            out += c;
        }

        ++pos;
    }

    return {};
}

std::vector<TypingUser> ParseUsers(const std::string& json) {
    std::vector<TypingUser> users;

    const std::string marker = "\"userId\"";

    std::vector<size_t> starts;

    for (size_t p = json.find(marker);
         p != std::string::npos;
         p = json.find(marker, p + 1)) {
        starts.push_back(p);
    }

    const ULONGLONG now = GetTickCount64();

    for (size_t i = 0; i < starts.size(); ++i) {
        // Les champs de ce user sont cherches jusqu'au userId suivant,
        // pour ne jamais "voler" le pseudo d'un autre.
        const size_t begin = starts[i];
        const size_t end = (i + 1 < starts.size()) ? starts[i + 1] : json.size();

        TypingUser user;

        user.userId = ExtractString(json, "userId", begin, end);
        user.username = ExtractString(json, "username", begin, end);
        user.avatarUrl = ExtractString(json, "avatarUrl", begin, end);
        user.lastTyping = now;

        if (!user.userId.empty())
            users.push_back(std::move(user));
    }

    return users;
}

// ===== Avatars =====

enum class AvatarState {
    Loading,      // telechargement ou decodage en cours
    Downloaded,   // octets recus, en attente de decodage (thread UI)
    Ready,        // bitmap pret a dessiner
    Failed
};

struct AvatarEntry {
    AvatarState state = AvatarState::Loading;
    std::string bytes;
    Gdiplus::Bitmap* bitmap = nullptr; // appartient au thread UI
    ULONGLONG failedAt = 0;
};

// Alloues volontairement sans jamais etre liberes : les threads de
// telechargement detaches ne risquent pas de toucher un objet detruit
// pendant la fermeture du programme.
std::mutex& g_avatarMutex = *new std::mutex();
std::map<std::string, AvatarEntry>& g_avatars = *new std::map<std::string, AvatarEntry>();

struct WinHttpHandle {
    HINTERNET h = nullptr;

    explicit WinHttpHandle(HINTERNET handle = nullptr) : h(handle) {}

    ~WinHttpHandle() {
        if (h)
            WinHttpCloseHandle(h);
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty())
        return {};

    const int len = MultiByteToWideChar(
        CP_UTF8, 0,
        utf8.c_str(), static_cast<int>(utf8.size()),
        nullptr, 0
    );

    if (len <= 0)
        return {};

    std::wstring wide(static_cast<size_t>(len), L'\0');

    MultiByteToWideChar(
        CP_UTF8, 0,
        utf8.c_str(), static_cast<int>(utf8.size()),
        &wide[0], len
    );

    return wide;
}

// Le serveur local accepte des POST de n'importe quel processus (ou
// page web). On ne telecharge donc QUE depuis les CDN de Discord.
bool IsAllowedHost(const std::wstring& host) {
    static const wchar_t* const allowed[] = {
        L"cdn.discordapp.com",
        L"media.discordapp.net",
        L"discord.com"
    };

    for (const wchar_t* name : allowed) {
        if (_wcsicmp(host.c_str(), name) == 0)
            return true;
    }

    return false;
}

bool DownloadUrl(const std::string& url, std::string& out) {
    const std::wstring wurl = Utf8ToWide(url);

    if (wurl.empty())
        return false;

    URL_COMPONENTSW uc{};

    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc))
        return false;

    if (uc.nScheme != INTERNET_SCHEME_HTTPS)
        return false;

    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);

    if (!IsAllowedHost(host))
        return false;

    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);

    if (uc.dwExtraInfoLength > 0)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    WinHttpHandle session(WinHttpOpen(
        L"DiscordTaskbarTyping/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    ));

    if (!session.h)
        return false;

    WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 10000);

    WinHttpHandle connection(
        WinHttpConnect(session.h, host.c_str(), uc.nPort, 0)
    );

    if (!connection.h)
        return false;

    WinHttpHandle request(WinHttpOpenRequest(
        connection.h,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    ));

    if (!request.h)
        return false;

    if (!WinHttpSendRequest(
            request.h,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0,
            0, 0))
        return false;

    if (!WinHttpReceiveResponse(request.h, nullptr))
        return false;

    DWORD status = 0;
    DWORD statusSize = sizeof(status);

    if (!WinHttpQueryHeaders(
            request.h,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        return false;

    out.clear();

    while (true) {
        DWORD available = 0;

        if (!WinHttpQueryDataAvailable(request.h, &available))
            return false;

        if (available == 0)
            break;

        if (out.size() + available > MAX_AVATAR_BYTES)
            return false;

        const size_t offset = out.size();

        out.resize(offset + available);

        DWORD read = 0;

        if (!WinHttpReadData(request.h, &out[offset], available, &read))
            return false;

        out.resize(offset + read);

        if (read == 0)
            break;
    }

    return !out.empty();
}

// Thread de telechargement (un par nouvelle URL).
void DownloadWorker(HWND hwnd, std::string url) {
    std::string bytes;

    const bool ok = DownloadUrl(url, bytes);

    {
        std::lock_guard<std::mutex> lock(g_avatarMutex);

        auto it = g_avatars.find(url);

        if (it != g_avatars.end()) {
            if (ok) {
                it->second.bytes = std::move(bytes);
                it->second.state = AvatarState::Downloaded;
            }
            else {
                it->second.state = AvatarState::Failed;
                it->second.failedAt = GetTickCount64();
            }
        }
    }

    // Reveille le thread UI, qui decode.
    PostMessageW(hwnd, WM_AVATAR_READY, 0, 0);
}

// Lance un telechargement pour chaque pp pas encore en cache.
void RequestAvatars(HWND hwnd, const std::vector<TypingUser>& users) {
    for (const TypingUser& user : users) {
        if (user.avatarUrl.empty())
            continue;

        bool start = false;

        {
            std::lock_guard<std::mutex> lock(g_avatarMutex);

            auto it = g_avatars.find(user.avatarUrl);

            if (it == g_avatars.end()) {
                g_avatars[user.avatarUrl];
                start = true;
            }
            else if (it->second.state == AvatarState::Failed &&
                     GetTickCount64() - it->second.failedAt > AVATAR_RETRY_MS) {
                it->second.state = AvatarState::Loading;
                start = true;
            }
        }

        if (start)
            std::thread(DownloadWorker, hwnd, user.avatarUrl).detach();
    }
}

// Decode un PNG/JPEG/GIF en bitmap AVATAR_SIZE x AVATAR_SIZE (thread UI).
Gdiplus::Bitmap* DecodeAvatar(const std::string& bytes) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());

    if (!memory)
        return nullptr;

    void* ptr = GlobalLock(memory);

    if (!ptr) {
        GlobalFree(memory);
        return nullptr;
    }

    std::memcpy(ptr, bytes.data(), bytes.size());
    GlobalUnlock(memory);

    IStream* stream = nullptr;

    // TRUE : le stream libere la memoire quand on le Release().
    if (CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK) {
        GlobalFree(memory);
        return nullptr;
    }

    Gdiplus::Bitmap* source = Gdiplus::Bitmap::FromStream(stream);
    Gdiplus::Bitmap* result = nullptr;

    if (source &&
        source->GetLastStatus() == Gdiplus::Ok &&
        source->GetWidth() > 0 &&
        source->GetHeight() > 0) {

        // Copie dans un bitmap independant du stream, deja a la bonne taille.
        result = new Gdiplus::Bitmap(
            AVATAR_SIZE,
            AVATAR_SIZE,
            PixelFormat32bppPARGB
        );

        Gdiplus::Graphics graphics(result);

        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        graphics.DrawImage(
            source,
            Gdiplus::Rect(0, 0, AVATAR_SIZE, AVATAR_SIZE),
            0,
            0,
            static_cast<INT>(source->GetWidth()),
            static_cast<INT>(source->GetHeight()),
            Gdiplus::UnitPixel
        );

        if (graphics.GetLastStatus() != Gdiplus::Ok) {
            delete result;
            result = nullptr;
        }
    }

    delete source;
    stream->Release();

    return result;
}

// Decode toutes les pp telechargees (appele depuis le thread UI).
void DecodePendingAvatars() {
    std::vector<std::pair<std::string, std::string>> pending;

    {
        std::lock_guard<std::mutex> lock(g_avatarMutex);

        for (auto it = g_avatars.begin(); it != g_avatars.end(); ++it) {
            if (it->second.state == AvatarState::Downloaded) {
                pending.emplace_back(it->first, std::move(it->second.bytes));

                it->second.bytes.clear();
                it->second.state = AvatarState::Loading;
            }
        }
    }

    for (const auto& item : pending) {
        Gdiplus::Bitmap* bitmap = DecodeAvatar(item.second);

        std::lock_guard<std::mutex> lock(g_avatarMutex);

        auto it = g_avatars.find(item.first);

        if (it == g_avatars.end()) {
            delete bitmap;
            continue;
        }

        if (bitmap) {
            it->second.bitmap = bitmap;
            it->second.state = AvatarState::Ready;
        }
        else {
            it->second.state = AvatarState::Failed;
            it->second.failedAt = GetTickCount64();
        }
    }
}

// Renvoie le bitmap pret pour cette URL, ou nullptr.
Gdiplus::Bitmap* FindAvatar(const std::string& url) {
    if (url.empty())
        return nullptr;

    std::lock_guard<std::mutex> lock(g_avatarMutex);

    auto it = g_avatars.find(url);

    if (it == g_avatars.end() || it->second.state != AvatarState::Ready)
        return nullptr;

    return it->second.bitmap;
}

// Dessine la pp en cercle, bords lisses.
void DrawAvatarImage(HDC hdc, Gdiplus::Bitmap* bitmap, int x, int y) {
    Gdiplus::Graphics graphics(hdc);

    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    Gdiplus::TextureBrush brush(bitmap, Gdiplus::WrapModeClamp);

    brush.TranslateTransform(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y));

    graphics.FillEllipse(&brush, x, y, AVATAR_SIZE, AVATAR_SIZE);
}

// ===== HTTP =====

void SendResponse(
    SOCKET socket,
    const char* status,
    const std::string& body
) {
    const std::string response =
        "HTTP/1.1 " + std::string(status) + "\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
        "Access-Control-Allow-Private-Network: true\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;

    send(
        socket,
        response.c_str(),
        static_cast<int>(response.size()),
        0
    );
}

// Lit une requete complete (headers + body).
bool ReadRequest(
    SOCKET client,
    std::string& method,
    std::string& body
) {
    std::string data;
    char buffer[4096];

    size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos) {
        const int received = recv(client, buffer, sizeof(buffer), 0);

        if (received <= 0)
            return false;

        data.append(buffer, static_cast<size_t>(received));

        if (data.size() > MAX_REQUEST_SIZE)
            return false;

        headerEnd = data.find("\r\n\r\n");
    }

    headerEnd += 4;

    const std::string headers = data.substr(0, headerEnd);

    method = headers.substr(0, headers.find(' '));

    std::string lower = headers;

    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const size_t clPos = lower.find("content-length:");

    if (clPos != std::string::npos) {
        // Cas normal (fetch, curl...) : on lit exactement Content-Length octets.
        const size_t contentLength = static_cast<size_t>(
            std::strtoul(headers.c_str() + clPos + 15, nullptr, 10)
        );

        if (contentLength > MAX_REQUEST_SIZE)
            return false;

        while (data.size() - headerEnd < contentLength) {
            const int received = recv(client, buffer, sizeof(buffer), 0);

            if (received <= 0)
                break;

            data.append(buffer, static_cast<size_t>(received));
        }

        body = data.substr(headerEnd, contentLength);
    }
    else if (lower.find("transfer-encoding: chunked") != std::string::npos) {
        // Cas Node.js (http.request + write) : on attend le chunk final.
        // ParseUsers ignore le "bruit" des tailles de chunks.
        while (data.find("0\r\n\r\n", headerEnd) == std::string::npos) {
            const int received = recv(client, buffer, sizeof(buffer), 0);

            if (received <= 0)
                break;

            data.append(buffer, static_cast<size_t>(received));

            if (data.size() > MAX_REQUEST_SIZE)
                return false;
        }

        body = data.substr(headerEnd);
    }
    else {
        body = data.substr(headerEnd);
    }

    return true;
}

// Cree le socket d'ecoute. Fait dans Create() pour pouvoir
// signaler proprement l'echec (port deja pris).
SOCKET CreateListenSocket() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;

    // Empeche une autre appli de se lier au meme port.
    BOOL exclusive = TRUE;

    setsockopt(
        s,
        SOL_SOCKET,
        SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive),
        sizeof(exclusive)
    );

    sockaddr_in address{};

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(SERVER_PORT);

    if (bind(
            s,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) == SOCKET_ERROR ||
        listen(s, 8) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    return s;
}

void ServerThread(HWND hwnd) {
    while (true) {
        const SOCKET listenSocket = g_listenSocket.load();

        if (listenSocket == INVALID_SOCKET)
            break;

        SOCKET client = accept(listenSocket, nullptr, nullptr);

        // socket ferme par le destructeur / WM_DESTROY
        if (client == INVALID_SOCKET)
            break;

        // Un client qui ne parle pas ne doit pas bloquer le serveur.
        DWORD timeoutMs = 2000;

        setsockopt(
            client,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeoutMs),
            sizeof(timeoutMs)
        );

        std::string method;
        std::string body;

        if (!ReadRequest(client, method, body)) {
            closesocket(client);
            continue;
        }

        // CORS preflight
        if (method == "OPTIONS") {
            SendResponse(client, "204 No Content", "");
            closesocket(client);
            continue;
        }

        auto users = ParseUsers(body);

        if (users.size() > static_cast<size_t>(MAX_USERS))
            users.resize(MAX_USERS);

        RequestAvatars(hwnd, users);

        {
            std::lock_guard<std::mutex> lock(g_usersMutex);
            g_users = std::move(users);
        }

        PostMessageW(hwnd, WM_UPDATE_TYPING, 0, 0);

        SendResponse(client, "200 OK", "{\"ok\":true}");
        closesocket(client);
    }
}

// ===== Dessin =====

// Premiere lettre du pseudo (UTF-8 -> wide), gere accents / kanji / emojis.
std::wstring InitialOf(const std::string& utf8) {
    if (utf8.empty())
        return L"?";

    const int len = MultiByteToWideChar(
        CP_UTF8, 0,
        utf8.c_str(), static_cast<int>(utf8.size()),
        nullptr, 0
    );

    if (len <= 0)
        return L"?";

    std::wstring wide(static_cast<size_t>(len), L'\0');

    MultiByteToWideChar(
        CP_UTF8, 0,
        utf8.c_str(), static_cast<int>(utf8.size()),
        &wide[0], len
    );

    const size_t count =
        (IS_HIGH_SURROGATE(wide[0]) && wide.size() > 1) ? 2 : 1;

    std::wstring result = wide.substr(0, count);

    if (count == 1)
        result[0] = static_cast<wchar_t>(std::towupper(result[0]));

    return result;
}

} // namespace


OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    // Debloque accept() au cas ou on quitte sans passer par WM_DESTROY.
    SOCKET s = g_listenSocket.exchange(INVALID_SOCKET);

    if (s != INVALID_SOCKET)
        closesocket(s);

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    if (m_watchedProcess) {
        CloseHandle(m_watchedProcess);
        m_watchedProcess = nullptr;
    }

    // Les bitmaps GDI+ doivent etre liberes AVANT GdiplusShutdown.
    {
        std::lock_guard<std::mutex> lock(g_avatarMutex);

        for (auto it = g_avatars.begin(); it != g_avatars.end(); ++it) {
            delete it->second.bitmap;
            it->second.bitmap = nullptr;
            it->second.state = AvatarState::Failed;
            it->second.failedAt = GetTickCount64();
        }
    }

    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }

    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
}

bool OverlayWindow::Create(HINSTANCE hInstance) {

    Gdiplus::GdiplusStartupInput gdiplusInput;

    if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        m_gdiplusToken = 0;
        return false;
    }

    WNDCLASSEXW wc{};

    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = OverlayWindow::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc))
        return false;

    // Fenetre creee cachee : elle n'apparait que quand quelqu'un ecrit.
    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        WS_POPUP,
        0,
        0,
        AVATAR_SIZE + 2 * PAD_X,
        PAD_TOP + AVATAR_SIZE + DOTS_AREA_HEIGHT + PAD_BOTTOM,
        nullptr,
        nullptr,
        hInstance,
        this // recupere dans WM_NCCREATE
    );

    if (!m_hwnd)
        return false;

    WSADATA wsaData{};

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;

    m_wsaStarted = true;

    // Fait ici (thread principal) : si le port est deja pris,
    // Create() renvoie false et main.cpp affiche son message d'erreur.
    // Retente pendant ~5 s : si l'ancienne instance vient de se fermer,
    // Windows peut mettre un instant a liberer le port.
    SOCKET listenSocket = INVALID_SOCKET;

    for (int attempt = 0; attempt < 20; ++attempt) {
        listenSocket = CreateListenSocket();

        if (listenSocket != INVALID_SOCKET)
            break;

        Sleep(250);
    }

    if (listenSocket == INVALID_SOCKET)
        return false;

    g_listenSocket.store(listenSocket);

    std::thread(ServerThread, m_hwnd).detach();

    OutputDebugStringW(
        L"[TaskbarTyping] HTTP server ready on 127.0.0.1:27345\n"
    );

    return true;
}

bool OverlayWindow::WatchProcess(DWORD pid) {

    if (m_watchedProcess) {
        CloseHandle(m_watchedProcess);
        m_watchedProcess = nullptr;
    }

    m_watchedProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);

    if (m_watchedProcess)
        return true;

    // ERROR_INVALID_PARAMETER = ce process n'existe plus.
    // Autre erreur : on continue sans surveillance plutot que de partir.
    return GetLastError() != ERROR_INVALID_PARAMETER;
}

const wchar_t* OverlayWindow::WindowClassName() {
    return WINDOW_CLASS_NAME;
}

int OverlayWindow::Run() {
    MSG msg{};

    while (true) {
        const DWORD handleCount = m_watchedProcess ? 1 : 0;

        const DWORD result = MsgWaitForMultipleObjects(
            handleCount,
            handleCount ? &m_watchedProcess : nullptr,
            FALSE,
            INFINITE,
            QS_ALLINPUT
        );

        // Le process surveille (Discord) s'est termine : on ferme.
        if (handleCount && result == WAIT_OBJECT_0) {
            CloseHandle(m_watchedProcess);
            m_watchedProcess = nullptr;

            if (m_hwnd)
                DestroyWindow(m_hwnd);
        }

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return static_cast<int>(msg.wParam);

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void OverlayWindow::Layout(int userCount) {

    if (!m_hwnd || userCount <= 0)
        return;

    const int width =
        userCount * AVATAR_SIZE +
        (userCount - 1) * AVATAR_SPACING +
        2 * PAD_X;

    const int height =
        PAD_TOP + AVATAR_SIZE + DOTS_AREA_HEIGHT + PAD_BOTTOM;

    // Ecran de la barre des taches (et pas forcement l'ecran principal).
    APPBARDATA abd{};

    abd.cbSize = sizeof(abd);

    const bool haveTaskbar =
        SHAppBarMessage(ABM_GETTASKBARPOS, &abd) != 0;

    HMONITOR monitor = haveTaskbar
        ? MonitorFromRect(&abd.rc, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi{};

    mi.cbSize = sizeof(mi);

    GetMonitorInfoW(monitor, &mi);

    const RECT work = mi.rcWork;

    // Par defaut : centre, en bas de la zone de travail.
    int x = work.left + ((work.right - work.left) - width) / 2;
    int y = work.bottom - height - TASKBAR_MARGIN;

    if (haveTaskbar) {
        const RECT& tb = abd.rc;

        switch (abd.uEdge) {
        case ABE_BOTTOM:
            x = tb.left + ((tb.right - tb.left) - width) / 2;
            y = tb.top - height - TASKBAR_MARGIN;
            break;

        case ABE_TOP:
            x = tb.left + ((tb.right - tb.left) - width) / 2;
            y = tb.bottom + TASKBAR_MARGIN;
            break;

        case ABE_LEFT:
            x = tb.right + TASKBAR_MARGIN;
            y = work.bottom - height - TASKBAR_MARGIN;
            break;

        case ABE_RIGHT:
            x = tb.left - width - TASKBAR_MARGIN;
            y = work.bottom - height - TASKBAR_MARGIN;
            break;

        default:
            break;
        }
    }

    // Garde-fou : on ne sort jamais de l'ecran.
    const RECT& screen = mi.rcMonitor;

    x = std::max(static_cast<int>(screen.left),
        std::min(x, static_cast<int>(screen.right) - width));

    y = std::max(static_cast<int>(screen.top),
        std::min(y, static_cast<int>(screen.bottom) - height));

    // Log visible avec DebugView ou l'onglet Sortie de Visual Studio.
    wchar_t log[256];

    wsprintfW(
        log,
        L"[TaskbarTyping] Layout x=%d y=%d w=%d h=%d taskbar=%d edge=%u\n",
        x, y, width, height,
        haveTaskbar ? 1 : 0,
        haveTaskbar ? abd.uEdge : 99u
    );

    OutputDebugStringW(log);

    SetWindowPos(
        m_hwnd,
        HWND_TOPMOST,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE
    );

    // Coins arrondis (le systeme prend possession de la region).
    SetWindowRgn(
        m_hwnd,
        CreateRoundRectRgn(0, 0, width + 1, height + 1, 20, 20),
        TRUE
    );
}

void OverlayWindow::ShowTest() {

    g_testMode.store(true);

    {
        std::lock_guard<std::mutex> lock(g_usersMutex);

        g_users.clear();

        const char* names[] = { "Soleil", "Lucas", "Mathis" };

        for (int i = 0; i < 3; ++i) {
            TypingUser user;

            user.userId = std::to_string(i + 1);
            user.username = names[i];
            user.lastTyping = GetTickCount64();

            g_users.push_back(std::move(user));
        }
    }

    UpdateUsers();
}

void OverlayWindow::UpdateUsers() {

    if (!m_hwnd)
        return;

    int count = 0;

    {
        std::lock_guard<std::mutex> lock(g_usersMutex);
        count = static_cast<int>(g_users.size());
    }

    // Personne n'ecrit : on cache tout.
    if (count == 0) {
        KillTimer(m_hwnd, TIMER_ID);
        ShowWindow(m_hwnd, SW_HIDE);
        return;
    }

    Layout(count);

    if (!IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        SetTimer(m_hwnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
    UpdateWindow(m_hwnd);
}

void OverlayWindow::OnTick() {

    bool changed = false;

    if (!g_testMode.load()) {
        std::lock_guard<std::mutex> lock(g_usersMutex);

        const ULONGLONG now = GetTickCount64();
        const size_t before = g_users.size();

        g_users.erase(
            std::remove_if(
                g_users.begin(),
                g_users.end(),
                [now](const TypingUser& u) {
                    return now - u.lastTyping > TYPING_TIMEOUT_MS;
                }
            ),
            g_users.end()
        );

        changed = g_users.size() != before;
    }

    if (changed) {
        UpdateUsers();
        return;
    }

    // Juste l'animation des points.
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void OverlayWindow::Draw(HDC hdc, int width, int height) {

    // Fond
    RECT rect{ 0, 0, width, height };

    HBRUSH background = CreateSolidBrush(RGB(30, 31, 34));

    FillRect(hdc, &rect, background);

    DeleteObject(background);

    std::vector<TypingUser> users;

    {
        std::lock_guard<std::mutex> lock(g_usersMutex);
        users = g_users;
    }

    if (users.empty())
        return;

    const int count = static_cast<int>(users.size());

    const int totalWidth =
        count * AVATAR_SIZE + (count - 1) * AVATAR_SPACING;

    const int startX = (width - totalWidth) / 2;

    // Plus de contour noir par defaut sur les Ellipse().
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));

    HBRUSH avatarBrush = CreateSolidBrush(RGB(88, 101, 242));

    HFONT font = CreateFontW(
        24, 0, 0, 0,
        FW_BOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY,
        DEFAULT_PITCH,
        L"Segoe UI"
    );

    HGDIOBJ oldFont = SelectObject(hdc, font);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    // Phase d'animation partagee : les 3 points "rebondissent" en vague.
    const double phase =
        static_cast<double>(GetTickCount64() % 1200) / 1200.0 * 2.0 * PI;

    for (int i = 0; i < count; ++i) {

        const int x = startX + i * (AVATAR_SIZE + AVATAR_SPACING);
        const int y = PAD_TOP;

        Gdiplus::Bitmap* avatar = FindAvatar(users[i].avatarUrl);

        if (avatar) {
            // Vraie pp.
            DrawAvatarImage(hdc, avatar, x, y);
        }
        else {
            // Pas encore chargee (ou echec) : rond bleu + initiale.
            HGDIOBJ oldBrush = SelectObject(hdc, avatarBrush);

            Ellipse(hdc, x, y, x + AVATAR_SIZE + 1, y + AVATAR_SIZE + 1);

            SelectObject(hdc, oldBrush);

            const std::wstring initial = InitialOf(users[i].username);

            RECT textRect{ x, y, x + AVATAR_SIZE, y + AVATAR_SIZE };

            DrawTextW(
                hdc,
                initial.c_str(),
                -1,
                &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE
            );
        }

        // 3 petits points propres a chaque avatar.
        const int centerX = x + AVATAR_SIZE / 2;
        const int baseY = y + AVATAR_SIZE + 7;

        for (int dot = 0; dot < 3; ++dot) {

            const double bounce =
                std::max(0.0, std::sin(phase - dot * 0.9));

            const int dotX = centerX + (dot - 1) * 9;
            const int dotY = baseY - static_cast<int>(bounce * 3.0);

            // gris -> blanc selon le rebond
            const int level = 120 + static_cast<int>(bounce * 135.0);

            HBRUSH dotBrush = CreateSolidBrush(RGB(level, level, level));

            HGDIOBJ oldDotBrush = SelectObject(hdc, dotBrush);

            Ellipse(hdc, dotX - 3, dotY - 3, dotX + 3, dotY + 3);

            SelectObject(hdc, oldDotBrush);

            DeleteObject(dotBrush);
        }
    }

    SelectObject(hdc, oldFont);
    SelectObject(hdc, oldPen);

    DeleteObject(font);
    DeleteObject(avatarBrush);
}

LRESULT CALLBACK OverlayWindow::WndProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
) {

    // Le pointeur this est fourni a la creation.
    if (message == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);

        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(cs->lpCreateParams)
        );
    }

    OverlayWindow* window =
        reinterpret_cast<OverlayWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA)
        );

    switch (message) {

    case WM_NCHITTEST:
        // Click-through.
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_UPDATE_TYPING:

        if (window)
            window->UpdateUsers();

        return 0;

    case WM_AVATAR_READY:

        DecodePendingAvatars();

        if (IsWindowVisible(hwnd))
            InvalidateRect(hwnd, nullptr, FALSE);

        return 0;

    case WM_TIMER:

        if (wParam == TIMER_ID && window)
            window->OnTick();

        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:

        // Resolution / barre des taches changee : on replace.
        if (window && IsWindowVisible(hwnd))
            window->UpdateUsers();

        return 0;

    case WM_PAINT: {

        PAINTSTRUCT ps{};

        HDC hdc = BeginPaint(hwnd, &ps);

        if (window) {
            RECT rc{};

            GetClientRect(hwnd, &rc);

            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;

            // Double buffering : plus de scintillement.
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP bitmap = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ oldBitmap = SelectObject(memDC, bitmap);

            window->Draw(memDC, w, h);

            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(memDC);
        }

        EndPaint(hwnd, &ps);

        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY: {

        // Debloque accept() dans le thread serveur.
        SOCKET s = g_listenSocket.exchange(INVALID_SOCKET);

        if (s != INVALID_SOCKET)
            closesocket(s);

        KillTimer(hwnd, TIMER_ID);

        if (window)
            window->m_hwnd = nullptr;

        PostQuitMessage(0);

        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
