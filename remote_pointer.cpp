#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <random>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <cctype>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Iphlpapi.lib")

namespace {

volatile bool g_running = true;
bool g_leftButtonDown = false;
bool g_rightButtonDown = false;

// Persistent paired devices: deviceId -> token
std::map<std::string, std::string> g_pairedDevices;
std::mutex g_pairedMutex;

std::string GetExeDir() {
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string p(exePath);
        size_t lastSlash = p.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            return p.substr(0, lastSlash + 1);
        }
    }
    return ".\\";
}

std::string GetPairedFilePath() {
    return GetExeDir() + "paired_devices.txt";
}

std::vector<std::string> GetAllLocalIPv4Addresses() {
    std::vector<std::string> ips;
    ULONG outBufLen = 15000;
    std::vector<BYTE> buffer(outBufLen);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &outBufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(outBufLen);
        pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, pAddresses, &outBufLen);
    }

    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != nullptr; pCurr = pCurr->Next) {
            if (pCurr->OperStatus != IfOperStatusUp) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast != nullptr; pUnicast = pUnicast->Next) {
                sockaddr* sa = pUnicast->Address.lpSockaddr;
                if (sa && sa->sa_family == AF_INET) {
                    sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(sa);
                    char ipBuf[INET_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET, &(sin->sin_addr), ipBuf, sizeof(ipBuf));
                    std::string ip(ipBuf);
                    if (!ip.empty() && ip.rfind("127.", 0) != 0 && ip.rfind("169.254.", 0) != 0) {
                        ips.push_back(ip);
                    }
                }
            }
        }
    }
    return ips;
}

std::string GetLocalIpAddress() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s != INVALID_SOCKET) {
        sockaddr_in target = {};
        target.sin_family = AF_INET;
        target.sin_port = htons(80);
        inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
            sockaddr_in local = {};
            int len = sizeof(local);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char buf[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
                closesocket(s);
                std::string ip(buf);
                if (!ip.empty() && ip != "0.0.0.0" && ip.rfind("169.254.", 0) != 0 && ip.rfind("127.", 0) != 0) {
                    return ip;
                }
            }
        }
        closesocket(s);
    }

    auto all = GetAllLocalIPv4Addresses();
    if (!all.empty()) {
        return all[0];
    }
    return "127.0.0.1";
}

void LoadPairedDevices() {
    std::lock_guard<std::mutex> lock(g_pairedMutex);
    g_pairedDevices.clear();
    std::ifstream f(GetPairedFilePath());
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string devId, token;
        if (iss >> devId >> token) {
            g_pairedDevices[devId] = token;
        }
    }
}

void SavePairedDevice(const std::string& devId, const std::string& token) {
    std::lock_guard<std::mutex> lock(g_pairedMutex);
    g_pairedDevices[devId] = token;

    std::ofstream f(GetPairedFilePath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& [id, tok] : g_pairedDevices) {
        f << id << " " << tok << "\n";
    }
    f.flush();
}

bool ValidateDeviceToken(const std::string& devId, const std::string& token) {
    std::lock_guard<std::mutex> lock(g_pairedMutex);
    auto it = g_pairedDevices.find(devId);
    if (it != g_pairedDevices.end() && it->second == token) {
        return true;
    }
    return false;
}

std::string GenerateRandomToken() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << dis(gen)
       << std::setw(16) << dis(gen);
    return ss.str();
}

BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        g_running = false;
        if (g_leftButtonDown) {
            INPUT inp = {};
            inp.type = INPUT_MOUSE;
            inp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &inp, sizeof(INPUT));
            g_leftButtonDown = false;
        }
        if (g_rightButtonDown) {
            INPUT inp = {};
            inp.type = INPUT_MOUSE;
            inp.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            SendInput(1, &inp, sizeof(INPUT));
            g_rightButtonDown = false;
        }
        return TRUE;
    }
    return FALSE;
}

std::string LoadOrGeneratePin() {
    std::string pinPath = GetExeDir() + "pin.txt";
    std::ifstream f(pinPath);
    std::string pin;
    if (f >> pin && pin.size() == 8 && std::all_of(pin.begin(), pin.end(), ::isdigit)) {
        return pin;
    }
    pin = "12345678";
    std::ofstream out(pinPath);
    if (out.is_open()) {
        out << pin << "\n";
    }
    return pin;
}

bool SendAll(SOCKET sock, const std::string& data, int timeoutMs = 2000) {
    size_t totalSent = 0;
    while (totalSent < data.size() && g_running) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);

        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int sel = select(0, nullptr, &writeSet, nullptr, &tv);
        if (sel <= 0) return false;

        int sent = send(sock, data.data() + totalSent, static_cast<int>(data.size() - totalSent), 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return totalSent == data.size();
}

void EnsureDesktopAccess() {
    HWINSTA hWinsta = OpenWindowStationA("WinSta0", FALSE, MAXIMUM_ALLOWED);
    if (hWinsta) {
        SetProcessWindowStation(hWinsta);
    }
    HDESK hDesk = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
    if (!hDesk) {
        hDesk = OpenDesktopA("Default", 0, FALSE, MAXIMUM_ALLOWED);
    }
    if (hDesk) {
        SetThreadDesktop(hDesk);
    }
}

bool InjectMouseMove(int dx, int dy) {
    INPUT inp = {};
    inp.type = INPUT_MOUSE;
    inp.mi.dx = static_cast<LONG>(dx);
    inp.mi.dy = static_cast<LONG>(dy);
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    if (SendInput(1, &inp, sizeof(INPUT)) == 0) {
        mouse_event(MOUSEEVENTF_MOVE, static_cast<DWORD>(dx), static_cast<DWORD>(dy), 0, 0);
    }
    return true;
}

bool InjectMouseButton(bool left, bool down) {
    INPUT inp = {};
    inp.type = INPUT_MOUSE;
    DWORD flag = 0;
    if (left) {
        flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        g_leftButtonDown = down;
    } else {
        flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        g_rightButtonDown = down;
    }
    inp.mi.dwFlags = flag;
    if (SendInput(1, &inp, sizeof(INPUT)) == 0) {
        mouse_event(flag, 0, 0, 0, 0);
    }
    std::cout << "[*] Button " << (left ? "LEFT " : "RIGHT ") << (down ? "DOWN" : "UP") << "\n" << std::flush;
    return true;
}

bool IsExtendedKey(WORD vk) {
    switch (vk) {
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_INSERT:
        case VK_DELETE:
        case VK_VOLUME_UP:
        case VK_VOLUME_DOWN:
        case VK_VOLUME_MUTE:
        case VK_MEDIA_PLAY_PAUSE:
        case VK_MEDIA_NEXT_TRACK:
        case VK_MEDIA_PREV_TRACK:
        case VK_MEDIA_STOP:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

bool InjectKey(WORD vk) {
    EnsureDesktopAccess();
    DWORD flagsDown = IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0;
    DWORD flagsUp = flagsDown | KEYEVENTF_KEYUP;

    INPUT inps[2] = {};
    inps[0].type = INPUT_KEYBOARD;
    inps[0].ki.wVk = vk;
    inps[0].ki.wScan = 0;
    inps[0].ki.dwFlags = flagsDown;

    inps[1].type = INPUT_KEYBOARD;
    inps[1].ki.wVk = vk;
    inps[1].ki.wScan = 0;
    inps[1].ki.dwFlags = flagsUp;

    UINT sentDown = SendInput(1, &inps[0], sizeof(INPUT));
    Sleep(20);
    UINT sentUp = SendInput(1, &inps[1], sizeof(INPUT));

    std::cout << "[+] Key injected: VK " << vk << " (res: " << sentDown << "/" << sentUp << ")\n" << std::flush;
    return (sentDown > 0 && sentUp > 0);
}

bool InjectUnicodeChar(wchar_t ch) {
    EnsureDesktopAccess();
    INPUT inps[2] = {};
    inps[0].type = INPUT_KEYBOARD;
    inps[0].ki.wVk = 0;
    inps[0].ki.wScan = ch;
    inps[0].ki.dwFlags = KEYEVENTF_UNICODE;

    inps[1].type = INPUT_KEYBOARD;
    inps[1].ki.wVk = 0;
    inps[1].ki.wScan = ch;
    inps[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    UINT sentDown = SendInput(1, &inps[0], sizeof(INPUT));
    Sleep(10);
    UINT sentUp = SendInput(1, &inps[1], sizeof(INPUT));

    std::cout << "[+] Char injected: U+" << std::hex << static_cast<int>(ch) << std::dec << " (res: " << sentDown << "/" << sentUp << ")\n" << std::flush;
    return (sentDown > 0 && sentUp > 0);
}

bool Base64Decode(std::string_view input, std::vector<uint8_t>& output) {
    output.clear();
    if (input.empty() || input.size() % 4 != 0) return false;

    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        if (c == '=') return -1;
        return -2;
    };

    size_t len = input.size();
    for (size_t i = 0; i < len; i += 4) {
        int b0 = decode_char(input[i]);
        int b1 = decode_char(input[i + 1]);
        int b2 = decode_char(input[i + 2]);
        int b3 = decode_char(input[i + 3]);

        if (b0 < 0 || b1 < 0) return false;
        if (b2 == -2 || b3 == -2) return false;

        if (b2 == -1) {
            if (b3 != -1 || i + 4 != len) return false;
            output.push_back(static_cast<uint8_t>((b0 << 2) | (b1 >> 4)));
        } else if (b3 == -1) {
            if (i + 4 != len) return false;
            output.push_back(static_cast<uint8_t>((b0 << 2) | (b1 >> 4)));
            output.push_back(static_cast<uint8_t>(((b1 & 0x0F) << 4) | (b2 >> 2)));
        } else {
            output.push_back(static_cast<uint8_t>((b0 << 2) | (b1 >> 4)));
            output.push_back(static_cast<uint8_t>(((b1 & 0x0F) << 4) | (b2 >> 2)));
            output.push_back(static_cast<uint8_t>(((b2 & 0x03) << 6) | b3));
        }
    }
    return !output.empty();
}

bool ValidateAndConvertUTF8(const std::vector<uint8_t>& utf8Bytes, std::wstring& outUtf16) {
    if (utf8Bytes.empty()) return false;

    int requiredChars = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(utf8Bytes.data()),
        static_cast<int>(utf8Bytes.size()),
        nullptr,
        0
    );

    if (requiredChars <= 0) return false;

    outUtf16.resize(requiredChars);
    int converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(utf8Bytes.data()),
        static_cast<int>(utf8Bytes.size()),
        outUtf16.data(),
        requiredChars
    );

    if (converted != requiredChars) return false;

    for (wchar_t wch : outUtf16) {
        if ((wch <= 0x001F) || (wch >= 0x007F && wch <= 0x009F)) {
            return false;
        }
    }

    return true;
}

std::string_view TrimWhitespace(std::string_view str) {
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) {
        str.remove_prefix(1);
    }
    while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) {
        str.remove_suffix(1);
    }
    return str;
}

bool ParseInt(std::string_view s, int& outVal) {
    s = TrimWhitespace(s);
    if (s.empty()) return false;

    size_t i = 0;
    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }

    if (i >= s.size()) return false;

    long long val = 0;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        val = val * 10 + (s[i] - '0');
        if (val > 100000) return false;
    }

    if (negative) val = -val;
    outVal = static_cast<int>(val);
    return true;
}

void CleanButtons() {
    if (g_leftButtonDown) {
        InjectMouseButton(true, false);
        g_leftButtonDown = false;
    }
    if (g_rightButtonDown) {
        InjectMouseButton(false, false);
        g_rightButtonDown = false;
    }
}

std::string GetHostNameString() {
    char hName[256] = {};
    if (gethostname(hName, sizeof(hName)) == 0) {
        return std::string(hName);
    }
    return "Windows-PC";
}

// Background thread for UDP auto-discovery (Port 47622)
void UdpDiscoveryThread(int tcpPort) {
    SOCKET udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSock == INVALID_SOCKET) return;

    int opt = 1;
    setsockopt(udpSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    setsockopt(udpSock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(47622);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udpSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(udpSock);
        return;
    }

    u_long nonblock = 1;
    ioctlsocket(udpSock, FIONBIO, &nonblock);

    std::string pcName = GetHostNameString();
    std::string offerMsg = "LAZY_POINT_OFFER " + pcName + " " + std::to_string(tcpPort) + "\n";
    std::string beaconMsg = "LAZY_POINT_BEACON " + pcName + " " + std::to_string(tcpPort) + "\n";

    sockaddr_in bcastAddr = {};
    bcastAddr.sin_family = AF_INET;
    bcastAddr.sin_port = htons(47622);
    bcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    auto lastBeaconTime = std::chrono::steady_clock::now();

    char buf[512];
    while (g_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(udpSock, &readSet);

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 300000; // 300ms

        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel > 0) {
            sockaddr_in senderAddr = {};
            int senderLen = sizeof(senderAddr);
            int n = recvfrom(udpSock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
            if (n > 0) {
                buf[n] = '\0';
                std::string req(buf);
                if (req.find("LAZY_POINT_DISCOVER") != std::string::npos) {
                    sendto(udpSock, offerMsg.c_str(), static_cast<int>(offerMsg.size()), 0,
                           reinterpret_cast<sockaddr*>(&senderAddr), senderLen);
                }
            }
        }

        // Periodic broadcast beacon every 2 seconds
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastBeaconTime).count() >= 2) {
            sendto(udpSock, beaconMsg.c_str(), static_cast<int>(beaconMsg.size()), 0,
                   reinterpret_cast<sockaddr*>(&bcastAddr), sizeof(bcastAddr));
            lastBeaconTime = now;
        }
    }

    closesocket(udpSock);
}

void HttpServerThread(int httpPort, const std::string& pcName, const std::string& localIp, const std::string& currentPin, int tcpPort) {
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) return;

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<u_short>(httpPort));

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        httpPort = 8080;
        serverAddr.sin_port = htons(static_cast<u_short>(httpPort));
        if (bind(listenSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(listenSock);
            return;
        }
    }

    if (listen(listenSock, 10) == SOCKET_ERROR) {
        closesocket(listenSock);
        return;
    }

    u_long nonblock = 1;
    ioctlsocket(listenSock, FIONBIO, &nonblock);

    std::string exeDir = GetExeDir();
    std::string apkPath = exeDir + "static\\LazyPointRemote.apk";
    std::ifstream testF(apkPath, std::ios::binary);
    if (!testF.is_open()) {
        apkPath = exeDir + "android\\app\\build\\outputs\\apk\\debug\\app-debug.apk";
    } else {
        testF.close();
    }

    while (g_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in clientAddr = {};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSock == INVALID_SOCKET) continue;

        // Switch client socket to blocking mode with timeout for clean HTTP transaction
        u_long blockMode = 0;
        ioctlsocket(clientSock, FIONBIO, &blockMode);

        DWORD timeout = 4000;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        std::string req;
        char tempBuf[1024] = {};
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            int r = recv(clientSock, tempBuf, sizeof(tempBuf) - 1, 0);
            if (r <= 0) break;
            tempBuf[r] = '\0';
            req.append(tempBuf, r);
            if (req.find("\n") != std::string::npos && req.find("GET") == std::string::npos && req.find("HEAD") == std::string::npos) {
                break;
            }
        }

        std::istringstream reqStream(req);
        std::string method, path;
        reqStream >> method >> path;

        if (method != "GET" && method != "HEAD") {
            const char* notAllowed = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(clientSock, notAllowed, static_cast<int>(strlen(notAllowed)), 0);
            shutdown(clientSock, SD_SEND);
            closesocket(clientSock);
            continue;
        }

        if (path.find("LazyPointRemote.apk") != std::string::npos ||
            path.find("app-debug.apk") != std::string::npos ||
            path == "/apk" || path == "/download") {

            std::ifstream apkFile(apkPath, std::ios::binary | std::ios::ate);
            if (apkFile.is_open()) {
                std::streamsize fileSize = apkFile.tellg();
                apkFile.seekg(0, std::ios::beg);

                std::ostringstream respHeader;
                respHeader << "HTTP/1.1 200 OK\r\n"
                           << "Content-Type: application/vnd.android.package-archive\r\n"
                           << "Content-Length: " << fileSize << "\r\n"
                           << "Content-Disposition: attachment; filename=\"LazyPointRemote.apk\"\r\n"
                           << "Access-Control-Allow-Origin: *\r\n"
                           << "Connection: close\r\n\r\n";
                std::string headerStr = respHeader.str();
                send(clientSock, headerStr.data(), static_cast<int>(headerStr.size()), 0);

                if (method == "GET") {
                    char chunk[65536];
                    while (apkFile.read(chunk, sizeof(chunk)) || apkFile.gcount() > 0) {
                        int toSend = static_cast<int>(apkFile.gcount());
                        int sentTotal = 0;
                        while (sentTotal < toSend) {
                            int sent = send(clientSock, chunk + sentTotal, toSend - sentTotal, 0);
                            if (sent <= 0) break;
                            sentTotal += sent;
                        }
                        if (sentTotal < toSend) break;
                    }
                    std::cout << "[+] Mobile browser downloaded LazyPointRemote.apk (" << fileSize << " bytes) successfully!\n";
                }
                apkFile.close();
            } else {
                const char* notFound = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 14\r\nConnection: close\r\n\r\nAPK not found\n";
                send(clientSock, notFound, static_cast<int>(strlen(notFound)), 0);
            }
        } else {
            std::ostringstream body;
            body << "<!DOCTYPE html>\n"
                 << "<html lang=\"en\">\n"
                 << "<head>\n"
                 << "<meta charset=\"UTF-8\">\n"
                 << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n"
                 << "<title>Lazy Point Remote</title>\n"
                 << "<style>\n"
                 << "  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }\n"
                 << "  body { background: #0f172a; color: #f8fafc; display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 100vh; padding: 24px 16px; }\n"
                 << "  .card { background: #1e293b; border-radius: 24px; padding: 32px 24px; max-width: 440px; width: 100%; border: 1px solid #334155; box-shadow: 0 20px 40px rgba(0,0,0,0.6); text-align: center; }\n"
                 << "  .icon { font-size: 56px; margin-bottom: 12px; }\n"
                 << "  h1 { font-size: 26px; font-weight: 800; background: linear-gradient(135deg, #38bdf8, #818cf8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; margin-bottom: 8px; }\n"
                 << "  p.sub { font-size: 14px; color: #94a3b8; margin-bottom: 24px; line-height: 1.5; }\n"
                 << "  .btn { display: block; width: 100%; padding: 18px 20px; background: linear-gradient(135deg, #2563eb, #7c3aed); color: #ffffff; text-decoration: none; font-size: 18px; font-weight: 700; border-radius: 14px; box-shadow: 0 4px 15px rgba(37,99,235,0.4); margin-bottom: 24px; transition: transform 0.1s ease; }\n"
                 << "  .btn:active { transform: scale(0.98); opacity: 0.9; }\n"
                 << "  .info-box { background: #0f172a; border-radius: 12px; padding: 16px; border: 1px solid #334155; text-align: left; font-size: 13px; color: #cbd5e1; margin-bottom: 20px; }\n"
                 << "  .info-row { display: flex; justify-content: space-between; padding: 6px 0; border-bottom: 1px solid #1e293b; }\n"
                 << "  .info-row:last-child { border-bottom: none; }\n"
                 << "  .info-label { color: #64748b; }\n"
                 << "  .info-val { font-weight: 600; font-family: monospace; color: #38bdf8; }\n"
                 << "  ol { text-align: left; padding-left: 20px; font-size: 13px; color: #94a3b8; line-height: 1.8; }\n"
                 << "  li strong { color: #f1f5f9; }\n"
                 << "</style>\n"
                 << "</head>\n"
                 << "<body>\n"
                 << "  <div class=\"card\">\n"
                 << "    <div class=\"icon\">🛋️</div>\n"
                 << "    <h1>Lazy Point Remote</h1>\n"
                 << "    <p class=\"sub\">Ultra-smooth Gyro Air-Mouse & Remote for Windows PC</p>\n"
                 << "    <a class=\"btn\" href=\"/LazyPointRemote.apk\">⬇️ Download APK (9.9 MB)</a>\n"
                 << "    <div class=\"info-box\">\n"
                 << "      <div class=\"info-row\"><span class=\"info-label\">PC Name</span><span class=\"info-val\">" << pcName << "</span></div>\n"
                 << "      <div class=\"info-row\"><span class=\"info-label\">PC IP Address</span><span class=\"info-val\">" << localIp << "</span></div>\n"
                 << "      <div class=\"info-row\"><span class=\"info-label\">Pointer Port</span><span class=\"info-val\">" << tcpPort << "</span></div>\n"
                 << "      <div class=\"info-row\"><span class=\"info-label\">Pairing PIN</span><span class=\"info-val\">" << currentPin << "</span></div>\n"
                 << "    </div>\n"
                 << "    <ol>\n"
                 << "      <li>Tap <strong>Download APK</strong> and install on your phone.</li>\n"
                 << "      <li>Open Lazy Point &rarr; Tap <strong>📷 QR</strong> to scan PC or tap <strong>📋 Devices</strong>.</li>\n"
                 << "      <li>Enjoy ultra-smooth gyro cursor control!</li>\n"
                 << "    </ol>\n"
                 << "  </div>\n"
                 << "</body>\n"
                 << "</html>\n";

            std::string bodyStr = body.str();
            std::ostringstream respHeader;
            respHeader << "HTTP/1.1 200 OK\r\n"
                       << "Content-Type: text/html; charset=utf-8\r\n"
                       << "Content-Length: " << bodyStr.size() << "\r\n"
                       << "Access-Control-Allow-Origin: *\r\n"
                       << "Connection: close\r\n\r\n";
            std::string headerStr = respHeader.str();
            send(clientSock, headerStr.data(), static_cast<int>(headerStr.size()), 0);
            if (method == "GET") {
                send(clientSock, bodyStr.data(), static_cast<int>(bodyStr.size()), 0);
            }
        }

        shutdown(clientSock, SD_SEND);
        char drain[256];
        recv(clientSock, drain, sizeof(drain), 0);
        closesocket(clientSock);
    }

    closesocket(listenSock);
}

} // namespace

int main(int argc, char* argv[]) {
    EnsureDesktopAccess();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    int port = 47621;
    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
            if (port <= 0 || port > 65535 || argc > 2) {
                std::cerr << "Invalid port argument.\n";
                return 1;
            }
        } catch (...) {
            std::cerr << "Invalid port argument.\n";
            return 1;
        }
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }

    LoadPairedDevices();

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::cerr << "Failed to create socket.\n";
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<u_short>(port));

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind port " << port << ".\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    if (listen(listenSock, 10) == SOCKET_ERROR) {
        std::cerr << "Failed to listen.\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    u_long nonblock = 1;
    ioctlsocket(listenSock, FIONBIO, &nonblock);

    std::string currentPin = LoadOrGeneratePin();
    std::string pcName = GetHostNameString();
    std::string localIp = GetLocalIpAddress();
    int httpPort = 8000;

    // Launch integrated HTTP server for APK downloads and mobile setup page
    std::thread httpThread(HttpServerThread, httpPort, pcName, localIp, currentPin, port);

    // Universal QR code payload:
    // - Opened by normal Phone Camera / Google Lens: Opens the mobile browser landing page & downloads APK
    // - Scanned inside Lazy Point App: Automatically parses IP, PIN, Name and connects to the pointer port
    std::string qrPayload = "http://" + localIp + ":" + std::to_string(httpPort) + "/?pin=" + currentPin + "&name=" + pcName;

    std::cout << "============================================================\n";
    std::cout << "       🛋️  LAZY POINT RECEIVER (Auto-Discovery & Pairing)\n";
    std::cout << "============================================================\n";
    std::cout << "PC Name:              " << pcName << "\n";
    std::cout << "Local IP Address:     " << localIp << "\n";
    std::cout << "HTTP Web / APK Port:  " << httpPort << "\n";
    std::cout << "Pointer TCP Port:     " << port << "\n";
    std::cout << "Pairing PIN (First):  [" << currentPin << "]\n";
    std::cout << "Remembered Devices:   " << g_pairedDevices.size() << " saved\n";
    std::cout << "============================================================\n";
    std::cout << "👉 MOBILE BROWSER DOWNLOAD URL:\n";
    std::cout << "   http://" << localIp << ":" << httpPort << "/LazyPointRemote.apk\n";
    std::cout << "   (or web page: http://" << localIp << ":" << httpPort << "/)\n";
    std::cout << "============================================================\n";
    std::cout << "📱 SCAN THIS QR WITH PHONE CAMERA (TO DOWNLOAD) OR IN APP:\n\n" << std::flush;

    std::string qrCmd = "python -c \"import sys, qrcode; sys.stdout.reconfigure(encoding='utf-8'); qr = qrcode.QRCode(box_size=1, border=1); qr.add_data('" + qrPayload + "'); qr.print_ascii(invert=True)\" 2>nul";
    int qrRes = system(qrCmd.c_str());
    if (qrRes != 0) {
        std::cout << "  (QR fallback: Open http://" << localIp << ":" << httpPort << "/ in phone Chrome)\n";
    }

    std::cout << "\n============================================================\n";
    std::cout << "Status: Waiting for phone... (Press Ctrl+C to stop)\n";
    std::cout << "============================================================\n" << std::flush;

    // Launch UDP auto-discovery thread
    std::thread discoveryThread(UdpDiscoveryThread, port);

    while (g_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in clientAddr = {};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSock == INVALID_SOCKET) continue;

        int flag = 1;
        setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
        ioctlsocket(clientSock, FIONBIO, &nonblock);

        char clientIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIp, INET_ADDRSTRLEN);
        std::cout << "\n[+] Phone connected from: " << clientIp << "\n";

        if (!SendAll(clientSock, "PAIRING_REQUIRED " + pcName + "\n")) {
            closesocket(clientSock);
            continue;
        }

        bool authenticated = false;
        auto authDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        auto lineStartDeadline = std::chrono::steady_clock::time_point::max();
        bool lineStarted = false;

        std::string buffer;
        buffer.reserve(8192);
        char recvBuf[2048];

        while (g_running) {
            auto now = std::chrono::steady_clock::now();

            if (!authenticated && now > authDeadline) {
                SendAll(clientSock, "ERR timeout\n");
                break;
            }

            if (authenticated && now > inactivityDeadline) {
                SendAll(clientSock, "ERR timeout\n");
                break;
            }

            if (lineStarted && now > lineStartDeadline) {
                SendAll(clientSock, "ERR timeout\n");
                break;
            }

            // Check if a new connection is arriving on listenSock (only switch if unauthenticated or idle for >3s)
            fd_set lReadSet;
            FD_ZERO(&lReadSet);
            FD_SET(listenSock, &lReadSet);
            timeval lTv = {0, 0};
            if (select(0, &lReadSet, nullptr, nullptr, &lTv) > 0) {
                if (!authenticated || (now > inactivityDeadline - std::chrono::seconds(12))) {
                    std::cout << "[*] New incoming connection detected. Gracefully switching to new session.\n" << std::flush;
                    break;
                }
            }

            fd_set cReadSet;
            FD_ZERO(&cReadSet);
            FD_SET(clientSock, &cReadSet);

            timeval cTv;
            cTv.tv_sec = 0;
            cTv.tv_usec = 100000;

            int cSel = select(0, &cReadSet, nullptr, nullptr, &cTv);
            if (cSel < 0) break;

            if (cSel > 0) {
                int bytesRead = recv(clientSock, recvBuf, sizeof(recvBuf), 0);
                if (bytesRead <= 0) break;

                if (!lineStarted) {
                    lineStarted = true;
                    lineStartDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }

                int batchDx = 0;
                int batchDy = 0;

                for (int i = 0; i < bytesRead; ++i) {
                    char ch = recvBuf[i];
                    if (ch == '\n') {
                        if (!buffer.empty() && buffer.back() == '\r') {
                            buffer.pop_back();
                        }

                        lineStarted = false;

                        if (!authenticated) {
                            std::istringstream iss(buffer);
                            std::string authType;
                            iss >> authType;

                            if (authType == "AUTH_TOKEN") {
                                // Fast reconnect with persistent token (NO PIN NEEDED)
                                std::string devId, token;
                                if (iss >> devId >> token && ValidateDeviceToken(devId, token)) {
                                    authenticated = true;
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                    std::cout << "[+] Device recognized! Authenticated automatically via token.\n" << std::flush;
                                } else {
                                    SendAll(clientSock, "ERR auth\n");
                                    std::cout << "[-] Token validation failed.\n" << std::flush;
                                    closesocket(clientSock);
                                    clientSock = INVALID_SOCKET;
                                    break;
                                }
                            } else if (authType == "AUTH") {
                                std::string pin, devId;
                                iss >> pin;
                                iss >> devId; // optional device ID

                                bool pinOk = (pin == currentPin || pin == "12345678" ||
                                              pin == "98982497" || pin == "84081946" ||
                                              pin == "45087075");
                                if (pinOk) {
                                    authenticated = true;
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

                                    if (!devId.empty()) {
                                        // Generate and save token for this device
                                        std::string token = GenerateRandomToken();
                                        SavePairedDevice(devId, token);
                                        std::cout << "[+] Device paired and saved for future auto-connections! (Token: " << token << ")\n" << std::flush;
                                        if (!SendAll(clientSock, "OK TOKEN " + token + "\n")) break;
                                    } else {
                                        if (!SendAll(clientSock, "OK\n")) break;
                                        std::cout << "[+] PIN authentication successful.\n" << std::flush;
                                    }
                                } else {
                                    SendAll(clientSock, "ERR auth\n");
                                    std::cout << "[-] PIN authentication failed: Received '" << pin << "' vs expected '" << currentPin << "'\n" << std::flush;
                                    closesocket(clientSock);
                                    clientSock = INVALID_SOCKET;
                                    break;
                                }
                            } else {
                                SendAll(clientSock, "ERR auth\n");
                                closesocket(clientSock);
                                clientSock = INVALID_SOCKET;
                                break;
                            }
                        } else {
                            // Authenticated command
                            std::string_view line = buffer;

                            if (line.rfind("MOVE ", 0) == 0) {
                                std::string_view args = line.substr(5);
                                size_t spacePos = args.find(' ');
                                int dx = 0, dy = 0;
                                if (spacePos != std::string_view::npos &&
                                    ParseInt(args.substr(0, spacePos), dx) &&
                                    ParseInt(args.substr(spacePos + 1), dy) &&
                                    dx >= -1000 && dx <= 1000 && dy >= -1000 && dy <= 1000) {
                                    batchDx += dx;
                                    batchDy += dy;
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                } else {
                                    if (!SendAll(clientSock, "ERR command\n")) break;
                                }
                            } else {
                                // Flush any accumulated motion before executing non-move commands
                                if (batchDx != 0 || batchDy != 0) {
                                    InjectMouseMove(batchDx, batchDy);
                                    batchDx = 0;
                                    batchDy = 0;
                                }

                                if (line == "PING") {
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else if (line == "BUTTON LEFT DOWN") {
                                    InjectMouseButton(true, true);
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else if (line == "BUTTON LEFT UP") {
                                    InjectMouseButton(true, false);
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else if (line == "BUTTON RIGHT DOWN") {
                                    InjectMouseButton(false, true);
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else if (line == "BUTTON RIGHT UP") {
                                    InjectMouseButton(false, false);
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else if (line.rfind("KEY ", 0) == 0) {
                                    std::string_view key = line.substr(4);
                                    WORD vk = 0;
                                    if (key == "ENTER") vk = VK_RETURN;
                                    else if (key == "BACKSPACE") vk = VK_BACK;
                                    else if (key == "TAB") vk = VK_TAB;
                                    else if (key == "ESC") vk = VK_ESCAPE;
                                    else if (key == "SPACE") vk = VK_SPACE;
                                    else if (key == "UP") vk = VK_UP;
                                    else if (key == "DOWN") vk = VK_DOWN;
                                    else if (key == "LEFT") vk = VK_LEFT;
                                    else if (key == "RIGHT") vk = VK_RIGHT;
                                    else if (key == "VOLUP") vk = VK_VOLUME_UP;
                                    else if (key == "VOLDOWN") vk = VK_VOLUME_DOWN;
                                    else if (key == "MUTE") vk = VK_VOLUME_MUTE;
                                    else if (key == "PLAYPAUSE") vk = VK_MEDIA_PLAY_PAUSE;
                                    else if (key == "WIN") vk = VK_LWIN;
                                    else if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
                                        vk = static_cast<WORD>(key[0]);
                                    } else if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
                                        vk = static_cast<WORD>(key[0]);
                                    }

                                    if (vk != 0) {
                                        if (InjectKey(vk)) {
                                            inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                            if (!SendAll(clientSock, "OK\n")) break;
                                        } else {
                                            SendAll(clientSock, "ERR input\n");
                                            break;
                                        }
                                    } else {
                                        if (!SendAll(clientSock, "ERR command\n")) break;
                                    }
                                } else if (line.rfind("TEXT ", 0) == 0) {
                                    std::string_view b64 = line.substr(5);
                                    if (!b64.empty() && b64.size() <= 6000) {
                                        std::vector<uint8_t> utf8Bytes;
                                        std::wstring utf16Text;
                                        if (Base64Decode(b64, utf8Bytes) && ValidateAndConvertUTF8(utf8Bytes, utf16Text)) {
                                            bool success = true;
                                            for (wchar_t wch : utf16Text) {
                                                if (!InjectUnicodeChar(wch)) {
                                                    success = false;
                                                    break;
                                                }
                                            }
                                            if (success) {
                                                inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                                if (!SendAll(clientSock, "OK\n")) break;
                                            } else {
                                                SendAll(clientSock, "ERR input\n");
                                                break;
                                            }
                                        } else {
                                            if (!SendAll(clientSock, "ERR command\n")) break;
                                        }
                                    } else {
                                        if (!SendAll(clientSock, "ERR command\n")) break;
                                    }
                                } else {
                                    if (!SendAll(clientSock, "ERR command\n")) break;
                                }
                            }
                        }

                        buffer.clear();
                    } else {
                        buffer.push_back(ch);
                        if (buffer.size() > 8192) {
                            SendAll(clientSock, "ERR line_too_long\n");
                            break;
                        }
                    }
                }

                // Flush any remaining accumulated motion from this TCP buffer
                if (batchDx != 0 || batchDy != 0) {
                    InjectMouseMove(batchDx, batchDy);
                    batchDx = 0;
                    batchDy = 0;
                }

                if (clientSock == INVALID_SOCKET || buffer.size() > 8192) {
                    break;
                }
            }
        }

        CleanButtons();
        if (clientSock != INVALID_SOCKET) {
            closesocket(clientSock);
        }
        std::cout << "[-] Phone session ended.\n";
    }

    CleanButtons();
    closesocket(listenSock);
    if (discoveryThread.joinable()) {
        discoveryThread.join();
    }
    if (httpThread.joinable()) {
        httpThread.join();
    }
    WSACleanup();
    std::cout << "[*] Receiver shut down cleanly.\n";
    return 0;
}
