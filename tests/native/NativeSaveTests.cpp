#include "LamaPon/Native/NativeBridge.h"
#include "LamaPon/Native/NativeServices.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
    void Require(bool value, const char* message)
    { if (!value) throw std::runtime_error(message); }
    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Require(bool(input), "Cannot read test save");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    std::string Load(const char* key)
    {
        char* value = LamaPon::Native::LoadPortableText(key);
        Require(value != nullptr, "Expected saved value");
        std::string result(value);
        std::free(value);
        return result;
    }
}

// The caller supplies an existing empty directory whose creation is authorized.
int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2, "Expected an authorized empty save directory");
        const auto root = std::filesystem::canonical(argv[1]);
        Require(std::filesystem::is_directory(root) && std::filesystem::is_empty(root), "Save directory is not empty");
        LamaPon::Native::ConfigureServices(nullptr, root, root);
        Require(LamaPon::Native::LoadPortableText("missing") == nullptr, "Missing key was not absent");
        Require(LamaPon::Native::SavePortableText("text", "保存テスト"), "Initial save failed");
        Require(LamaPon::Native::SavePortableText("empty", ""), "Empty save failed");
        Require(Load("text") == "保存テスト" && Load("empty").empty(), "Saved values changed");
        const auto target = root / "values.json";
        const auto original = Read(target);
        // Blocking the pending file exercises a real write failure.
        std::filesystem::create_directory(root / "values.pending");
        Require(!LamaPon::Native::SavePortableText("text", "lost"), "Blocked save reported success");
        Require(Read(target) == original && Load("text") == "保存テスト", "Failed write changed committed data");
        std::filesystem::rename(root / "values.pending", root / "blocked-pending");
        Require(LamaPon::Native::SavePortableText("text", "再保存"), "Retry failed");
        LamaPon::Native::ConfigureServices(nullptr, root, root);
        Require(Load("text") == "再保存" && Load("empty").empty(), "Reload changed saved values");
        const auto valid = Read(target);
        { std::ofstream output(target, std::ios::binary | std::ios::trunc); output << "not valid JSON"; }
        bool rejected = false;
        try { LamaPon::Native::ConfigureServices(nullptr, root, root); }
        catch (const std::runtime_error&) { rejected = true; }
        Require(rejected, "Corrupt document was accepted");
        Require(!LamaPon::Native::SavePortableText("text", "overwrite"), "Save after failed load was accepted");
        Require(Read(target) == "not valid JSON", "Failed load overwrote existing data");
        { std::ofstream output(target, std::ios::binary | std::ios::trunc); output << valid; }
        LamaPon::Native::ConfigureServices(nullptr, root, root);
        Require(Load("text") == "再保存", "Explicit repair did not restore saves");
        std::cout << "Native save empty-value, failure preservation, retry and failed-load protection passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
