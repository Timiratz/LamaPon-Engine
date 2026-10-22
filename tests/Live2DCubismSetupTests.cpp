#include "LamaPon/Editor/Live2DCubismSetup.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // 条件不成立を検査失敗にします(condition: 成立条件, message: 失敗理由)。
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // 出力先フォルダーを作成し、検査ファイルを書き込みます(path: 出力先, contents: 書込内容)。
    void WriteFile(const fs::path& path, const std::string& contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        Require(static_cast<bool>(output), "test file must be writable");
        output << contents;
        Require(static_cast<bool>(output), "test file must be saved");
    }

    // 64-bit Windows DLLのPEヘッダー先頭部分を作ります(machine: PE machine code)。
    std::string MakePeHeader(const std::uint16_t machine)
    {
        std::array<unsigned char, 128> bytes{};
        constexpr std::uint16_t dosMagic = 0x5a4d;
        constexpr std::uint32_t peOffset = 64;
        constexpr std::uint32_t peSignature = 0x00004550;
        std::memcpy(bytes.data(), &dosMagic, sizeof(dosMagic));
        std::memcpy(bytes.data() + 0x3c, &peOffset, sizeof(peOffset));
        std::memcpy(
            bytes.data() + peOffset,
            &peSignature,
            sizeof(peSignature));
        std::memcpy(
            bytes.data() + peOffset + sizeof(peSignature),
            &machine,
            sizeof(machine));
        return std::string(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size());
    }

    // Cubism Core SDKに必要な最小テストファイルを作ります(root: SDK展開先, machine: Core DLLのPE machine code)。
    void CreateSdkFixture(
        const fs::path& root,
        const std::uint16_t machine = IMAGE_FILE_MACHINE_AMD64)
    {
        WriteFile(
            root / "Core/include/Live2DCubismCore.h",
            "void csmGetRenderOrders(void);\n"
            "void csmGetOffscreenCount(void);\n");
        WriteFile(
            root / "Core/lib/windows/x86_64/Live2DCubismCore_MD.lib",
            "fixture import library");
        WriteFile(
            root / "Core/dll/windows/x86_64/Live2DCubismCore.dll",
            MakePeHeader(machine));
    }

    // Cubism SDKの検査、プロジェクト導入、native設定更新を確認します(root: テストルート)。
    void TestSdkInspectionAndInstall(const fs::path& root)
    {
        const fs::path sdkRoot = root / "sdk-source";
        const fs::path packageRoot = root / "assets/packages/live2d-cubism";
        CreateSdkFixture(sdkRoot);
        WriteFile(
            packageRoot / "package.json",
            "{\"name\":\"live2d-cubism\",\"native\":{"
            "\"defines\":[\"CUSTOM_DEFINE\"]}}\n");

        const auto inspection = LamaPon::InspectLive2DCubismSdk(sdkRoot);
        Require(inspection.IsValid(), "Cubism 5.3 x64 SDK must be accepted");
        Require(inspection.usesCubism53, "Cubism 5.3 API must be detected");
        LamaPon::InstallLive2DCubismSdk(inspection, packageRoot);
        LamaPon::InstallLive2DCubismSdk(inspection, packageRoot);

        Require(
            fs::is_regular_file(
                packageRoot / "sdk/Core/include/Live2DCubismCore.h"),
            "Core header must be copied into the project package");
        Require(
            fs::is_regular_file(
                packageRoot
                    / "sdk/Core/lib/windows/x86_64/Live2DCubismCore_MD.lib"),
            "MSVC import library must be copied into the project package");
        Require(
            fs::is_regular_file(
                packageRoot
                    / "sdk/Core/dll/windows/x86_64/Live2DCubismCore.dll"),
            "Core runtime DLL must be copied into the project package");

        std::ifstream manifestStream(packageRoot / "package.json");
        const auto manifest = nlohmann::json::parse(manifestStream);
        const auto& native = manifest.at("native");
        Require(
            native.at("defines").size() == 3,
            "native configuration must preserve and uniquely add defines");
        Require(
            std::ranges::find(
                native.at("defines"),
                "LAMAPON_LIVE2D_CUBISM_ENABLED")
                != native.at("defines").end(),
            "Live2D Core compile define must be added");
        Require(
            std::ranges::find(
                native.at("defines"),
                "LAMAPON_LIVE2D_CUBISM_53")
                != native.at("defines").end(),
            "Cubism 5.3 compile define must be added");
        Require(
            std::ranges::find(
                native.at("defines"),
                "CUSTOM_DEFINE")
                != native.at("defines").end(),
            "pre-existing package defines must be preserved");

        const fs::path incompatibleRoot = root / "sdk-x86";
        CreateSdkFixture(
            incompatibleRoot,
            IMAGE_FILE_MACHINE_I386);
        const auto incompatible =
            LamaPon::InspectLive2DCubismSdk(incompatibleRoot);
        Require(
            !incompatible.IsValid()
                && incompatible.error.find("x64") != std::string::npos,
            "32-bit Core DLL must report an actionable architecture error");

        const fs::path legacyRoot = root / "sdk-legacy";
        WriteFile(
            legacyRoot / "Core/include/Live2DCubismCore.h",
            "void csmGetDrawableRenderOrders(void);\n");
        WriteFile(
            legacyRoot
                / "Core/lib/windows/x86_64/Live2DCubismCore_MD.lib",
            "fixture import library");
        WriteFile(
            legacyRoot
                / "Core/dll/windows/x86_64/Live2DCubismCore.dll",
            MakePeHeader(IMAGE_FILE_MACHINE_AMD64));
        const auto legacy = LamaPon::InspectLive2DCubismSdk(legacyRoot);
        Require(
            legacy.IsValid() && !legacy.usesCubism53,
            "supported legacy Core render-order API must be detected");
    }

    // モデル参照検査と素材コピー、同名モデルの保護を確認します(root: テストルート)。
    void TestModelImport(const fs::path& root)
    {
        const fs::path source = root / "source/Haruka";
        const fs::path assetRoot = root / "project/assets";
        fs::create_directories(assetRoot);
        WriteFile(
            source / "Haruka.model3.json",
            R"({"FileReferences":{"Moc":"Haruka.moc3","Textures":["texture/texture_00.png"]}})");
        WriteFile(source / "Haruka.moc3", "fixture moc");
        WriteFile(source / "texture/texture_00.png", "fixture texture");
        WriteFile(source / "motions/Idle.motion3.json", "fixture motion");

        const auto imported = LamaPon::ImportLive2DModel(
            source / "Haruka.model3.json",
            assetRoot);
        Require(
            imported == fs::path("Live2D/Haruka/Haruka.model3.json"),
            "model3 JSON must receive a project-relative assets path");
        Require(
            fs::is_regular_file(assetRoot / imported),
            "model3 JSON must be copied into the project assets");
        Require(
            fs::is_regular_file(
                assetRoot / "Live2D/Haruka/texture/texture_00.png"),
            "model textures must retain their relative folder layout");
        Require(
            fs::is_regular_file(
                assetRoot / "Live2D/Haruka/motions/Idle.motion3.json"),
            "the selected model folder must be copied as a whole");

        const auto importedAgain = LamaPon::ImportLive2DModel(
            source / "Haruka.model3.json",
            assetRoot);
        Require(
            importedAgain
                == fs::path("Live2D/Haruka-2/Haruka.model3.json"),
            "importing the same model again must not overwrite its files");

        const fs::path missingTexture = root / "source/Missing";
        WriteFile(
            missingTexture / "Missing.model3.json",
            R"({"FileReferences":{"Moc":"Missing.moc3","Textures":["missing.png"]}})");
        WriteFile(missingTexture / "Missing.moc3", "fixture moc");
        bool missingRejected = false;
        try
        {
            static_cast<void>(LamaPon::ImportLive2DModel(
                missingTexture / "Missing.model3.json",
                assetRoot));
        }
        catch (const std::runtime_error&)
        {
            missingRejected = true;
        }
        Require(
            missingRejected,
            "missing model textures must be reported before copying");

        const fs::path escaping = root / "source/Escaping";
        WriteFile(
            escaping / "Escaping.model3.json",
            R"({"FileReferences":{"Moc":"../outside.moc3","Textures":["texture.png"]}})");
        WriteFile(root / "source/outside.moc3", "outside moc");
        WriteFile(escaping / "texture.png", "fixture texture");
        bool escapingRejected = false;
        try
        {
            static_cast<void>(LamaPon::ImportLive2DModel(
                escaping / "Escaping.model3.json",
                assetRoot));
        }
        catch (const std::runtime_error&)
        {
            escapingRejected = true;
        }
        Require(
            escapingRejected,
            "model files outside the selected folder must be rejected");
    }
}

int main(const int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "A test output directory is required.\n";
        return 2;
    }

    const fs::path root = fs::absolute(argv[1]);
    try
    {
        std::error_code error;
        fs::remove_all(root, error);
        if (error)
        {
            throw std::runtime_error(
                "Could not reset the approved test output directory.");
        }
        fs::create_directories(root);
        TestSdkInspectionAndInstall(root);
        TestModelImport(root);
        fs::remove_all(root, error);
        if (error)
        {
            throw std::runtime_error(
                "Could not clean the test output directory.");
        }
        std::cout << "Live2D Cubism setup checks passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
