#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

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

namespace {

volatile bool g_running = true;
bool g_leftButtonDown = false;
bool g_rightButtonDown = false;

// Persistent paired devices: deviceId -> token
std::map<std::string, std::string> g_pairedDevices;
std::mutex g_pairedMutex;
const std::string PAIRED_FILE = "paired_devices.txt";

void LoadPairedDevices() {
    std::lock_guard<std::mutex> lock(g_pairedMutex);
    g_pairedDevices.clear();
    std::ifstream f(PAIRED_FILE);
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

    std::ofstream f(PAIRED_FILE, std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& [id, tok] : g_pairedDevices) {
        f << id << " " << tok << "\n";
    }
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

std::string GeneratePin() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 99999999);
    int num = dis(gen);
    std::ostringstream ss;
    ss << std::setw(8) << std::setfill('0') << num;
    return ss.str();
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

bool InjectMouseMove(int dx, int dy) {
    INPUT inp = {};
    inp.type = INPUT_MOUSE;
    inp.mi.dx = dx;
    inp.mi.dy = dy;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    UINT result = SendInput(1, &inp, sizeof(INPUT));
    return result == 1;
}

bool InjectMouseButton(bool left, bool down) {
    INPUT inp = {};
    inp.type = INPUT_MOUSE;
    if (left) {
        inp.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        g_leftButtonDown = down;
    } else {
        inp.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        g_rightButtonDown = down;
    }
    UINT result = SendInput(1, &inp, sizeof(INPUT));
    return result == 1;
}

bool InjectKey(WORD vk) {
    INPUT inps[2] = {};
    inps[0].type = INPUT_KEYBOARD;
    inps[0].ki.wVk = vk;
    inps[0].ki.dwFlags = 0;

    inps[1].type = INPUT_KEYBOARD;
    inps[1].ki.wVk = vk;
    inps[1].ki.dwFlags = KEYEVENTF_KEYUP;

    UINT result = SendInput(2, inps, sizeof(INPUT));
    return result == 2;
}

bool InjectUnicodeChar(wchar_t ch) {
    INPUT inps[2] = {};
    inps[0].type = INPUT_KEYBOARD;
    inps[0].ki.wVk = 0;
    inps[0].ki.wScan = ch;
    inps[0].ki.dwFlags = KEYEVENTF_UNICODE;

    inps[1].type = INPUT_KEYBOARD;
    inps[1].ki.wVk = 0;
    inps[1].ki.wScan = ch;
    inps[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    UINT result = SendInput(2, inps, sizeof(INPUT));
    return result == 2;
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

} // namespace

int main(int argc, char* argv[]) {
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

    if (listen(listenSock, 1) == SOCKET_ERROR) {
        std::cerr << "Failed to listen.\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    u_long nonblock = 1;
    ioctlsocket(listenSock, FIONBIO, &nonblock);

    std::string currentPin = GeneratePin();
    std::string pcName = GetHostNameString();

    std::cout << "============================================================\n";
    std::cout << "       🛋️ LAZY POINT RECEIVER (Auto-Discovery & Pairing)\n";
    std::cout << "============================================================\n";
    std::cout << "PC Name:              " << pcName << "\n";
    std::cout << "TCP Port:             " << port << "\n";
    std::cout << "UDP Auto-Discovery:   Active on port 47622\n";
    std::cout << "Pairing PIN (First):  [" << currentPin << "]\n";
    std::cout << "Remembered Devices:   " << g_pairedDevices.size() << " saved\n";
    std::cout << "Status:               Waiting for phone... (Press Ctrl+C to stop)\n";
    std::cout << "============================================================\n";

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

        if (!SendAll(clientSock, "PAIRING_REQUIRED\n")) {
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
                                    std::cout << "[+] Device recognized! Authenticated automatically via token.\n";
                                } else {
                                    SendAll(clientSock, "ERR auth\n");
                                    std::cout << "[-] Token validation failed.\n";
                                    closesocket(clientSock);
                                    clientSock = INVALID_SOCKET;
                                    break;
                                }
                            } else if (authType == "AUTH") {
                                std::string pin, devId;
                                iss >> pin;
                                iss >> devId; // optional device ID

                                if (pin == currentPin) {
                                    authenticated = true;
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

                                    if (!devId.empty()) {
                                        // Generate and save token for this device
                                        std::string token = GenerateRandomToken();
                                        SavePairedDevice(devId, token);
                                        std::cout << "[+] Device paired and saved for future auto-connections!\n";
                                        if (!SendAll(clientSock, "OK TOKEN " + token + "\n")) break;
                                    } else {
                                        if (!SendAll(clientSock, "OK\n")) break;
                                        std::cout << "[+] PIN authentication successful.\n";
                                    }
                                } else {
                                    SendAll(clientSock, "ERR auth\n");
                                    std::cout << "[-] PIN authentication failed.\n";
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

                            if (line == "PING") {
                                inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                if (!SendAll(clientSock, "OK\n")) break;
                            } else if (line.rfind("MOVE ", 0) == 0) {
                                std::string_view args = line.substr(5);
                                size_t spacePos = args.find(' ');
                                int dx = 0, dy = 0;
                                if (spacePos != std::string_view::npos &&
                                    ParseInt(args.substr(0, spacePos), dx) &&
                                    ParseInt(args.substr(spacePos + 1), dy) &&
                                    dx >= -500 && dx <= 500 && dy >= -500 && dy <= 500) {
                                    if (InjectMouseMove(dx, dy)) {
                                        inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                        if (!SendAll(clientSock, "OK\n")) break;
                                    } else {
                                        SendAll(clientSock, "ERR input\n");
                                        break;
                                    }
                                } else {
                                    if (!SendAll(clientSock, "ERR command\n")) break;
                                }
                            } else if (line == "BUTTON LEFT DOWN") {
                                if (InjectMouseButton(true, true)) {
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else {
                                    SendAll(clientSock, "ERR input\n");
                                    break;
                                }
                            } else if (line == "BUTTON LEFT UP") {
                                if (InjectMouseButton(true, false)) {
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else {
                                    SendAll(clientSock, "ERR input\n");
                                    break;
                                }
                            } else if (line == "BUTTON RIGHT DOWN") {
                                if (InjectMouseButton(false, true)) {
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else {
                                    SendAll(clientSock, "ERR input\n");
                                    break;
                                }
                            } else if (line == "BUTTON RIGHT UP") {
                                if (InjectMouseButton(false, false)) {
                                    inactivityDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                    if (!SendAll(clientSock, "OK\n")) break;
                                } else {
                                    SendAll(clientSock, "ERR input\n");
                                    break;
                                }
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
                                else if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
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

                        buffer.clear();
                    } else {
                        buffer.push_back(ch);
                        if (buffer.size() > 8192) {
                            SendAll(clientSock, "ERR line_too_long\n");
                            break;
                        }
                    }
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
    WSACleanup();
    std::cout << "[*] Receiver shut down cleanly.\n";
    return 0;
}
