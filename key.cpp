// revkey.cpp - REvil Decryptor (Automatic File Restorer)
#include <cryptopp/cryptlib.h>
#include <cryptopp/aes.h>
#include <cryptopp/rsa.h>
#include <cryptopp/osrng.h>
#include <cryptopp/modes.h>
#include <cryptopp/filters.h>
#include <cryptopp/files.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <cstring>

using namespace CryptoPP;
namespace fs = std::filesystem;

// Konfigurasi
const std::string PRIVATE_KEY_FILE = "rsa_private.der";
const std::string AES_KEY_ENC_FILE = "aes_key.enc";
const std::string ENCRYPTED_SUFFIX = ".revil";

// ====================== HELPER: DETEKSI SEMUA DRIVE ======================

std::vector<std::string> get_all_roots() {
    std::vector<std::string> roots;
#ifdef _WIN32
    char drives[256];
    DWORD len = GetLogicalDriveStringsA(sizeof(drives), drives);
    for (DWORD i = 0; i < len; i += strlen(&drives[i]) + 1) {
        std::string drive = &drives[i];
        if (drive != "A:\\" && drive != "B:\\") {
            roots.push_back(drive);
        }
    }
#else
    roots.push_back("/");
#endif
    return roots;
}

bool ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ====================== CORE: PROSES DEKRIPSI ======================

void decrypt_file(const std::string& filepath, const SecByteBlock& key) {
    try {
        std::ifstream infile(filepath, std::ios::binary);
        if (!infile) return;

        // Ambil IV dari 16 byte pertama file
        byte iv[AES::BLOCKSIZE];
        infile.read(reinterpret_cast<char*>(iv), AES::BLOCKSIZE);

        // Baca sisa konten terenkripsi
        std::string cipher_text((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
        infile.close();

        if (cipher_text.empty()) return;

        std::string plain_text;
        CBC_Mode<AES>::Decryption d;
        d.SetKeyWithIV(key, key.size(), iv);

        StringSource ss(cipher_text, true, 
            new StreamTransformationFilter(d, 
                new StringSink(plain_text)
            )
        );

        // Nama file asli tanpa .revil
        std::string original_path = filepath.substr(0, filepath.size() - ENCRYPTED_SUFFIX.size());
        
        std::ofstream outfile(original_path, std::ios::binary);
        outfile.write(plain_text.data(), plain_text.size());
        outfile.close();

        // Hapus file terenkripsi jika berhasil
        fs::remove(filepath);
        std::cout << "[✓] Pulihkan: " << original_path << std::endl;

    } catch (const std::exception& e) {
        // Abaikan error (file sedang digunakan, permission, dll)
    }
}

void scan_and_decrypt(const std::string& path, const SecByteBlock& key) {
    auto iter_opt = fs::directory_options::skip_permission_denied;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(path, iter_opt)) {
            try {
                if (fs::is_regular_file(entry) && ends_with(entry.path().string(), ENCRYPTED_SUFFIX)) {
                    decrypt_file(entry.path().string(), key);
                }
            } catch (...) {}
        }
    } catch (...) {}
}

// ====================== LOAD & DECRYPT KUNCI AES ======================

SecByteBlock load_and_decrypt_aes_key() {
    // 1. Load RSA Private Key
    RSA::PrivateKey privateKey;
    FileSource fs_key(PRIVATE_KEY_FILE.c_str(), true);
    privateKey.Load(fs_key);

    // 2. Load encrypted AES key file
    std::ifstream f_enc(AES_KEY_ENC_FILE, std::ios::binary | std::ios::ate);
    if (!f_enc) {
        throw std::runtime_error("File aes_key.enc tidak ditemukan!");
    }
    
    std::streamsize sz = f_enc.tellg();
    std::string enc_aes_key(sz, '\0');
    f_enc.seekg(0);
    f_enc.read(&enc_aes_key[0], sz);
    f_enc.close();

    // 3. Dekripsi AES key menggunakan RSA
    AutoSeededRandomPool prng;
    RSAES_OAEP_SHA_Decryptor d(privateKey);
    
    SecByteBlock decrypted_aes_key(32); 

    try {
        d.Decrypt(prng, 
            reinterpret_cast<const byte*>(enc_aes_key.data()), 
            enc_aes_key.size(), 
            decrypted_aes_key.data());
    } catch (const CryptoPP::Exception& e) {
        throw std::runtime_error("Gagal dekripsi kunci AES: Kunci RSA tidak cocok.");
    }
    
    return decrypted_aes_key;
}

// ====================== MAIN ENTRY POINT ======================

int main() {
    std::cout << "========================================\n";
    std::cout << "   AUTOMATIC REvil FILE RESTORER        \n";
    std::cout << "========================================\n\n";

    try {
        std::cout << "[*] Tahap 1: Memulihkan Kunci Utama...\n";
        SecByteBlock aes_key = load_and_decrypt_aes_key();
        std::cout << "[✓] Kunci AES Berhasil Didapatkan.\n\n";

        std::cout << "[*] Tahap 2: Mencari Drive Sistem...\n";
        std::vector<std::string> drives = get_all_roots();

        for (const std::string& drive : drives) {
            std::cout << "[>>>] Memindai dan Mendekripsi: " << drive << std::endl;
            scan_and_decrypt(drive, aes_key);
        }

        // Wipe key from memory untuk keamanan
        memset(aes_key.data(), 0, aes_key.size());
        
        std::cout << "\n[!] PROSES SELESAI. Seluruh drive telah diproses.\n";

    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL ERROR] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
