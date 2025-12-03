/**
 * PuffyEngine (Puffy) - A Steam HTMLSurface Web Game Engine
 *
 * This application creates a native Windows window that hosts a Chromium-based
 * browser via Steam's HTMLSurface API. A local HTTP server serves game content,
 * allowing web-based games to access Steam features through a JavaScript
 * bridge.
 *
 * Features:
 *   - Built-in HTTP server for serving game content
 *   - Full Steam API integration (achievements, stats, cloud saves)
 *   - Keyboard, mouse, and gamepad input forwarding
 *   - High-DPI display support
 *   - JavaScript-to-native communication via JSAlert bridge
 *
 * Command Line Options:
 *   --puffyport <port>      HTTP server port (default: 19996)
 *   --puffydir <path>       Game content directory (default: ./game/)
 *   --puffywidth <pixels>   Window width (default: 80% of screen)
 *   --puffyheight <pixels>  Window height (default: 80% of screen)
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <shellscalingapi.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <xinput.h>

#include <steam/isteamhtmlsurface.h>
#include <steam/steam_api.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "steam_api64.lib")

// ============================================================================
// Configuration Constants
// ============================================================================

static constexpr int DEFAULT_PORT = 19996;
static constexpr int HTTP_THREAD_POOL_SIZE = 4;
static constexpr int HTTP_BUFFER_SIZE = 8192;
static constexpr int HTTP_MAX_PENDING = 16;
static constexpr float DEFAULT_WINDOW_SCALE = 0.8f;
static constexpr UINT GAMEPAD_POLL_INTERVAL = 16; // ~60Hz polling
static constexpr float GAMEPAD_DEADZONE = 0.2f;
static constexpr float GAMEPAD_CURSOR_SPEED = 15.0f;

// ============================================================================
// Forward Declarations
// ============================================================================

class HttpServer;
class SteamBrowserCallbacks;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// Global State
// ============================================================================

struct PuffyConfig {
  int port = DEFAULT_PORT;
  std::string gameDir = "game";
  int width = 0;
  int height = 0;
  bool customSize = false;
};

struct PuffyState {
  HWND hwnd = nullptr;
  HDC hdcMem = nullptr;
  HBITMAP hBitmap = nullptr;
  void *pixelBuffer = nullptr;
  int bufferWidth = 0;
  int bufferHeight = 0;

  HHTMLBrowser browser = INVALID_HTMLBROWSER;
  bool browserReady = false;
  bool needsResize = false;

  std::atomic<bool> running = true;
  bool cursorVisible = true;
  bool isFullscreen = false;
  WINDOWPLACEMENT windowPlacement = {};

  float gamepadCursorX = 0.0f;
  float gamepadCursorY = 0.0f;
  bool gamepadAPressed = false;
  bool gamepadBPressed = false;

  std::string appDir;
};

static PuffyConfig g_config;
static PuffyState g_state;
static HttpServer *g_httpServer = nullptr;

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Retrieves the directory containing the executable.
 */
std::string GetApplicationDirectory() {
  char buffer[MAX_PATH];
  GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  std::string path(buffer);
  size_t pos = path.find_last_of("\\/");
  return (pos != std::string::npos) ? path.substr(0, pos + 1) : "";
}

/**
 * Normalizes a file path, converting forward slashes and removing ".."
 * sequences. Returns empty string if the path attempts to escape the root.
 */
std::string NormalizePath(const std::string &path) {
  std::string result;
  std::vector<std::string> parts;
  std::string current;

  for (char c : path) {
    if (c == '/' || c == '\\') {
      if (!current.empty()) {
        if (current == "..") {
          if (parts.empty())
            return "";
          parts.pop_back();
        } else if (current != ".") {
          parts.push_back(current);
        }
        current.clear();
      }
    } else {
      current += c;
    }
  }

  if (!current.empty() && current != "." && current != "..") {
    parts.push_back(current);
  } else if (current == "..") {
    if (parts.empty())
      return "";
    parts.pop_back();
  }

  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0)
      result += "\\";
    result += parts[i];
  }

  return result;
}

/**
 * Returns the MIME type for a file based on its extension.
 */
const char *GetMimeType(const std::string &path) {
  static const std::unordered_map<std::string, const char *> mimeTypes = {
      {".html", "text/html"},        {".htm", "text/html"},
      {".css", "text/css"},          {".js", "application/javascript"},
      {".json", "application/json"}, {".png", "image/png"},
      {".jpg", "image/jpeg"},        {".jpeg", "image/jpeg"},
      {".gif", "image/gif"},         {".svg", "image/svg+xml"},
      {".ico", "image/x-icon"},      {".woff", "font/woff"},
      {".woff2", "font/woff2"},      {".ttf", "font/ttf"},
      {".otf", "font/otf"},          {".mp3", "audio/mpeg"},
      {".ogg", "audio/ogg"},         {".wav", "audio/wav"},
      {".mp4", "video/mp4"},         {".webm", "video/webm"},
      {".webp", "image/webp"},       {".txt", "text/plain"},
      {".xml", "application/xml"},   {".wasm", "application/wasm"},
  };

  size_t dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](int c) { return static_cast<char>(::tolower(c)); });
    auto it = mimeTypes.find(ext);
    if (it != mimeTypes.end())
      return it->second;
  }
  return "application/octet-stream";
}

/**
 * Reads an entire file into a vector of bytes.
 */
bool ReadFileToVector(const std::string &path, std::vector<char> &out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return false;

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  out.resize(static_cast<size_t>(size));
  return file.read(out.data(), size).good();
}

/**
 * URL-decodes a string (converts %XX sequences to characters).
 */
std::string UrlDecode(const std::string &str) {
  std::string result;
  result.reserve(str.size());

  for (size_t i = 0; i < str.size(); ++i) {
    if (str[i] == '%' && i + 2 < str.size()) {
      int value;
      if (sscanf(str.c_str() + i + 1, "%2x", &value) == 1) {
        result += static_cast<char>(value);
        i += 2;
        continue;
      }
    } else if (str[i] == '+') {
      result += ' ';
      continue;
    }
    result += str[i];
  }
  return result;
}

// ============================================================================
// HTTP Server Implementation
// ============================================================================

/**
 * A simple multi-threaded HTTP server using Winsock.
 * Serves static files from a configured directory to localhost clients only.
 */
class HttpServer {
public:
  HttpServer(int port, const std::string &rootDir)
      : m_port(port), m_rootDir(rootDir), m_running(false),
        m_socket(INVALID_SOCKET) {}

  ~HttpServer() { Stop(); }

  /**
   * Starts the HTTP server on a background thread.
   * Creates a thread pool for handling incoming connections.
   */
  bool Start() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
      return false;
    }

    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET) {
      WSACleanup();
      return false;
    }

    int opt = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt,
               sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
    addr.sin_port = htons(static_cast<u_short>(m_port));

    if (bind(m_socket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
      closesocket(m_socket);
      WSACleanup();
      return false;
    }

    if (listen(m_socket, HTTP_MAX_PENDING) == SOCKET_ERROR) {
      closesocket(m_socket);
      WSACleanup();
      return false;
    }

    m_running = true;

    for (int i = 0; i < HTTP_THREAD_POOL_SIZE; ++i) {
      m_workers.emplace_back(&HttpServer::WorkerThread, this);
    }

    m_acceptThread = std::thread(&HttpServer::AcceptThread, this);

    return true;
  }

  /**
   * Stops the HTTP server and cleans up all resources.
   */
  void Stop() {
    if (!m_running)
      return;

    m_running = false;

    if (m_socket != INVALID_SOCKET) {
      closesocket(m_socket);
      m_socket = INVALID_SOCKET;
    }

    m_condition.notify_all();

    if (m_acceptThread.joinable()) {
      m_acceptThread.join();
    }

    for (auto &worker : m_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    m_workers.clear();

    WSACleanup();
  }

private:
  /**
   * Thread that accepts incoming connections and queues them for workers.
   */
  void AcceptThread() {
    while (m_running) {
      sockaddr_in clientAddr = {};
      int clientLen = sizeof(clientAddr);

      SOCKET client = accept(m_socket, (sockaddr *)&clientAddr, &clientLen);
      if (client == INVALID_SOCKET) {
        continue;
      }

      if (clientAddr.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        closesocket(client);
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_clientQueue.push(client);
      }
      m_condition.notify_one();
    }
  }

  /**
   * Worker thread that processes HTTP requests from the queue.
   */
  void WorkerThread() {
    while (m_running) {
      SOCKET client;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(
            lock, [this] { return !m_clientQueue.empty() || !m_running; });

        if (!m_running && m_clientQueue.empty())
          return;
        if (m_clientQueue.empty())
          continue;

        client = m_clientQueue.front();
        m_clientQueue.pop();
      }

      HandleClient(client);
      closesocket(client);
    }
  }

  /**
   * Handles a single HTTP request/response cycle.
   */
  void HandleClient(SOCKET client) {
    char buffer[HTTP_BUFFER_SIZE];
    int received = recv(client, buffer, HTTP_BUFFER_SIZE - 1, 0);
    if (received <= 0)
      return;

    buffer[received] = '\0';

    char methodBuf[16], pathBuf[1024];
    if (sscanf(buffer, "%15s %1023s", methodBuf, pathBuf) < 2) {
      SendError(client, 400, "Bad Request");
      return;
    }

    std::string method = methodBuf;
    std::string path = UrlDecode(pathBuf);

    if (method != "GET" && method != "HEAD") {
      SendError(client, 405, "Method Not Allowed");
      return;
    }

    size_t queryPos = path.find('?');
    if (queryPos != std::string::npos) {
      path = path.substr(0, queryPos);
    }

    if (path == "/")
      path = "/index.html";

    std::string relativePath = NormalizePath(path.substr(1));
    if (relativePath.empty() && path != "/index.html") {
      SendError(client, 403, "Forbidden");
      return;
    }

    std::string fullPath =
        m_rootDir + "\\" + (relativePath.empty() ? "index.html" : relativePath);

    std::vector<char> fileData;
    if (!ReadFileToVector(fullPath, fileData)) {
      SendError(client, 404, "Not Found");
      return;
    }

    const char *mime = GetMimeType(fullPath);

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << mime << "\r\n";
    response << "Content-Length: " << fileData.size() << "\r\n";
    response << "Connection: close\r\n";
    response << "Cache-Control: no-cache\r\n";
    response << "\r\n";

    std::string header = response.str();
    send(client, header.c_str(), static_cast<int>(header.size()), 0);

    if (method == "GET" && !fileData.empty()) {
      send(client, fileData.data(), static_cast<int>(fileData.size()), 0);
    }
  }

  /**
   * Sends an HTTP error response.
   */
  void SendError(SOCKET client, int code, const char *message) {
    std::ostringstream response;
    response << "HTTP/1.1 " << code << " " << message << "\r\n";
    response << "Content-Type: text/plain\r\n";
    response << "Content-Length: " << strlen(message) << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << message;

    std::string str = response.str();
    send(client, str.c_str(), static_cast<int>(str.size()), 0);
  }

  int m_port;
  std::string m_rootDir;
  std::atomic<bool> m_running;
  SOCKET m_socket;

  std::thread m_acceptThread;
  std::vector<std::thread> m_workers;
  std::queue<SOCKET> m_clientQueue;
  std::mutex m_mutex;
  std::condition_variable m_condition;
};

// ============================================================================
// Steam HTML Surface Callbacks
// ============================================================================

/**
 * Handles callbacks from the Steam HTMLSurface.
 * Manages browser lifecycle, rendering, and JavaScript bridge communication.
 */
class SteamBrowserCallbacks {
public:
  CCallResult<SteamBrowserCallbacks, HTML_BrowserReady_t> m_BrowserReady;

  STEAM_CALLBACK(SteamBrowserCallbacks, OnNeedsPaint, HTML_NeedsPaint_t);
  STEAM_CALLBACK(SteamBrowserCallbacks, OnStartRequest, HTML_StartRequest_t);
  STEAM_CALLBACK(SteamBrowserCallbacks, OnFinishedRequest,
                 HTML_FinishedRequest_t);
  STEAM_CALLBACK(SteamBrowserCallbacks, OnJSAlert, HTML_JSAlert_t);
  STEAM_CALLBACK(SteamBrowserCallbacks, OnJSConfirm, HTML_JSConfirm_t);
  STEAM_CALLBACK(SteamBrowserCallbacks, OnURLChanged, HTML_URLChanged_t);
  STEAM_CALLBACK(SteamBrowserCallbacks, OnSetCursor, HTML_SetCursor_t);

  void OnBrowserReady(HTML_BrowserReady_t *pCallback, bool bIOFailure);
};

/**
 * Called when the browser instance is ready for use.
 */
void SteamBrowserCallbacks::OnBrowserReady(HTML_BrowserReady_t *pCallback,
                                           bool bIOFailure) {
  if (bIOFailure || !pCallback) {
    printf("ERROR: Browser ready callback failed (IO failure)\n");
    return;
  }

  g_state.browser = pCallback->unBrowserHandle;
  g_state.browserReady = true;

  // Set browser size to window size
  RECT rc;
  GetClientRect(g_state.hwnd, &rc);
  int width = rc.right - rc.left;
  int height = rc.bottom - rc.top;
  SteamHTMLSurface()->SetSize(g_state.browser, width, height);
  SteamHTMLSurface()->SetKeyFocus(g_state.browser, true);

  std::string url = "http://127.0.0.1:" + std::to_string(g_config.port) + "/";
  SteamHTMLSurface()->LoadURL(g_state.browser, url.c_str(), nullptr);
}

/**
 * Called when the browser has new pixels to paint.
 * Copies the browser's BGRA pixel data to our backing buffer.
 */
void SteamBrowserCallbacks::OnNeedsPaint(HTML_NeedsPaint_t *pCallback) {
  if (pCallback->unBrowserHandle != g_state.browser)
    return;
  if (!g_state.pixelBuffer) {
    printf("WARNING: OnNeedsPaint called but pixelBuffer is null\n");
    return;
  }

  int srcWidth = static_cast<int>(pCallback->unWide);
  int srcHeight = static_cast<int>(pCallback->unTall);

  int copyWidth = (std::min)(srcWidth, g_state.bufferWidth);
  int copyHeight = (std::min)(srcHeight, g_state.bufferHeight);

  const char *src = pCallback->pBGRA;
  char *dst = static_cast<char *>(g_state.pixelBuffer);

  int srcPitch = srcWidth * 4;
  int dstPitch = g_state.bufferWidth * 4;

  for (int y = 0; y < copyHeight; ++y) {
    int srcY = (srcHeight - 1) - y; // Flip vertically for Windows DIB
    memcpy(dst + y * dstPitch, src + srcY * srcPitch, copyWidth * 4);
  }

  InvalidateRect(g_state.hwnd, nullptr, FALSE);
}

/**
 * Called when the browser starts loading a resource.
 */
void SteamBrowserCallbacks::OnStartRequest(HTML_StartRequest_t *pCallback) {
  if (pCallback->unBrowserHandle != g_state.browser)
    return;
  SteamHTMLSurface()->AllowStartRequest(g_state.browser, true);
}

/**
 * Called when the browser finishes loading a resource.
 */
void SteamBrowserCallbacks::OnFinishedRequest(
    HTML_FinishedRequest_t *pCallback) {
  (void)pCallback;
}

/**
 * Called when the browser's URL changes.
 */
void SteamBrowserCallbacks::OnURLChanged(HTML_URLChanged_t *pCallback) {
  (void)pCallback;
}

/**
 * Called when the browser wants to change the cursor.
 */
void SteamBrowserCallbacks::OnSetCursor(HTML_SetCursor_t *pCallback) {
  if (pCallback->unBrowserHandle != g_state.browser)
    return;

  HCURSOR cursor = LoadCursor(nullptr, IDC_ARROW);
  switch (pCallback->eMouseCursor) {
  case ISteamHTMLSurface::k_EHTMLMouseCursor_Hand:
    cursor = LoadCursor(nullptr, IDC_HAND);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_IBeam:
    cursor = LoadCursor(nullptr, IDC_IBEAM);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_Crosshair:
    cursor = LoadCursor(nullptr, IDC_CROSS);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_WaitArrow:
  case ISteamHTMLSurface::k_EHTMLMouseCursor_Hourglass:
    cursor = LoadCursor(nullptr, IDC_WAIT);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_SizeNW:
  case ISteamHTMLSurface::k_EHTMLMouseCursor_SizeSE:
    cursor = LoadCursor(nullptr, IDC_SIZENWSE);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_SizeNE:
  case ISteamHTMLSurface::k_EHTMLMouseCursor_SizeSW:
    cursor = LoadCursor(nullptr, IDC_SIZENESW);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_SizeWE:
    cursor = LoadCursor(nullptr, IDC_SIZEWE);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_SizeNS:
    cursor = LoadCursor(nullptr, IDC_SIZENS);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_SizeAll:
    cursor = LoadCursor(nullptr, IDC_SIZEALL);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_No:
    cursor = LoadCursor(nullptr, IDC_NO);
    break;
  case ISteamHTMLSurface::k_EHTMLMouseCursor_None:
    cursor = nullptr;
    break;
  default:
    break;
  }

  if (g_state.cursorVisible && cursor) {
    SetCursor(cursor);
  }
}

/**
 * Called when JavaScript triggers an alert().
 * This is the main communication channel from the game to the engine.
 * Messages prefixed with "puffy:" are engine commands.
 */
void SteamBrowserCallbacks::OnJSAlert(HTML_JSAlert_t *pCallback) {
  if (pCallback->unBrowserHandle != g_state.browser)
    return;

  std::string message = pCallback->pchMessage;

  SteamHTMLSurface()->JSDialogResponse(g_state.browser, true);

  if (message.rfind("puffy:", 0) != 0)
    return;

  std::string command = message.substr(6);

  if (command == "exit") {
    PostMessage(g_state.hwnd, WM_CLOSE, 0, 0);
    return;
  }

  if (command == "setFullscreen(true)") {
    if (!g_state.isFullscreen) {
      g_state.windowPlacement.length = sizeof(WINDOWPLACEMENT);
      GetWindowPlacement(g_state.hwnd, &g_state.windowPlacement);

      MONITORINFO mi = {sizeof(mi)};
      GetMonitorInfo(MonitorFromWindow(g_state.hwnd, MONITOR_DEFAULTTOPRIMARY),
                     &mi);

      SetWindowLongPtr(g_state.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
      SetWindowPos(g_state.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);

      g_state.isFullscreen = true;
    }
    return;
  }

  if (command == "setFullscreen(false)") {
    if (g_state.isFullscreen) {
      SetWindowLongPtr(g_state.hwnd, GWL_STYLE,
                       WS_OVERLAPPEDWINDOW | WS_VISIBLE);
      SetWindowPlacement(g_state.hwnd, &g_state.windowPlacement);
      SetWindowPos(g_state.hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

      g_state.isFullscreen = false;
    }
    return;
  }

  if (command == "setCursorVisible(true)") {
    g_state.cursorVisible = true;
    ShowCursor(TRUE);
    return;
  }

  if (command == "setCursorVisible(false)") {
    g_state.cursorVisible = false;
    ShowCursor(FALSE);
    return;
  }

  if (command.rfind("grantSteamAchievement(", 0) == 0) {
    size_t start = command.find('"');
    size_t end = command.rfind('"');
    if (start != std::string::npos && end != std::string::npos && end > start) {
      std::string achievement = command.substr(start + 1, end - start - 1);
      if (SteamUserStats()) {
        SteamUserStats()->SetAchievement(achievement.c_str());
        SteamUserStats()->StoreStats();
      }
    }
    return;
  }

  if (command.rfind("grantSteamStat(", 0) == 0) {
    size_t start = command.find('"');
    size_t end = command.find('"', start + 1);
    size_t comma = command.find(',', end);
    size_t paren = command.rfind(')');

    if (start != std::string::npos && end != std::string::npos &&
        comma != std::string::npos && paren != std::string::npos) {
      std::string statName = command.substr(start + 1, end - start - 1);
      std::string valueStr = command.substr(comma + 1, paren - comma - 1);

      while (!valueStr.empty() && valueStr[0] == ' ')
        valueStr.erase(0, 1);

      if (SteamUserStats()) {
        if (valueStr.find('.') != std::string::npos) {
          float value = std::stof(valueStr);
          SteamUserStats()->SetStat(statName.c_str(), value);
        } else {
          int value = std::stoi(valueStr);
          SteamUserStats()->SetStat(statName.c_str(), value);
        }
        SteamUserStats()->StoreStats();
      }
    }
    return;
  }

  if (command.rfind("requestSteamCloudSave(", 0) == 0) {
    size_t start1 = command.find('"');
    size_t end1 = command.find('"', start1 + 1);
    size_t start2 = command.find('"', end1 + 1);
    size_t end2 = command.find('"', start2 + 1);

    if (start1 != std::string::npos && end1 != std::string::npos &&
        start2 != std::string::npos && end2 != std::string::npos) {
      std::string filename = command.substr(start1 + 1, end1 - start1 - 1);
      std::string callback = command.substr(start2 + 1, end2 - start2 - 1);

      std::string jsCall;

      if (SteamRemoteStorage() &&
          SteamRemoteStorage()->FileExists(filename.c_str())) {
        int32 fileSize = SteamRemoteStorage()->GetFileSize(filename.c_str());
        if (fileSize > 0) {
          std::vector<char> buffer(fileSize);
          int32 bytesRead = SteamRemoteStorage()->FileRead(
              filename.c_str(), buffer.data(), fileSize);

          if (bytesRead == fileSize) {
            std::string data(buffer.begin(), buffer.end());
            std::string escaped;
            for (char c : data) {
              if (c == '\\')
                escaped += "\\\\";
              else if (c == '"')
                escaped += "\\\"";
              else if (c == '\n')
                escaped += "\\n";
              else if (c == '\r')
                escaped += "\\r";
              else if (c == '\t')
                escaped += "\\t";
              else
                escaped += c;
            }
            jsCall = callback + "(\"" + escaped + "\");";
          } else {
            jsCall = callback + "(null);";
          }
        } else {
          jsCall = callback + "(null);";
        }
      } else {
        jsCall = callback + "(null);";
      }

      SteamHTMLSurface()->ExecuteJavascript(g_state.browser, jsCall.c_str());
    }
    return;
  }

  if (command.rfind("saveToSteamCloudSave(", 0) == 0) {
    size_t start1 = command.find('"');
    size_t end1 = command.find('"', start1 + 1);
    size_t start2 = command.find('"', end1 + 1);
    size_t end2 = command.rfind('"');

    if (start1 != std::string::npos && end1 != std::string::npos &&
        start2 != std::string::npos && end2 != std::string::npos &&
        end2 > start2) {
      std::string filename = command.substr(start1 + 1, end1 - start1 - 1);
      std::string data = command.substr(start2 + 1, end2 - start2 - 1);

      if (SteamRemoteStorage()) {
        SteamRemoteStorage()->FileWrite(filename.c_str(), data.c_str(),
                                        static_cast<int32>(data.size()));
      }
    }
    return;
  }

  if (command.rfind("removeSteamCloudSave(", 0) == 0) {
    size_t start = command.find('"');
    size_t end = command.rfind('"');
    if (start != std::string::npos && end != std::string::npos && end > start) {
      std::string filename = command.substr(start + 1, end - start - 1);
      if (SteamRemoteStorage()) {
        SteamRemoteStorage()->FileDelete(filename.c_str());
      }
    }
    return;
  }
}

/**
 * Called when JavaScript triggers a confirm() dialog.
 * Simply accepts all confirmations.
 */
void SteamBrowserCallbacks::OnJSConfirm(HTML_JSConfirm_t *pCallback) {
  if (pCallback->unBrowserHandle != g_state.browser)
    return;
  SteamHTMLSurface()->JSDialogResponse(g_state.browser, true);
}

static SteamBrowserCallbacks *g_steamCallbacks = nullptr;

// ============================================================================
// Window Management
// ============================================================================

/**
 * Creates or recreates the backing pixel buffer for the browser.
 * Called on window resize.
 */
void RecreatePixelBuffer(int width, int height) {
  if (g_state.hdcMem) {
    DeleteDC(g_state.hdcMem);
    g_state.hdcMem = nullptr;
  }
  if (g_state.hBitmap) {
    DeleteObject(g_state.hBitmap);
    g_state.hBitmap = nullptr;
  }

  if (width <= 0 || height <= 0)
    return;

  HDC hdc = GetDC(g_state.hwnd);
  g_state.hdcMem = CreateCompatibleDC(hdc);

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = height; // Positive = bottom-up DIB
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  g_state.hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS,
                                     &g_state.pixelBuffer, nullptr, 0);

  SelectObject(g_state.hdcMem, g_state.hBitmap);
  ReleaseDC(g_state.hwnd, hdc);

  g_state.bufferWidth = width;
  g_state.bufferHeight = height;

  if (g_state.pixelBuffer) {
    memset(g_state.pixelBuffer, 0, width * height * 4);
  }
}

/**
 * Handles window resize by updating the browser and pixel buffer.
 */
void HandleResize() {
  RECT rc;
  GetClientRect(g_state.hwnd, &rc);
  int width = rc.right - rc.left;
  int height = rc.bottom - rc.top;

  if (width <= 0 || height <= 0)
    return;
  if (width == g_state.bufferWidth && height == g_state.bufferHeight)
    return;

  RecreatePixelBuffer(width, height);

  if (g_state.browserReady && g_state.browser != INVALID_HTMLBROWSER) {
    SteamHTMLSurface()->SetSize(g_state.browser, width, height);
  }

  g_state.gamepadCursorX = static_cast<float>(width / 2);
  g_state.gamepadCursorY = static_cast<float>(height / 2);
}

/**
 * Maps Windows virtual key codes to Steam HTMLSurface key codes.
 */
ISteamHTMLSurface::EHTMLKeyModifiers GetKeyModifiers() {
  int modifiers = 0;
  if (GetKeyState(VK_MENU) & 0x8000)
    modifiers |= ISteamHTMLSurface::k_eHTMLKeyModifier_AltDown;
  if (GetKeyState(VK_CONTROL) & 0x8000)
    modifiers |= ISteamHTMLSurface::k_eHTMLKeyModifier_CtrlDown;
  if (GetKeyState(VK_SHIFT) & 0x8000)
    modifiers |= ISteamHTMLSurface::k_eHTMLKeyModifier_ShiftDown;
  return static_cast<ISteamHTMLSurface::EHTMLKeyModifiers>(modifiers);
}

/**
 * Maps Windows virtual key codes to their corresponding HTML scan codes.
 * Based on the DOM KeyboardEvent code values.
 */
uint32 VirtualKeyToNativeKeyCode(WPARAM vk) {
  return static_cast<uint32>(
      MapVirtualKey(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
}

/**
 * Window procedure for the main Puffy window.
 */
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  // Handle window lifecycle messages regardless of browser state
  switch (msg) {
  case WM_CLOSE:
    g_state.running = false;
    return 0;

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;

  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    if (g_state.hdcMem && g_state.pixelBuffer) {
      BitBlt(hdc, 0, 0, g_state.bufferWidth, g_state.bufferHeight,
             g_state.hdcMem, 0, 0, SRCCOPY);
    }

    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_SIZE:
    g_state.needsResize = true;
    return 0;
  }

  // For browser-related messages, check if browser is ready
  if (!g_state.browserReady || g_state.browser == INVALID_HTMLBROWSER) {
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  auto modifiers = GetKeyModifiers();

  switch (msg) {

  case WM_MOUSEMOVE: {
    int x = LOWORD(lParam);
    int y = HIWORD(lParam);
    SteamHTMLSurface()->MouseMove(g_state.browser, x, y);
    return 0;
  }

  case WM_LBUTTONDOWN:
    SteamHTMLSurface()->MouseDown(g_state.browser,
                                  ISteamHTMLSurface::eHTMLMouseButton_Left);
    SetCapture(hwnd);
    return 0;

  case WM_LBUTTONUP:
    SteamHTMLSurface()->MouseUp(g_state.browser,
                                ISteamHTMLSurface::eHTMLMouseButton_Left);
    ReleaseCapture();
    return 0;

  case WM_RBUTTONDOWN:
    SteamHTMLSurface()->MouseDown(g_state.browser,
                                  ISteamHTMLSurface::eHTMLMouseButton_Right);
    return 0;

  case WM_RBUTTONUP:
    SteamHTMLSurface()->MouseUp(g_state.browser,
                                ISteamHTMLSurface::eHTMLMouseButton_Right);
    return 0;

  case WM_MBUTTONDOWN:
    SteamHTMLSurface()->MouseDown(g_state.browser,
                                  ISteamHTMLSurface::eHTMLMouseButton_Middle);
    return 0;

  case WM_MBUTTONUP:
    SteamHTMLSurface()->MouseUp(g_state.browser,
                                ISteamHTMLSurface::eHTMLMouseButton_Middle);
    return 0;

  case WM_MOUSEWHEEL: {
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    SteamHTMLSurface()->MouseWheel(g_state.browser, delta);
    return 0;
  }

  case WM_KEYDOWN:
  case WM_SYSKEYDOWN: {
    uint32 nativeKeyCode = VirtualKeyToNativeKeyCode(wParam);
    SteamHTMLSurface()->KeyDown(g_state.browser, nativeKeyCode, modifiers,
                                false);
    return 0;
  }

  case WM_KEYUP:
  case WM_SYSKEYUP: {
    uint32 nativeKeyCode = VirtualKeyToNativeKeyCode(wParam);
    SteamHTMLSurface()->KeyUp(g_state.browser, nativeKeyCode, modifiers);
    return 0;
  }

  case WM_CHAR: {
    SteamHTMLSurface()->KeyChar(g_state.browser, static_cast<uint32>(wParam),
                                modifiers);
    return 0;
  }

  case WM_SETFOCUS:
    SteamHTMLSurface()->SetKeyFocus(g_state.browser, true);
    return 0;

  case WM_KILLFOCUS:
    SteamHTMLSurface()->SetKeyFocus(g_state.browser, false);
    return 0;
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**
 * Creates the main application window with DPI awareness.
 */
bool CreatePuffyWindow(HINSTANCE hInstance) {
  SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = L"PuffyEngineClass";
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

  if (!RegisterClassExW(&wc)) {
    return false;
  }

  HMONITOR hMonitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfo(hMonitor, &mi);

  int screenWidth = mi.rcWork.right - mi.rcWork.left;
  int screenHeight = mi.rcWork.bottom - mi.rcWork.top;

  int width, height;
  if (g_config.customSize) {
    width = g_config.width;
    height = g_config.height;
  } else {
    width = static_cast<int>(screenWidth * DEFAULT_WINDOW_SCALE);
    height = static_cast<int>(screenHeight * DEFAULT_WINDOW_SCALE);
  }

  int x = (screenWidth - width) / 2 + mi.rcWork.left;
  int y = (screenHeight - height) / 2 + mi.rcWork.top;

  RECT rc = {0, 0, width, height};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

  g_state.hwnd =
      CreateWindowExW(0, L"PuffyEngineClass", L"PuffyEngine",
                      WS_OVERLAPPEDWINDOW, x, y, rc.right - rc.left,
                      rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

  if (!g_state.hwnd) {
    return false;
  }

  ShowWindow(g_state.hwnd, SW_SHOW);
  UpdateWindow(g_state.hwnd);

  return true;
}

// ============================================================================
// Gamepad Input Handling
// ============================================================================

/**
 * Applies deadzone to analog stick input and normalizes to -1.0 to 1.0 range.
 */
float ApplyDeadzone(SHORT value) {
  float normalized = value / 32767.0f;
  if (normalized < -1.0f)
    normalized = -1.0f;
  if (normalized > 1.0f)
    normalized = 1.0f;

  if (fabs(normalized) < GAMEPAD_DEADZONE) {
    return 0.0f;
  }

  float sign = (normalized > 0) ? 1.0f : -1.0f;
  return sign * (fabs(normalized) - GAMEPAD_DEADZONE) /
         (1.0f - GAMEPAD_DEADZONE);
}

/**
 * Polls gamepad state and translates to mouse movement and clicks.
 * Uses left stick for cursor movement, A button for left click, B for right
 * click.
 */
void UpdateGamepad() {
  if (!g_state.browserReady || g_state.browser == INVALID_HTMLBROWSER)
    return;

  XINPUT_STATE state;
  ZeroMemory(&state, sizeof(XINPUT_STATE));

  if (XInputGetState(0, &state) != ERROR_SUCCESS) {
    return;
  }

  float stickX = ApplyDeadzone(state.Gamepad.sThumbLX);
  float stickY = ApplyDeadzone(state.Gamepad.sThumbLY);

  if (stickX != 0.0f || stickY != 0.0f) {
    g_state.gamepadCursorX += stickX * GAMEPAD_CURSOR_SPEED;
    g_state.gamepadCursorY -= stickY * GAMEPAD_CURSOR_SPEED; // Y is inverted

    g_state.gamepadCursorX =
        (std::max)(0.0f,
                   (std::min)(g_state.gamepadCursorX,
                              static_cast<float>(g_state.bufferWidth - 1)));
    g_state.gamepadCursorY =
        (std::max)(0.0f,
                   (std::min)(g_state.gamepadCursorY,
                              static_cast<float>(g_state.bufferHeight - 1)));

    SteamHTMLSurface()->MouseMove(g_state.browser,
                                  static_cast<int>(g_state.gamepadCursorX),
                                  static_cast<int>(g_state.gamepadCursorY));
  }

  bool aPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
  if (aPressed && !g_state.gamepadAPressed) {
    SteamHTMLSurface()->MouseDown(g_state.browser,
                                  ISteamHTMLSurface::eHTMLMouseButton_Left);
  } else if (!aPressed && g_state.gamepadAPressed) {
    SteamHTMLSurface()->MouseUp(g_state.browser,
                                ISteamHTMLSurface::eHTMLMouseButton_Left);
  }
  g_state.gamepadAPressed = aPressed;

  bool bPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
  if (bPressed && !g_state.gamepadBPressed) {
    SteamHTMLSurface()->MouseDown(g_state.browser,
                                  ISteamHTMLSurface::eHTMLMouseButton_Right);
  } else if (!bPressed && g_state.gamepadBPressed) {
    SteamHTMLSurface()->MouseUp(g_state.browser,
                                ISteamHTMLSurface::eHTMLMouseButton_Right);
  }
  g_state.gamepadBPressed = bPressed;
}

// ============================================================================
// Command Line Parsing
// ============================================================================

/**
 * Parses command line arguments and populates the global configuration.
 */
void ParseCommandLine(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--puffyport" && i + 1 < argc) {
      g_config.port = std::atoi(argv[++i]);
    } else if (arg == "--puffydir" && i + 1 < argc) {
      g_config.gameDir = argv[++i];
    } else if (arg == "--puffywidth" && i + 1 < argc) {
      g_config.width = std::atoi(argv[++i]);
      g_config.customSize = true;
    } else if (arg == "--puffyheight" && i + 1 < argc) {
      g_config.height = std::atoi(argv[++i]);
      g_config.customSize = true;
    }
  }

  if (g_config.customSize && (g_config.width <= 0 || g_config.height <= 0)) {
    g_config.customSize = false;
  }
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char *argv[]) {
  g_state.appDir = GetApplicationDirectory();

  ParseCommandLine(argc, argv);

  std::string gameRoot;
  if (g_config.gameDir.size() >= 2 &&
      (g_config.gameDir[1] == ':' || g_config.gameDir[0] == '\\')) {
    gameRoot = g_config.gameDir;
  } else {
    gameRoot = g_state.appDir + g_config.gameDir;
  }

  g_httpServer = new HttpServer(g_config.port, gameRoot);
  if (!g_httpServer->Start()) {
    printf("ERROR: Failed to start HTTP server on port %d\n", g_config.port);
    delete g_httpServer;
    return 1;
  }

  if (!SteamAPI_Init()) {
    printf("ERROR: SteamAPI_Init() failed. Make sure Steam is running.\n");
    g_httpServer->Stop();
    delete g_httpServer;
    return 1;
  }

  if (!SteamHTMLSurface()->Init()) {
    printf("ERROR: SteamHTMLSurface()->Init() failed\n");
    SteamAPI_Shutdown();
    g_httpServer->Stop();
    delete g_httpServer;
    return 1;
  }

  HINSTANCE hInstance = GetModuleHandle(nullptr);
  if (!CreatePuffyWindow(hInstance)) {
    printf("ERROR: Failed to create window\n");
    SteamHTMLSurface()->Shutdown();
    SteamAPI_Shutdown();
    g_httpServer->Stop();
    delete g_httpServer;
    return 1;
  }

  HandleResize();

  g_steamCallbacks = new SteamBrowserCallbacks();

  RECT rc;
  GetClientRect(g_state.hwnd, &rc);
  SteamAPICall_t hCall =
      SteamHTMLSurface()->CreateBrowser("PuffyEngine/1.0", nullptr);
  g_steamCallbacks->m_BrowserReady.Set(hCall, g_steamCallbacks,
                                       &SteamBrowserCallbacks::OnBrowserReady);

  MSG msg;
  DWORD lastGamepadPoll = GetTickCount();
  DWORD startTime = GetTickCount();
  bool loggedWaiting = false;

  while (g_state.running) {
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        g_state.running = false;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    if (!g_state.running)
      break;

    SteamAPI_RunCallbacks();

    // Log if we're waiting too long for browser ready
    if (!g_state.browserReady && !loggedWaiting) {
      DWORD elapsed = GetTickCount() - startTime;
      if (elapsed > 3000) {
        printf("WARNING: Browser not ready after %d ms. This may indicate:\n",
               elapsed);
        printf("  - Steam overlay is disabled\n");
        printf("  - Steam HTML Surface is not available\n");
        printf("  - Callback registration issue\n");
        loggedWaiting = true;
      }
    }

    if (g_state.needsResize) {
      HandleResize();
      g_state.needsResize = false;
    }

    DWORD now = GetTickCount();
    if (now - lastGamepadPoll >= GAMEPAD_POLL_INTERVAL) {
      UpdateGamepad();
      lastGamepadPoll = now;
    }

    Sleep(1); // ~1000 FPS cap, prevents CPU spinning
  }

  if (g_state.browser != INVALID_HTMLBROWSER) {
    SteamHTMLSurface()->RemoveBrowser(g_state.browser);
  }

  delete g_steamCallbacks;
  g_steamCallbacks = nullptr;

  SteamHTMLSurface()->Shutdown();
  SteamAPI_Shutdown();

  g_httpServer->Stop();
  delete g_httpServer;
  g_httpServer = nullptr;

  if (g_state.hdcMem) {
    DeleteDC(g_state.hdcMem);
  }
  if (g_state.hBitmap) {
    DeleteObject(g_state.hBitmap);
  }

  return 0;
}
