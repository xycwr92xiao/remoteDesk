#include "../common/protocol.h"
#include <windows.h>
#include <cstdio>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <chrono>
#include <QGuiApplication>
#include <QImage>
#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <fstream>
#include <map>
#include <cstdlib>
#include <shellapi.h>
#include <shlobj.h>
#include <QStorageInfo>
#include <QDir>
#include <QFileInfo>
#include <cstdarg>
#include <string>

static std::string PASSWORD = "changeme123";   // const char* → std::string（可被 argv 覆盖）
static int         PORT     = 5202;            // constexpr → 普通 int（可被 argv 覆盖）

// ============================================================
// DPI 感知 —— 用 GetProcAddress 动态调用，不依赖头文件声明/链接
// 这样 MinGW / MSVC / 任意 WINVER 都能编译通过
// ============================================================
static void enable_dpi_awareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    // 1) 优先 PerMonitorV2 (Win10 1703+)  —— 推荐
    typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(HANDLE);
    auto fn1 = (SetProcessDpiAwarenessContextFn)
        GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (fn1) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4
        if (fn1((HANDLE)(LONG_PTR)-4)) return;
    }

    // 2) 降级 shcore.SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE) (Win8.1+)
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *SetProcessDpiAwarenessFn)(int);
        auto fn2 = (SetProcessDpiAwarenessFn)
            GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn2) {
            // 2 = PROCESS_PER_MONITOR_DPI_AWARE
            if (SUCCEEDED(fn2(2))) return;
        }
    }

    // 3) 最后降级 system-DPI aware (Vista+)
    typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
    auto fn3 = (SetProcessDPIAwareFn)
        GetProcAddress(user32, "SetProcessDPIAware");
    if (fn3) fn3();
}
// ============================================================
// exe 所在目录（带末尾反斜杠）
// ============================================================
static std::wstring get_exe_dir() {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return L".\\";
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
    return p;
}

static void show_error_box(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"Remote Desktop Server",
                MB_OK | MB_ICONERROR);
}
// ============================================================
// 流参数
// ============================================================
struct StreamSettings {
    std::mutex lock;
    int quality = 78;   // ★ 55 -> 78
    int fps = 15;
    int targetW = 0;
    int targetH = 0;
};

// ============================================================
// 抓屏：始终按全分辨率 BitBlt，缩放交给 Qt（与 Linux 端一致）
// 不再用 StretchBlt(HALFTONE) 直接在 GDI 里缩放，消除硬锯齿
// ============================================================
struct ScreenCapturer {
    int w = 0, h = 0;
    HDC screenDC = nullptr;
    HDC memDC = nullptr;
    HBITMAP hBmp = nullptr;
    uint8_t* pBits = nullptr;

    ScreenCapturer() {
        screenDC = GetDC(nullptr);
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
        if (w <= 0) w = 1920;
        if (h <= 0) h = 1080;
        memDC = CreateCompatibleDC(screenDC);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;    // 负值：自上而下，与 QImage 一致
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        hBmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS,
                                (void**)&pBits, nullptr, 0);
        if (hBmp) SelectObject(memDC, hBmp);
    }
    ~ScreenCapturer() {
        if (hBmp) DeleteObject(hBmp);
        if (memDC) DeleteDC(memDC);
        if (screenDC) ReleaseDC(nullptr, screenDC);
    }

    bool grab() {
        return BitBlt(memDC, 0, 0, w, h, screenDC, 0, 0,
                      SRCCOPY | CAPTUREBLT) != 0;
    }
};
// ============================================================
// 控制台 UTF-8 输出：真控制台走 WriteConsoleW（无视代码页），
// 被重定向（IDE / 管道 / 文件）时直接写 UTF-8 字节
// ============================================================
static void log_utf8(const char* s, size_t len) {
    // GUI 子系统：stdout 已被 freopen 重定向到文件（或在极端情况下为 NULL）
    if (stdout) fwrite(s, 1, len, stdout);
}

static void logf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) log_utf8(buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}
// ============================================================
// 输入注入（与原代码一致）
// ============================================================
static void send_key(WORD vk, bool down) {
    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;

    if (vk == VK_LSHIFT) {
        in.ki.wVk = VK_SHIFT; in.ki.wScan = 0x2A;
    }
    else if (vk == VK_RSHIFT) {
        in.ki.wVk = VK_SHIFT; in.ki.wScan = 0x36;
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    else {
        in.ki.wVk = vk;
        in.ki.wScan = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
        if (vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_INSERT || vk == VK_DELETE ||
            vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
            vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    }
    SendInput(1, &in, sizeof(INPUT));
}

static WORD vk_from_name(const std::string& name) {
    static const std::map<std::string, WORD> m = {
        {"Return", VK_RETURN}, {"BackSpace", VK_BACK}, {"Tab", VK_TAB}, {"Escape", VK_ESCAPE},
        {"Delete", VK_DELETE}, {"Insert", VK_INSERT}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
        {"Up", VK_UP}, {"Down", VK_DOWN}, {"Home", VK_HOME}, {"End", VK_END},
        {"Page_Up", VK_PRIOR}, {"Page_Down", VK_NEXT}, {"space", VK_SPACE},
        {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"shift", VK_SHIFT}, {"win", VK_LWIN},
    };
    auto it = m.find(name); if (it != m.end()) return it->second;
    if (name.size() == 1) {
        char c = (char)toupper((unsigned char)name[0]);
        if (c >= 'A' && c <= 'Z') return (WORD)c;
        if (c >= '0' && c <= '9') return (WORD)c;
    }
    if (name.size() >= 2 && name[0] == 'F') {
        int n = atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) return (WORD)(VK_F1 + n - 1);
    }
    return 0;
}

static WORD parse_vk(const std::string& s) {
    if (s == "shiftleft") return VK_LSHIFT;
    if (s == "shiftright") return VK_RSHIFT;
    if (s == "ctrl") return VK_LCONTROL;
    if (s == "alt") return VK_LMENU;
    if (s == "win") return VK_LWIN;
    if (s == "space") return VK_SPACE;
    char* p;
    long v = strtol(s.c_str(), &p, 0);
    if (*p == '\0' && v > 0 && v < 256) return (WORD)v;
    return vk_from_name(s);
}

static void send_mouse_move(int x, int y) {
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw > 1 && sh > 1) {
        in.mi.dx = (LONG)((LONGLONG)x * 65535 / (sw - 1));
        in.mi.dy = (LONG)((LONGLONG)y * 65535 / (sh - 1));
        SendInput(1, &in, sizeof(INPUT));
    }
}
static void send_mouse_button(int id, bool down) {
    INPUT in{}; in.type = INPUT_MOUSE;
    if (id == 1) in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    else if (id == 3) in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    else in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    SendInput(1, &in, sizeof(INPUT));
}

static void set_clipboard_text(const std::string& text) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
    if (len > 0) {
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(wchar_t));
        if (hMem) {
            wchar_t* pMem = (wchar_t*)GlobalLock(hMem);
            if (pMem) {
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pMem, len);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }
    }
    CloseClipboard();
}

static void set_clipboard_file(const std::wstring& filepath) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();

    std::wstring doubleNullPath = filepath;
    doubleNullPath.push_back(L'\0');
    doubleNullPath.push_back(L'\0');

    size_t headerSize = sizeof(DROPFILES);
    size_t dataSize = doubleNullPath.size() * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, headerSize + dataSize);

    if (hMem) {
        char* pMem = (char*)GlobalLock(hMem);
        if (pMem) {
            DROPFILES* df = (DROPFILES*)pMem;
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            memcpy(pMem + headerSize, doubleNullPath.c_str(), dataSize);
            GlobalUnlock(hMem);
            SetClipboardData(CF_HDROP, hMem);
        }
    }
    CloseClipboard();
}

static void execute_cmd(const std::string& cmd) {
    if (cmd.rfind("keydown ", 0) == 0) {
        WORD vk = parse_vk(cmd.substr(8));
        if (vk) send_key(vk, true);
        return;
    }
    if (cmd.rfind("keyup ", 0) == 0) {
        WORD vk = parse_vk(cmd.substr(6));
        if (vk) send_key(vk, false);
        return;
    }
    if (cmd.rfind("type ", 0) == 0) {
        std::string t = cmd.substr(5);
        for (char ch : t) {
            SHORT vk = VkKeyScanA(ch);
            WORD k = LOBYTE(vk);
            bool shift = (HIBYTE(vk) & 1) != 0;
            if (k == 0xFFFF) continue;
            if (shift && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                send_key(VK_SHIFT, true);
                send_key(k, true); send_key(k, false);
                send_key(VK_SHIFT, false);
            } else {
                send_key(k, true); send_key(k, false);
            }
        }
        return;
    }
    if (cmd.rfind("mousemove ", 0) == 0) {
        int x, y;
        if (sscanf(cmd.c_str(), "mousemove %d %d", &x, &y) == 2)
            send_mouse_move(x, y);
        return;
    }
    if (cmd.rfind("mousedown ", 0) == 0) {
        int id = atoi(cmd.c_str() + 10); send_mouse_button(id, true); return;
    }
    if (cmd.rfind("mouseup ", 0) == 0) {
        int id = atoi(cmd.c_str() + 8); send_mouse_button(id, false); return;
    }
    if (cmd.rfind("click ", 0) == 0) {
        int id = atoi(cmd.c_str() + 6);
        if (id == 4 || id == 5) {
            INPUT in{}; in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = (id == 4) ? 120 : (DWORD)(-120);
            SendInput(1, &in, sizeof(INPUT));
        } else {
            send_mouse_button(id, true); send_mouse_button(id, false);
        }
        return;
    }
    if (cmd.rfind("paste ", 0) == 0) {
        std::string b64 = cmd.substr(6);
        std::string text = proto::b64_decode(b64);
        set_clipboard_text(text);
        Sleep(20);
        send_key(VK_CONTROL, true); send_key('V', true);
        send_key('V', false); send_key(VK_CONTROL, false);
        return;
    }
}

// ============================================================
// 客户端会话
// ============================================================
static void handle_client(socket_t sock) {
    uint32_t len;
    if (!recv_exact(sock, &len, 4)) { sock_close(sock); return; }
    len = ntohl(len);
    if (len > 1024) { sock_close(sock); return; }
    std::string pwd(len, '\0');
    if (!recv_exact(sock, pwd.data(), len) || pwd != PASSWORD) {
        uint8_t zero = 0; send_all(sock, &zero, 1); sock_close(sock); return;
    }
    uint8_t one = 1; send_all(sock, &one, 1);

    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    uint8_t hdr[8] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
    };
    send_all(sock, hdr, 8);

    StreamSettings settings;
    std::atomic<bool> running{true};
    std::mutex sendLock;

    std::mutex receivedLock;
    std::wstring lastReceivedFile;

    // ---- 剪贴板监控线程（与原代码完全一致） ----
    std::thread clipWatcher([&] {
        std::string lastText;
        std::wstring lastFileUri;

        while (running) {
            Sleep(500);
            if (!running) break;

            bool handledFile = false;

            if (OpenClipboard(NULL)) {
                HANDLE hDrop = GetClipboardData(CF_HDROP);
                if (hDrop) {
                    HDROP hdrop = (HDROP)hDrop;
                    wchar_t filePath[MAX_PATH];
                    if (DragQueryFileW(hdrop, 0, filePath, MAX_PATH) > 0) {
                        handledFile = true;
                        std::wstring wpath = filePath;

                        if (wpath != lastFileUri) {
                            lastFileUri = wpath;

                            bool isReceived = false;
                            {
                                std::lock_guard<std::mutex> lk(receivedLock);
                                if (!lastReceivedFile.empty() && lastReceivedFile == wpath)
                                    isReceived = true;
                            }

                            if (!isReceived) {
                                FILE* f = _wfopen(wpath.c_str(), L"rb");
                                if (f) {
                                    _fseeki64(f, 0, SEEK_END);
                                    __int64 sz = _ftelli64(f);
                                    _fseeki64(f, 0, SEEK_SET);

                                    if (sz > 0) {
                                        std::wstring wname = wpath;
                                        size_t slash = wname.find_last_of(L"\\/");
                                        if (slash != std::wstring::npos)
                                            wname = wname.substr(slash + 1);
                                        if (wname.empty()) wname = L"received_file";

                                        std::string name;
                                        {
                                            int wlen = WideCharToMultiByte(CP_UTF8, 0,
                                                wname.c_str(), -1, NULL, 0, NULL, NULL);
                                            if (wlen > 0) {
                                                name.resize(wlen - 1);
                                                WideCharToMultiByte(CP_UTF8, 0,
                                                    wname.c_str(), -1, &name[0], wlen, NULL, NULL);
                                            }
                                        }

                                        uint32_t nameLen = (uint32_t)name.size();
                                        uint64_t fileSize = (uint64_t)sz;
                                        std::vector<uint8_t> payload(12 + nameLen);
                                        uint32_t nameLenBE = htonl(nameLen);
                                        memcpy(payload.data(), &nameLenBE, 4);
                                        for (int i = 0; i < 8; ++i)
                                            payload[4 + i] = (uint8_t)(fileSize >> (56 - 8 * i));
                                        memcpy(payload.data() + 12, name.data(), nameLen);

                                        {
                                            std::lock_guard<std::mutex> lk(sendLock);
                                            if (!send_frame(sock, proto::FRAME_FILE_BEGIN,
                                                            payload.data(), (uint32_t)payload.size())) {
                                                fclose(f);
                                                running = false;
                                                break;
                                            }
                                        }

                                        char chunk[48 * 1024];
                                        size_t n;
                                        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                                            std::lock_guard<std::mutex> lk(sendLock);
                                            if (!send_frame(sock, proto::FRAME_FILE_CHUNK,
                                                            (const uint8_t*)chunk, (uint32_t)n)) {
                                                running = false;
                                                break;
                                            }
                                        }
                                    }
                                    fclose(f);
                                }
                            }
                        }
                    }
                }
                CloseClipboard();
            }

            if (!handledFile) {
                if (!OpenClipboard(NULL)) continue;
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* pData = (wchar_t*)GlobalLock(hData);
                    if (pData) {
                        int l = WideCharToMultiByte(CP_UTF8, 0, pData, -1, NULL, 0, NULL, NULL);
                        if (l > 0) {
                            std::string text(l - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, pData, -1, &text[0], l, NULL, NULL);
                            if (text != lastText) {
                                lastText = text;
                                std::lock_guard<std::mutex> lk(sendLock);
                                if (!send_frame(sock, proto::FRAME_CLIPBOARD,
                                                text.c_str(), (uint32_t)text.size())) {
                                    running = false;
                                    GlobalUnlock(hData);
                                    CloseClipboard();
                                    break;
                                }
                            }
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
        }
    });

    // ============================================================
    // ★ 屏幕发送线程：全分辨率抓屏 + Qt 双线性缩放 + 高质量 JPEG
    // ============================================================
    std::thread sender([&] {
        ScreenCapturer cap;
        while (running) {
            auto t0 = std::chrono::steady_clock::now();
            int q, fps, tw, th;
            {
                std::lock_guard<std::mutex> lk(settings.lock);
                q = settings.quality;
                fps = settings.fps;
                tw = settings.targetW;
                th = settings.targetH;
            }

            if (cap.grab() && cap.pBits) {
                QImage fullImg(cap.pBits, cap.w, cap.h,
                               cap.w * 4, QImage::Format_RGB32);

                QImage finalImg;
                if (tw > 0 && th > 0 && (tw != cap.w || th != cap.h)) {
                    // ★ 关键改动：用 Qt 双线性缩放（等同 Linux 端）
                    finalImg = fullImg.scaled(tw, th,
                                              Qt::IgnoreAspectRatio,
                                              Qt::SmoothTransformation);
                } else {
                    finalImg = fullImg;
                }

                QImage rgbImg = finalImg.convertToFormat(QImage::Format_RGB888);
                QByteArray ba;
                QBuffer buffer(&ba);
                buffer.open(QIODevice::WriteOnly);
                if (rgbImg.save(&buffer, "JPG", q) && ba.size() > 0) {
                    std::lock_guard<std::mutex> lk(sendLock);
                    if (!send_frame(sock, proto::FRAME_SCREEN,
                                    ba.constData(), (uint32_t)ba.size())) {
                        running = false;
                        break;
                    }
                }
            }

            auto dt = std::chrono::steady_clock::now() - t0;
            auto frameT = std::chrono::milliseconds(1000 / std::max(1, fps));
            if (dt < frameT) std::this_thread::sleep_for(frameT - dt);
        }
    });

    FILE* recvFile = nullptr;
    std::wstring recvFilePath;
    QFile* putFile = nullptr;

    std::string buf; char tmp[4096];
    while (running) {
        int r = ::recv(sock, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        buf.append(tmp, r);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty() || line == "ping") continue;

            if (line.rfind("set_quality ", 0) == 0) {
                std::lock_guard<std::mutex> lk(settings.lock);
                settings.quality = std::clamp(atoi(line.c_str() + 12), 10, 95);
            }
            else if (line.rfind("set_fps ", 0) == 0) {
                std::lock_guard<std::mutex> lk(settings.lock);
                settings.fps = std::clamp(atoi(line.c_str() + 8), 2, 60);
            }
            else if (line.rfind("set_resolution ", 0) == 0) {
                int rw, rh;
                if (sscanf(line.c_str(), "set_resolution %d %d", &rw, &rh) == 2) {
                    int maxW = GetSystemMetrics(SM_CXSCREEN);
                    int maxH = GetSystemMetrics(SM_CYSCREEN);
                    std::lock_guard<std::mutex> lk(settings.lock);
                    // ★ 去掉原来的 std::min(1920/1080)，允许到原始分辨率
                    settings.targetW = std::clamp(rw, 320, maxW);
                    settings.targetH = std::clamp(rh, 240, maxH);
                }
            }
            else if (line.rfind("clipboard ", 0) == 0) {
                std::string b64 = line.substr(10);
                std::string text = proto::b64_decode(b64);
                set_clipboard_text(text);
            }
            else if (line.rfind("file_begin ", 0) == 0) {
                size_t spacePos = line.find(' ', 11);
                if (spacePos != std::string::npos) {
                    std::string nameB64 = line.substr(11, spacePos - 11);
                    std::string name = proto::b64_decode(nameB64);

                    wchar_t desktopPath[MAX_PATH];
                    SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktopPath);
                    std::wstring fullPath = std::wstring(desktopPath) + L"\\";
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, NULL, 0);
                    if (wlen > 0) {
                        std::wstring wname(wlen - 1, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wname[0], wlen);
                        fullPath += wname;
                    }
                    recvFilePath = fullPath;

                    if (recvFile) { fclose(recvFile); recvFile = nullptr; }
                    recvFile = _wfopen(fullPath.c_str(), L"wb");
                    if (recvFile) logf("[+] 开始接收文件，保存至桌面\n");
                }
            }
            else if (line.rfind("file_chunk ", 0) == 0) {
                if (recvFile) {
                    std::string b64 = line.substr(11);
                    std::string chunk = proto::b64_decode(b64);
                    fwrite(chunk.data(), 1, chunk.size(), recvFile);
                }
            }
            else if (line.rfind("file_end", 0) == 0) {
                if (recvFile) {
                    fclose(recvFile);
                    recvFile = nullptr;
                    {
                        std::lock_guard<std::mutex> lk(receivedLock);
                        lastReceivedFile = recvFilePath;
                    }
                    set_clipboard_file(recvFilePath);
                    logf("[+] 文件接收完毕，已自动放入剪贴板\n");
                }
            }
            else if (line == "fs_roots") {
                std::string payload;
                for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
                    if (!v.isValid() || !v.isReady()) continue;
                    std::string p = v.rootPath().toStdString();
                    payload += proto::b64_encode(p.data(), p.size()) + "\n";
                }
                if (payload.empty()) {
                    std::string p = "C:\\";
                    payload = proto::b64_encode(p.data(), p.size()) + "\n";
                }
                std::lock_guard<std::mutex> lk(sendLock);
                send_frame(sock, proto::FRAME_FS_ROOTS, payload.data(), (uint32_t)payload.size());
            }
            else if (line.rfind("fs_list ", 0) == 0) {
                std::string path = proto::b64_decode(line.substr(8));
                QString qpath = QString::fromStdString(path);
                std::string payload = path + "\n";
                const QFileInfoList entries = QDir(qpath).entryInfoList(
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDir::DirsFirst | QDir::Name);
                for (const QFileInfo& fi : entries) {
                    std::string p = fi.absoluteFilePath().toStdString();
                    std::string pb64 = proto::b64_encode(p.data(), p.size());
                    char type = fi.isDir() ? 'd' : 'f';
                    qint64 sz = fi.isDir() ? 0 : fi.size();
                    payload += std::string(1, type) + "\t" + pb64 + "\t" + std::to_string(sz) + "\n";
                }
                std::lock_guard<std::mutex> lk(sendLock);
                send_frame(sock, proto::FRAME_FS_LIST, payload.data(), (uint32_t)payload.size());
            }
            else if (line.rfind("fs_get ", 0) == 0) {
                std::string path = proto::b64_decode(line.substr(7));
                QFile f(QString::fromStdString(path));
                if (!f.open(QIODevice::ReadOnly)) {
                    std::lock_guard<std::mutex> lk(sendLock);
                    send_frame(sock, proto::FRAME_FS_ERROR, nullptr, 0);
                } else {
                    { std::lock_guard<std::mutex> lk(sendLock);
                      send_frame(sock, proto::FRAME_DL_BEGIN, nullptr, 0); }
                    bool ok = true;
                    while (!f.atEnd()) {
                        QByteArray chunk = f.read(48 * 1024);
                        if (chunk.isEmpty()) break;
                        std::lock_guard<std::mutex> lk(sendLock);
                        if (!send_frame(sock, proto::FRAME_DL_CHUNK,
                                        (const uint8_t*)chunk.constData(),
                                        (uint32_t)chunk.size())) { ok = false; break; }
                    }
                    if (ok) {
                        std::lock_guard<std::mutex> lk(sendLock);
                        send_frame(sock, proto::FRAME_DL_END, nullptr, 0);
                    }
                    f.close();
                }
            }
            else if (line.rfind("fs_put_begin ", 0) == 0) {
                if (putFile) { putFile->close(); delete putFile; putFile = nullptr; }

                bool ok = false;
                size_t sp = line.find(' ', 13);
                if (sp != std::string::npos) {
                    std::string path = proto::b64_decode(line.substr(13, sp - 13));
                    QString qpath = QString::fromStdString(path);
                    QDir().mkpath(QFileInfo(qpath).absolutePath());

                    putFile = new QFile(qpath);
                    if (putFile->open(QIODevice::WriteOnly)) {
                        ok = true;
                    } else {
                        delete putFile;
                        putFile = nullptr;
                    }
                }
                std::lock_guard<std::mutex> lk(sendLock);
                send_frame(sock, ok ? proto::FRAME_DL_BEGIN : proto::FRAME_FS_ERROR, nullptr, 0);
            }
            else if (line.rfind("fs_put_chunk ", 0) == 0) {
                if (putFile) {
                    std::string chunk = proto::b64_decode(line.substr(13));
                    putFile->write(chunk.data(), (qint64)chunk.size());
                }
            }
            else if (line == "fs_put_end") {
                if (putFile) { putFile->close(); delete putFile; putFile = nullptr; }
            }
            else {
                execute_cmd(line);
            }
        }
    }
    running = false;
    if (recvFile) fclose(recvFile);
    if (putFile) { putFile->close(); delete putFile; putFile = nullptr; }
    if (clipWatcher.joinable()) clipWatcher.join();
    if (sender.joinable()) sender.join();
    sock_close(sock);
}

// ============================================================
// 局域网 IP
// ============================================================
static std::string get_lan_ip() {
    char hostname[256] = { 0 };
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent* host = gethostbyname(hostname);
        if (host != nullptr) {
            for (int i = 0; host->h_addr_list[i] != nullptr; ++i) {
                struct in_addr addr;
                memcpy(&addr, host->h_addr_list[i], sizeof(struct in_addr));
                char* ip_str = inet_ntoa(addr);
                if (ip_str) {
                    std::string ip = ip_str;
                    if (ip.find("192.168.") == 0 || ip.find("10.") == 0)
                        return ip;
                }
            }
        }
    }
    return "127.0.0.1";
}

// ============================================================
// main
// ============================================================
int main(int argc, char** argv) {
    if (argc >= 2) {
            int p = atoi(argv[1]);
            if (p > 0 && p <= 65535) PORT = p;
        }
        if (argc >= 3) PASSWORD = argv[2];
        // ---- 单实例锁：双击两次时第二次直接退出，不抢端口、不覆盖日志 ----
            HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\RemoteDesktopServer_Mutex");
            if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(hMutex);
                MessageBoxW(nullptr, L"服务端已在运行中。", L"Remote Desktop Server",
                            MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            enable_dpi_awareness();

            // ---- 切到 exe 目录 + 日志重定向（GUI 子系统下 printf/logf 全部写文件） ----
            std::wstring baseDir = get_exe_dir();
            SetCurrentDirectoryW(baseDir.c_str());
            freopen("server_log.txt", "w", stdout);
            freopen("server_err.txt", "w", stderr);
            if (stdout) setvbuf(stdout, nullptr, _IOLBF, 0);   // 行缓冲，崩了也有日志

            QGuiApplication app(argc, argv);
            std::ofstream log("startup_log.txt");
            log << "Program started!" << std::endl;
            log.close();
            sock_global_init();
    std::string local_ip = get_lan_ip();

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    logf("========================================\n");
    logf("       Remote Desktop Server Started\n");
    logf("========================================\n");
    logf("   LAN IP Address : %s\n", local_ip.c_str());
    logf("   Port           : %d\n", PORT);
    logf("   Password       : %s\n", PASSWORD.c_str());
    logf("   Screen         : %dx%d\n", sw, sh);
    logf("========================================\n");
    logf(" 用法: server_win.exe [port] [password]\n");
    logf(" 例:   server_win.exe 5202 mySecret\n");
    logf("========================================\n");
    logf(" Waiting for client connection...\n\n");
    if (stdout) fflush(stdout);

        // ---- socket ----
        socket_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
        if (srv == INVALID_SOCK) {
            logf("[-] socket failed\n");
            if (stdout) fflush(stdout);
            show_error_box(L"socket 初始化失败\n日志：" + baseDir + L"server_log.txt");
            return 1;
        }
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(PORT);

        if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
            logf("[-] Bind failed! Check if port %d is in use.\n", PORT);
            if (stdout) fflush(stdout);
            show_error_box(L"端口 " + std::to_wstring(PORT) +
                           L" 绑定失败，可能已被占用。\n日志：" +
                           baseDir + L"server_log.txt");
            return 1;
        }

        listen(srv, 5);

        while (true) {
            sockaddr_in c{};
            socklen_t cl = sizeof(c);
            socket_t cs = accept(srv, (sockaddr*)&c, &cl);
            if (cs == INVALID_SOCK) continue;

            logf("[+] Client connected!\n");
            if (stdout) fflush(stdout);
            std::thread(handle_client, cs).detach();
        }

        sock_global_cleanup();
        return 0;
}
