// ★ 1. 必须把所有的 Qt 头文件放在 X11 之前！
#include "../common/protocol.h"
#include <QGuiApplication>
#include <QImage>
#include <QBuffer>
#include <QByteArray>
#include <QClipboard>
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <unistd.h>
#include <dirent.h>       // ★ 新增：opendir / readdir / closedir / struct dirent
#include <sys/stat.h>     // ★ 新增：stat / S_ISDIR / struct stat

// ★ 2. 在引入 X11 之前，取消掉所有冲突的宏
#undef Bool
#undef Status
#undef CursorShape
#undef None
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef FontChange
#undef Expose
#undef Unsorted

// ★ 3. 最后再引入 X11 头文件
#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <jpeglib.h>

static std::string PASSWORD = "changeme123";
static int PORT = 5901;

// ---- JPEG 编码 ----
static std::vector<uint8_t> encode_jpeg(const uint8_t* bgrx, int w, int h, int quality) {
    std::vector<uint8_t> out;
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char* mem = nullptr;
    unsigned long memLen = 0;
    jpeg_mem_dest(&cinfo, &mem, &memLen);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    const size_t stride = (size_t)w * 4;
    std::vector<uint8_t> rowBuf((size_t)w * 3);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t* src = bgrx + stride * cinfo.next_scanline;
        for (int x = 0; x < w; ++x) {
            rowBuf[x*3]   = src[x*4+2];
            rowBuf[x*3+1] = src[x*4+1];
            rowBuf[x*3+2] = src[x*4];
        }
        JSAMPROW row = rowBuf.data();
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    out.assign(mem, mem + memLen);
    free(mem);
    jpeg_destroy_compress(&cinfo);
    return out;
}

// ---- XCB 截图 ----
struct XcbCapturer {
    xcb_connection_t* conn = nullptr;
    xcb_screen_t* screen = nullptr;
    int w = 0, h = 0;

    XcbCapturer() {
        conn = xcb_connect(nullptr, nullptr);
        screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
        w = screen->width_in_pixels;
        h = screen->height_in_pixels;
    }
    ~XcbCapturer() { if (conn) xcb_disconnect(conn); }

    std::vector<uint8_t> grab() {
        std::vector<uint8_t> out;
        auto cookie = xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, screen->root,
                                    0, 0, w, h, ~0u);
        auto* reply = xcb_get_image_reply(conn, cookie, nullptr);
        if (!reply) return out;
        auto* data = xcb_get_image_data(reply);
        size_t len = xcb_get_image_data_length(reply);
        out.assign(data, data + len);
        free(reply);
        return out;
    }
};

// ---- 输入注入 ----
struct InputInjector {
    Display* dpy = nullptr;
    InputInjector() { dpy = XOpenDisplay(nullptr); }
    ~InputInjector() { if (dpy) XCloseDisplay(dpy); }
    void move(int x, int y) { XTestFakeMotionEvent(dpy, -1, x, y, 0); XFlush(dpy); }
    void button(int b, bool down) { XTestFakeButtonEvent(dpy, b, down ? True : False, 0); XFlush(dpy); }
    void key(KeySym ks, bool down) {
        KeyCode kc = XKeysymToKeycode(dpy, ks);
        XTestFakeKeyEvent(dpy, kc, down ? True : False, 0);
        XFlush(dpy);
    }
};

static KeySym name_to_keysym(const std::string& n) {
    static const std::map<std::string, KeySym> m = {
        {"Return", XK_Return}, {"BackSpace", XK_BackSpace}, {"Tab", XK_Tab},
        {"Escape", XK_Escape}, {"Delete", XK_Delete}, {"Insert", XK_Insert},
        {"Left", XK_Left}, {"Right", XK_Right}, {"Up", XK_Up}, {"Down", XK_Down},
        {"Home", XK_Home}, {"End", XK_End}, {"Page_Up", XK_Prior}, {"Page_Down", XK_Next},
        {"space", XK_space}, {"ctrl", XK_Control_L}, {"alt", XK_Alt_L},
        {"shift", XK_Shift_L}, {"shiftleft", XK_Shift_L}, {"shiftright", XK_Shift_R},
        {"win", XK_Super_L},
    };
    auto it = m.find(n); if (it != m.end()) return it->second;
    if (n.size() == 1) return (KeySym)n[0];
    if (n.size() >= 2 && n[0] == 'F') {
        int k = atoi(n.c_str() + 1); if (k >= 1 && k <= 12) return XK_F1 + k - 1;
    }
    return NoSymbol;
}

static KeySym vk_to_keysym(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (KeySym)(vk + 32);
    if (vk >= '0' && vk <= '9') return (KeySym)vk;
    switch (vk) {
        case 8:  return XK_BackSpace; case 9:  return XK_Tab;       case 13: return XK_Return;
        case 16: return XK_Shift_L;   case 17: return XK_Control_L; case 18: return XK_Alt_L;
        case 27: return XK_Escape;    case 32: return XK_space;     case 33: return XK_Prior;
        case 34: return XK_Next;      case 35: return XK_End;       case 36: return XK_Home;
        case 37: return XK_Left;      case 38: return XK_Up;        case 39: return XK_Right;
        case 40: return XK_Down;      case 45: return XK_Insert;    case 46: return XK_Delete;
        case 91: return XK_Super_L;   case 92: return XK_Super_R;
    }
    if (vk >= 112 && vk <= 123) return XK_F1 + (vk - 112);
    return NoSymbol;
}

// ---- 剪贴板操作 ----
// ---- 剪贴板操作 ----
static void set_clipboard_text(const std::string& text) {
    // 1. 先转 base64，防止文本里的特殊符号破坏 shell 命令
    std::string b64 = proto::b64_encode(text.data(), text.size());
    
    // 2. 使用 setsid 和 & 放到后台运行，绝不阻塞主线程
    // 3. 同时兼容 xclip 和 xsel（考虑到有些系统可能未安装 xclip）
    std::string cmd = 
        "echo '" + b64 + "' | base64 -d | ( "
        "if command -v xclip >/dev/null 2>&1; then setsid xclip -selection clipboard -i >/dev/null 2>&1 & "
        "elif command -v xsel >/dev/null 2>&1; then setsid xsel -b -i >/dev/null 2>&1 & "
        "fi )";
    (void)system(cmd.c_str());
}

// 新增：统一读取剪贴板函数
static std::string get_clipboard_text() {
    std::string result;
    // 优先使用 xclip 读取 UTF8 字符串
    FILE* fp = popen("xclip -selection clipboard -t UTF8_STRING -o 2>/dev/null", "r");
    if (!fp) {
        fp = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    }
    if (fp) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) result.append(buf);
        int status = pclose(fp);
        if (status == 0 && !result.empty()) return result;
    }

    // 如果 xclip 没读到，回退尝试 xsel
    result.clear();
    fp = popen("xsel -b -o 2>/dev/null", "r");
    if (fp) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) result.append(buf);
        pclose(fp);
    }
    return result;
}

static std::string url_encode_path(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (unsigned char c : path) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

static void set_clipboard_file(const std::string& path) {
    std::string payload = "copy\nfile://" + url_encode_path(path) + "\n";
    std::string b64 = proto::b64_encode(payload.data(), payload.size());
    std::string cmd = "echo '" + b64 +
                      "' | base64 -d | setsid xclip -selection clipboard -t x-special/gnome-copied-files -i >/dev/null 2>&1 &";
    (void)system(cmd.c_str());
}

// ---- 命令执行 ----
static void execute_cmd(InputInjector& inj, const std::string& cmd) {
    if (cmd.rfind("mousemove ", 0) == 0) { int x, y; if (sscanf(cmd.c_str(), "mousemove %d %d", &x, &y) == 2) inj.move(x, y); return; }
    if (cmd.rfind("mousedown ", 0) == 0) { inj.button(atoi(cmd.c_str()+10), true); return; }
    if (cmd.rfind("mouseup ", 0) == 0)   { inj.button(atoi(cmd.c_str()+8), false); return; }
    if (cmd.rfind("click ", 0) == 0) {
        int id = atoi(cmd.c_str()+6);
        inj.button(id, true); inj.button(id, false);
        return;
    }
    if (cmd.rfind("key ", 0) == 0) {
        std::string combo = cmd.substr(4); std::vector<KeySym> syms; size_t s = 0;
        while (s < combo.size()) {
            size_t e = combo.find('+', s);
            std::string p = combo.substr(s, e == std::string::npos ? std::string::npos : e - s);
            KeySym k = name_to_keysym(p); if (k != NoSymbol) syms.push_back(k);
            if (e == std::string::npos) break; s = e + 1;
        }
        for (auto k : syms) inj.key(k, true);
        for (auto it = syms.rbegin(); it != syms.rend(); ++it) inj.key(*it, false);
        return;
    }
    if (cmd.rfind("type ", 0) == 0) {
        std::string t = cmd.substr(5);
        for (char c : t) { KeySym k = (KeySym)c; inj.key(k, true); inj.key(k, false); }
        return;
    }
    if (cmd.rfind("keydown ", 0) == 0) {
        std::string keyStr = cmd.substr(8); KeySym ks = NoSymbol;
        if (keyStr.find_first_not_of("0123456789") == std::string::npos) ks = vk_to_keysym(atoi(keyStr.c_str()));
        else ks = name_to_keysym(keyStr);
        if (ks != NoSymbol) inj.key(ks, true); return;
    }
    if (cmd.rfind("keyup ", 0) == 0) {
        std::string keyStr = cmd.substr(6); KeySym ks = NoSymbol;
        if (keyStr.find_first_not_of("0123456789") == std::string::npos) ks = vk_to_keysym(atoi(keyStr.c_str()));
        else ks = name_to_keysym(keyStr);
        if (ks != NoSymbol) inj.key(ks, false); return;
    }
}

// ============ 新增：文件系统浏览 / 下载 ============
static void handle_fs_list(const std::string& path, socket_t sock, std::mutex& sendLock) {
    QString qpath = QString::fromUtf8(path.c_str(), (int)path.size());
    QDir dir;
    if (qpath.isEmpty()) dir = QDir(QDir::homePath());
    else dir = QDir(qpath);
    QString actual = dir.absolutePath();

    QByteArray payload;
    payload.append(actual.toUtf8());
    payload.append('\n');

    if (dir.exists()) {
        QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
            QDir::DirsFirst | QDir::Name);
        for (const QFileInfo& fi : entries) {
            char type = fi.isDir() ? 'd' : 'f';
            QByteArray absUtf8 = fi.absoluteFilePath().toUtf8();
            std::string absB64 = proto::b64_encode(absUtf8.constData(), absUtf8.size());
            qint64 size = fi.isDir() ? 0 : fi.size();
            payload.append(type);
            payload.append('\t');
            payload.append(absB64.c_str());
            payload.append('\t');
            payload.append(QByteArray::number(size));
            payload.append('\n');
        }
    }

    std::lock_guard<std::mutex> lk(sendLock);
    send_frame(sock, proto::FRAME_FS_LIST, (const uint8_t*)payload.constData(), (uint32_t)payload.size());
}

static void handle_fs_get(const std::string& path, socket_t sock, std::mutex& sendLock) {
    QString qpath = QString::fromUtf8(path.c_str(), (int)path.size());
    QFile f(qpath);
    if (!f.open(QIODevice::ReadOnly)) {
        uint8_t dummy = 0;
        std::lock_guard<std::mutex> lk(sendLock);
        send_frame(sock, proto::FRAME_FS_ERROR, &dummy, 0);
        return;
    }
    qint64 size = f.size();
    uint8_t begin[8];
    for (int i = 0; i < 8; ++i) begin[i] = (uint8_t)(size >> (56 - 8 * i));

    {
        std::lock_guard<std::mutex> lk(sendLock);
        if (!send_frame(sock, proto::FRAME_DL_BEGIN, begin, 8)) return;
    }

    char buf[48 * 1024];
    qint64 n;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
        std::lock_guard<std::mutex> lk(sendLock);
        if (!send_frame(sock, proto::FRAME_DL_CHUNK, (const uint8_t*)buf, (uint32_t)n)) return;
    }
    std::lock_guard<std::mutex> lk(sendLock);
    send_frame(sock, proto::FRAME_DL_END, nullptr, 0);
}

// ---- 会话 ----
struct StreamSettings {
    std::mutex lock;
    int quality = 55;
    int fps = 12;
    int targetW = 0;
    int targetH = 0;
};

static void handle_client(socket_t sock) {
    uint32_t len;
    if (!recv_exact(sock, &len, 4)) { sock_close(sock); return; }
    len = ntohl(len);
    std::string pwd(len, '\0');
    if (!recv_exact(sock, pwd.data(), len) || pwd != PASSWORD) { uint8_t zero = 0; send_all(sock, &zero, 1); sock_close(sock); return; }
    uint8_t one = 1; send_all(sock, &one, 1);

    XcbCapturer cap;
    InputInjector inj;
    uint8_t hdr[8] = {
        (uint8_t)(cap.w >> 24), (uint8_t)(cap.w >> 16), (uint8_t)(cap.w >> 8), (uint8_t)cap.w,
        (uint8_t)(cap.h >> 24), (uint8_t)(cap.h >> 16), (uint8_t)(cap.h >> 8), (uint8_t)cap.h,
    };
    send_all(sock, hdr, 8);

    StreamSettings settings;
    std::atomic<bool> running{true};
    std::mutex sendLock;

    std::mutex clipStateLock;
    std::string lastReceivedFileUri;

    // 1. 剪贴板监控线程
    std::thread clipWatcher([&] {
        std::string lastText;
        std::string lastFileUri;

        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            bool handledFile = false;

            std::string uriData;
            {
                FILE* fpFile = popen("xclip -selection clipboard -t text/uri-list -o 2>/dev/null", "r");
                if (fpFile) {
                    char buf[4096];
                    while (fgets(buf, sizeof(buf), fpFile)) uriData += buf;
                    pclose(fpFile);
                }
            }
            if (uriData.empty()) {
                FILE* fpGnome = popen("xclip -selection clipboard -t x-special/gnome-copied-files -o 2>/dev/null", "r");
                if (fpGnome) {
                    char buf[4096];
                    std::string g;
                    while (fgets(buf, sizeof(buf), fpGnome)) g += buf;
                    pclose(fpGnome);
                    size_t nl = g.find('\n');
                    if (nl != std::string::npos) uriData = g.substr(nl + 1);
                    else uriData = g;
                }
            }

            if (!uriData.empty()) {
                std::string currentUri;
                for (size_t i = 0; i < uriData.size(); ) {
                    size_t end = uriData.find('\n', i);
                    if (end == std::string::npos) end = uriData.size();
                    std::string line = uriData.substr(i, end - i);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("file://", 0) == 0) {
                        std::string path = line.substr(7);
                        std::string decoded;
                        for (size_t j = 0; j < path.size(); ++j) {
                            if (path[j] == '%' && j + 2 < path.size()) {
                                char hex[3] = { path[j+1], path[j+2], 0 };
                                decoded += (char)strtol(hex, nullptr, 16);
                                j += 2;
                            } else {
                                decoded += path[j];
                            }
                        }
                        currentUri = decoded;
                        break;
                    }
                    i = end + 1;
                }

                if (!currentUri.empty()) {
                    handledFile = true;
                    if (currentUri != lastFileUri) {
                        lastFileUri = currentUri;

                        bool isReceived = false;
                        {
                            std::lock_guard<std::mutex> lk(clipStateLock);
                            if (!lastReceivedFileUri.empty() &&
                                lastReceivedFileUri == ("file://" + currentUri)) {
                                isReceived = true;
                            }
                        }

                        if (!isReceived) {
                            FILE* f = fopen(currentUri.c_str(), "rb");
                            if (f) {
                                fseek(f, 0, SEEK_END);
                                long sz = ftell(f);
                                fseek(f, 0, SEEK_SET);
                                if (sz > 0) {
                                    std::string baseName = currentUri;
                                    size_t slash = baseName.find_last_of('/');
                                    if (slash != std::string::npos) baseName = baseName.substr(slash + 1);
                                    if (baseName.empty()) baseName = "received_file";

                                    uint32_t nameLen = (uint32_t)baseName.size();
                                    uint64_t fileSize = (uint64_t)sz;
                                    std::vector<uint8_t> beginPayload(12 + nameLen);
                                    uint32_t nameLenBE = htonl(nameLen);
                                    memcpy(beginPayload.data(), &nameLenBE, 4);
                                    for (int i = 0; i < 8; ++i)
                                        beginPayload[4 + i] = (uint8_t)(fileSize >> (56 - 8 * i));
                                    memcpy(beginPayload.data() + 12, baseName.data(), nameLen);

                                    {
                                        std::lock_guard<std::mutex> lk(sendLock);
                                        if (!send_frame(sock, proto::FRAME_FILE_BEGIN,
                                                        beginPayload.data(), (uint32_t)beginPayload.size())) {
                                            fclose(f);
                                            running = false;
                                            return;
                                        }
                                    }

                                    bool sendOk = true;
                                    char chunk[48 * 1024];
                                    size_t n;
                                    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                                        std::lock_guard<std::mutex> lk(sendLock);
                                        if (!send_frame(sock, proto::FRAME_FILE_CHUNK,
                                                        (const uint8_t*)chunk, (uint32_t)n)) {
                                            sendOk = false;
                                            break;
                                        }
                                    }
                                    fclose(f);
                                    (void)sendOk;
                                } else {
                                    fclose(f);
                                }
                            }
                        }
                    }
                }
            }

           if (!handledFile) {
                // 使用新建的统一读取函数
                std::string text = get_clipboard_text();
                if (!text.empty() && text != lastText) {
                    lastText = text;
                    std::lock_guard<std::mutex> lk(sendLock);
                    if (!send_frame(sock, proto::FRAME_CLIPBOARD, text.c_str(), (uint32_t)text.size())) {
                        running = false;
                        return;
                    }
                }
            }
        }
    });

    // 2. 屏幕发送线程
    std::thread sender([&] {
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

            auto buf = cap.grab();
            if (!buf.empty()) {
                std::vector<uint8_t> jpg;
                if (tw > 0 && th > 0 && (tw != cap.w || th != cap.h)) {
                    QImage img(buf.data(), cap.w, cap.h, cap.w * 4, QImage::Format_RGB32);
                    QImage scaled = img.scaled(tw, th, Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation);
                    scaled = scaled.convertToFormat(QImage::Format_RGB32);
                    jpg = encode_jpeg(scaled.constBits(), scaled.width(), scaled.height(), q);
                } else {
                    jpg = encode_jpeg(buf.data(), cap.w, cap.h, q);
                }

                if (!jpg.empty()) {
                    std::lock_guard<std::mutex> lk(sendLock);
                    if (!send_frame(sock, proto::FRAME_SCREEN,
                                    jpg.data(), (uint32_t)jpg.size())) break;
                }
            }

            auto dt = std::chrono::steady_clock::now() - t0;
            auto ft = std::chrono::milliseconds(1000 / std::max(1, fps));
            if (dt < ft) std::this_thread::sleep_for(ft - dt);
        }
    });

    // 3. 文件接收状态
    FILE* recvFile = nullptr;
    std::string recvFilePath;
    QFile* putFile = nullptr;   // === 新增：fs_put 上传接收 ===

    // 4. 指令接收循环
    std::string buf;
    
    char tmp[4096];
    while (running) {
        ssize_t r = ::recv(sock, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        buf.append(tmp, r);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty() || line == "ping") continue;

            if (line.rfind("set_quality ", 0) == 0) {
                std::lock_guard<std::mutex> lk(settings.lock);
                settings.quality = std::clamp(atoi(line.c_str()+12), 10, 95);
            }
            else if (line.rfind("set_fps ", 0) == 0) {
                std::lock_guard<std::mutex> lk(settings.lock);
                settings.fps = std::clamp(atoi(line.c_str()+8), 2, 30);
            }
            else if (line.rfind("set_resolution ", 0) == 0) {
                int rw, rh;
                if (sscanf(line.c_str(), "set_resolution %d %d", &rw, &rh) == 2) {
                    std::lock_guard<std::mutex> lk(settings.lock);
                    settings.targetW = std::clamp(rw, 320, std::min(1920, cap.w));
                    settings.targetH = std::clamp(rh, 240, std::min(1080, cap.h));
                }
            }
            else if (line.rfind("clipboard ", 0) == 0) {
                std::string b64 = line.substr(10);
                set_clipboard_text(proto::b64_decode(b64));
            }
            else if (line.rfind("file_begin ", 0) == 0) {
                size_t spacePos = line.find(' ', 11);
                if (spacePos != std::string::npos) {
                    std::string nameB64 = line.substr(11, spacePos - 11);
                    std::string name = proto::b64_decode(nameB64);

                    QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
                    if (desktop.isEmpty()) desktop = QDir::homePath() + "/Desktop";

                    recvFilePath = desktop.toStdString() + "/" + name;
                    if (recvFile) { fclose(recvFile); recvFile = nullptr; }
                    recvFile = fopen(recvFilePath.c_str(), "wb");
                    if (recvFile) printf("[+] 开始接收文件，保存至: %s\n", recvFilePath.c_str());
                }
            }
            else if (line.rfind("file_chunk ", 0) == 0) {
                if (recvFile) {
                    std::string chunk = proto::b64_decode(line.substr(11));
                    fwrite(chunk.data(), 1, chunk.size(), recvFile);
                }
            }
            else if (line.rfind("file_end", 0) == 0) {
                if (recvFile) {
                    fclose(recvFile);
                    recvFile = nullptr;

                    {
                        std::lock_guard<std::mutex> lk(clipStateLock);
                        lastReceivedFileUri = "file://" + recvFilePath;
                    }

                    set_clipboard_file(recvFilePath);
                    printf("[+] 文件接收完毕，已自动放入剪贴板\n");
                }
            }
                // ★ 文件传输：根列表
    else if (line == "fs_roots") {
        std::string payload;
        std::string root = "/";
        payload += proto::b64_encode(root.data(), root.size()) + "\n";

        DIR* d = opendir("/");
        if (d) {
            std::vector<std::string> subs;
            struct dirent* ent;
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.') continue;
                std::string full = std::string("/") + ent->d_name;
                struct stat st;
                if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                    subs.push_back(full);
            }
            closedir(d);
            std::sort(subs.begin(), subs.end());
            for (auto& s : subs)
                payload += proto::b64_encode(s.data(), s.size()) + "\n";
        }
        std::lock_guard<std::mutex> lk(sendLock);
        send_frame(sock, proto::FRAME_FS_ROOTS, payload.data(), (uint32_t)payload.size());
    }
    else if (line.rfind("fs_list ", 0) == 0) {
        std::string path = proto::b64_decode(line.substr(8));
        std::string payload = path + "\n";
        DIR* d = opendir(path.c_str());
        if (d) {
            std::vector<std::pair<std::string, bool>> entries; // (fullpath, isdir)
            struct dirent* ent;
            while ((ent = readdir(d))) {
                std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                std::string full = path + "/" + name;
                struct stat st;
                if (stat(full.c_str(), &st) != 0) continue;
                entries.push_back({full, S_ISDIR(st.st_mode) != 0});
            }
            closedir(d);
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b){
                          if (a.second != b.second) return a.second; // 目录在前
                          return a.first < b.first;
                      });
            for (auto& e : entries) {
                std::string pb64 = proto::b64_encode(e.first.data(), e.first.size());
                char type = e.second ? 'd' : 'f';
                qint64 sz = 0;
                if (!e.second) {
                    struct stat st;
                    if (stat(e.first.c_str(), &st) == 0) sz = st.st_size;
                }
                payload += std::string(1, type) + "\t" + pb64
                         + "\t" + std::to_string(sz) + "\n";
            }
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

            // 尝试建父目录（失败与否不影响后面 open 判定）
            QDir().mkpath(QFileInfo(qpath).absolutePath());

            putFile = new QFile(qpath);
            if (putFile->open(QIODevice::WriteOnly)) {
                ok = true;
            } else {
                delete putFile;
                putFile = nullptr;
            }
        }

        // 成功 → FRAME_DL_BEGIN 作为 ACK；失败 → FRAME_FS_ERROR
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
                execute_cmd(inj, line);
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

int main(int argc, char** argv) {
    if (argc >= 2) {
        int p = atoi(argv[1]);
        if (p > 0 && p <= 65535) PORT = p;
    }
    if (argc >= 3) PASSWORD = argv[2];

    sock_global_init();
    socket_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("[-] 绑定端口失败"); return 1; }

    listen(srv, 5);
    printf("========================================\n");
    printf("       Linux 被控端已启动\n");
    printf("========================================\n");
    printf("   监听端口 : %d\n", PORT);
    printf("   连接密码 : %s\n", PASSWORD.c_str());
    printf("========================================\n\n");

    while (true) {
        sockaddr_in c{}; socklen_t cl = sizeof(c);
        socket_t cs = accept(srv, (sockaddr*)&c, &cl);
        if (cs == INVALID_SOCK) continue;
        std::thread(handle_client, cs).detach();
    }
    sock_global_cleanup();
    return 0;
}
