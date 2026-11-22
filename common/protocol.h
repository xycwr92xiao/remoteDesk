#pragma once

// ★ 必须加在这，防止 Windows.h 引入过时的 winsock.h 导致内存冲突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socket_t = SOCKET;
  static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t INVALID_SOCK = -1;
#endif

namespace proto {
    constexpr uint8_t  FRAME_SCREEN      = 0x01;
    constexpr uint8_t  FRAME_CLIPBOARD   = 0x02;
    constexpr uint8_t  FRAME_FILE_BEGIN  = 0x03;
    constexpr uint8_t  FRAME_FILE_CHUNK  = 0x04;
    constexpr uint8_t  FRAME_FILE_END    = 0x05;
    constexpr uint8_t  FRAME_SCREEN_DIFF = 0x06;
// ★ 新增：Linux → Windows 剪贴板文件同步（方向与 FRAME_FILE_* 相反）
constexpr uint8_t  FRAME_CLIPBOARD_FILE_BEGIN = 0x07;
constexpr uint8_t  FRAME_CLIPBOARD_FILE_CHUNK = 0x08;
constexpr uint8_t  FRAME_CLIPBOARD_FILE_END   = 0x09;
// ===== 追加到 common/protocol.h 的 namespace proto 中 =====
constexpr uint8_t FRAME_FS_LIST  = 0x20;  // 目录列表
constexpr uint8_t FRAME_DL_BEGIN = 0x21;  // 远端->本地 下载开始
constexpr uint8_t FRAME_DL_CHUNK = 0x22;  // 远端->本地 下载数据
constexpr uint8_t FRAME_DL_END   = 0x23;  // 远端->本地 下载结束
constexpr uint8_t FRAME_FS_ERROR = 0x24;  // 文件操作错误
constexpr uint8_t FRAME_FS_ROOTS = 0x25;  // 获取远端磁盘/根目录列表
// ========================================================
    constexpr uint32_t MAX_FRAME         = 20u * 1024 * 1024;

    // base64 编码
    inline std::string b64_encode(const void* data, size_t len) {
        static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const uint8_t* p = (const uint8_t*)data;
        std::string out;
        out.reserve((len + 2) / 3 * 4);
        for (size_t i = 0; i < len; i += 3) {
            uint32_t v = (uint32_t)p[i] << 16;
            if (i + 1 < len) v |= (uint32_t)p[i+1] << 8;
            if (i + 2 < len) v |= p[i+2];
            out.push_back(T[(v >> 18) & 0x3f]);
            out.push_back(T[(v >> 12) & 0x3f]);
            out.push_back(i+1 < len ? T[(v >> 6) & 0x3f] : '=');
            out.push_back(i+2 < len ? T[v & 0x3f] : '=');
        }
        return out;
    }

    // base64 解码 (新增！)
    inline std::string b64_decode(const std::string& in) {
        std::string out;
        std::vector<int> T(256, -1);
        for (int i = 0; i < 64; i++) T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;

        int val = 0, valb = -8;
        for (unsigned char c : in) {
            if (T[c] == -1) break; // 遇到 '=' 或无效字符停止
            val = (val << 6) + T[c];
            valb += 6;
            if (valb >= 0) {
                out.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }
}

inline void sock_global_init() {
#if defined(_WIN32)
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
}
inline void sock_global_cleanup() {
#if defined(_WIN32)
    WSACleanup();
#endif
}
inline void sock_close(socket_t s) {
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

inline bool recv_exact(socket_t fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
#if defined(_WIN32)
        int r = ::recv(fd, (char*)p + got, (int)(n - got), 0);
#else
        ssize_t r = ::recv(fd, (char*)p + got, n - got, 0);
#endif
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

inline bool send_all(socket_t fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < n) {
#if defined(_WIN32)
        int r = ::send(fd, (const char*)p + sent, (int)(n - sent), 0);
#else
        ssize_t r = ::send(fd, (const char*)p + sent, n - sent, MSG_NOSIGNAL);
#endif
        if (r <= 0) return false;
        sent += (size_t)r;
    }
    return true;
}

inline bool send_frame(socket_t fd, uint8_t type, const void* data, uint32_t len) {
    uint8_t hdr[5] = { type,
        (uint8_t)(len >> 24), (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),  (uint8_t)len };
    if (!send_all(fd, hdr, 5)) return false;
    if (len && !send_all(fd, data, len)) return false;
    return true;
}
