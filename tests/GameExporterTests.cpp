#include "LamaPon/Assets/AssetArchive.h"
#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Editor/ExeIconTool.h"
#include "LamaPon/Editor/GameExporter.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Resources/WindowsResource.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // このテスト実行ファイル自身のパス（本物のPEとして使います）。
    std::filesystem::path SelfExecutablePath()
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer,
            MAX_PATH);
        Require(
            length > 0 && length < MAX_PATH,
            "Could not resolve the test executable path.");
        return std::filesystem::path(buffer);
    }

    // 実行ファイルへ埋め込まれたアイコングループを検証します。
    void RequireEmbeddedIcon(
        const std::filesystem::path& executablePath,
        const std::uint16_t expectedImageCount)
    {
        const HMODULE module = LoadLibraryExW(
            executablePath.c_str(),
            nullptr,
            LOAD_LIBRARY_AS_DATAFILE
                | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Require(
            module != nullptr,
            "Exported executable could not be inspected.");
        const HRSRC group = FindResourceW(
            module,
            MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE),
            RT_GROUP_ICON);
        std::uint16_t imageCount = 0;
        if (group != nullptr)
        {
            const HGLOBAL loaded =
                LoadResource(module, group);
            const auto* data = loaded != nullptr
                ? static_cast<const unsigned char*>(
                    LockResource(loaded))
                : nullptr;
            if (data != nullptr
                && SizeofResource(module, group) >= 6)
            {
                std::memcpy(
                    &imageCount,
                    data + 4,
                    sizeof(imageCount));
            }
        }
        const bool firstIconPresent = FindResourceW(
            module,
            MAKEINTRESOURCEW(1),
            RT_ICON) != nullptr;
        FreeLibrary(module);
        Require(
            group != nullptr
                && imageCount == expectedImageCount
                && firstIconPresent,
            "Icon group was not embedded correctly.");
    }

    // 事前コンパイル済みシェーダーを除いた同梱ファイル数。
    // shader-cacheの中身は入口の一覧が増えれば変わるので、
    // 「想定どおりのランタイム一式が入っているか」を見る側では
    // 数えません（そちらは別途、空でないことだけ確かめます）。
    std::size_t CountExportedFilesExcludingShaderCache(
        const std::filesystem::path& outputDirectory)
    {
        std::size_t count{};
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                outputDirectory))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            if (entry.path().parent_path().filename()
                == L"shader-cache")
            {
                continue;
            }
            ++count;
        }
        return count;
    }

    std::vector<std::uint8_t> ReadBytes(
        const std::filesystem::path& path)
    {
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        Require(
            static_cast<bool>(input),
            "Could not open a file for reading.");
        const auto end = input.tellg();
        Require(
            end >= 0,
            "Could not determine the size of a file.");
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(end));
        input.seekg(0);
        if (!bytes.empty())
        {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        return bytes;
    }

    void WriteBytes(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        Require(
            static_cast<bool>(output),
            "Could not create a test file.");
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    // 偽のLamaPonRuntime.dll。本物と同じように鍵スロットを1つだけ
    // 持たせます。書き出しはこのスロットを、そのゲームだけの鍵で
    // 書き換えます（スロットが無ければ書き出しは失敗するのが正しい
    // 挙動なので、テストの偽物にも必ず入れます）。
    constexpr std::size_t FakeRuntimeKeySlotOffset = 64;

    void WriteFakeRuntimeLibrary(
        const std::filesystem::path& path)
    {
        std::vector<std::uint8_t> bytes(
            FakeRuntimeKeySlotOffset,
            0x2a);
        // 目印さえ入っていれば書き換えられます（続く64バイトは
        // 書き出しで丸ごと上書きされるので、中身は何でも構いません）。
        const auto marker =
            LamaPon::Crypto::ExpectedKeySlotMarker();
        bytes.insert(bytes.end(), marker.begin(), marker.end());
        bytes.insert(
            bytes.end(),
            LamaPon::Crypto::KeySlotSize - marker.size(),
            0x71);
        bytes.insert(bytes.end(), 32, 0x5c);
        WriteBytes(path, bytes);
    }

    // 書き出したDLLへ焼き込まれた鍵を、スロットの並びから戻します。
    LamaPon::Crypto::AesKey ReadEmbeddedArchiveKey(
        const std::filesystem::path& runtimeLibrary)
    {
        const auto bytes = ReadBytes(runtimeLibrary);
        Require(
            bytes.size()
                >= FakeRuntimeKeySlotOffset
                    + LamaPon::Crypto::KeySlotSize,
            "Exported runtime is too small to hold a key slot.");
        LamaPon::Crypto::AesKey key{};
        for (std::size_t index = 0; index < key.size(); ++index)
        {
            const auto pad = bytes[
                FakeRuntimeKeySlotOffset
                + LamaPon::Crypto::KeySlotMarkerSize
                + index];
            const auto stored = bytes[
                FakeRuntimeKeySlotOffset
                + LamaPon::Crypto::KeySlotMarkerSize
                + LamaPon::Crypto::AesKeySize
                + index];
            key[index] = static_cast<std::uint8_t>(stored ^ pad);
        }
        return key;
    }

    // 封筒（暗号化）済みなら開いてから、平文ならそのまま文字列で返します。
    std::string ReadSealedText(
        const std::filesystem::path& path,
        const LamaPon::Crypto::AesKey& key)
    {
        const auto bytes = ReadBytes(path);
        if (!LamaPon::Crypto::IsSealed(
                bytes.data(),
                bytes.size()))
        {
            return std::string(bytes.begin(), bytes.end());
        }
        const auto plain = LamaPon::Crypto::Unseal(
            bytes.data(),
            bytes.size(),
            key);
        Require(
            plain.has_value(),
            "A sealed export file could not be opened with the"
                " key embedded in the exported runtime.");
        return std::string(plain->begin(), plain->end());
    }

    bool ContainsKeySlotMarker(
        const std::filesystem::path& path)
    {
        const auto bytes = ReadBytes(path);
        const auto marker =
            LamaPon::Crypto::ExpectedKeySlotMarker();
        return std::search(
            bytes.begin(),
            bytes.end(),
            marker.begin(),
            marker.end()) != bytes.end();
    }

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create test file.");
        }
        output << contents;
    }
}

int main()
{
    try
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "game-exporter";
        std::filesystem::remove_all(root);

        const auto runtimeDirectory = root / "runtime";
        const auto assetDirectory = root / "project" / "assets";
        const auto outputDirectory = root / "dist" / "MyGame";
        const auto startupScene =
            std::filesystem::path(L"scenes/日本語.scene.json");
        LamaPon::ProjectSettings projectSettings{
            "日本語ゲーム",
            1600,
            900,
            startupScene
        };
        projectSettings.splashScreenEnabled = false;
        // 1本目は「HLSLを残す」側の書き出しとして検証します
        // （外す側は後半のstrippedSettingsで別に見ます）。既定値へ
        // 任せると、既定が変わったときにshader_featureのバリアント数の
        // 検証が黙って別のことを測り始めます。
        projectSettings.stripShaderSourceOnExport = false;
        projectSettings.graphics =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::Ultra);
        projectSettings.graphics.targetFrameRate = 144;
        projectSettings.inputActions = {
            {
                "Dash",
                {
                    {
                        LamaPon::InputControl::KeyboardLeftShift,
                        1.0f
                    },
                    {
                        LamaPon::InputControl::GamePadRightShoulder,
                        1.0f
                    }
                }
            }
        };

        // 書き出し時にシェーダーが事前コンパイルされることを見るため、
        // 本物としてコンパイルできるHLSLを1本置きます。#includeも
        // 使い、依存の記録がアセットルート相対で残ることも兼ねて
        // 確かめます（絶対パスで残すと、配布先で必ず外れます）。
        WriteFile(
            assetDirectory / "shaders" / "TestCommon.hlsli",
            "float TestTint() { return 0.5f; }\n");
        WriteFile(
            assetDirectory / "shaders" / "TestExport.hlsl",
            "#include \"TestCommon.hlsli\"\n"
            // multi_compileは常に全組み合わせ、shader_featureは
            // 使われているものだけ。ここでは EXPORT_FEATURE_ON を
            // どのシーンも使っていないので、落ちるのが正解です。
            "#pragma multi_compile _ EXPORT_MULTI_ON\n"
            "#pragma shader_feature _ EXPORT_FEATURE_ON\n"
            "float4 PSMain(\n"
            "    float4 color : COLOR0,\n"
            "    float2 uv : TEXCOORD0,\n"
            "    float4 position : SV_Position) : SV_Target\n"
            "{\n"
            "    return color * TestTint();\n"
            "}\n");

        // アイコン埋め込みを検証するため、ゲームexeには本物のPE
        // （このテスト自身のコピー）を使います。
        std::filesystem::create_directories(
            runtimeDirectory);
        std::filesystem::copy_file(
            SelfExecutablePath(),
            runtimeDirectory / "LamaPonGame.exe");
        WriteFakeRuntimeLibrary(
            runtimeDirectory / "LamaPonRuntime.dll");
        WriteFile(
            runtimeDirectory / "xaudio2_9redist.dll",
            "audio-runtime");
        WriteFile(
            runtimeDirectory / "LamaPonGameModule.dll",
            "runtime-game-module");
        // 同梱対象のVC++ランタイム（あればコピーされる）。
        WriteFile(
            runtimeDirectory / "vcruntime140.dll",
            "crt");
        WriteFile(
            runtimeDirectory / "msvcp140.dll",
            "crt");

        // ゲームアイコン: 16pxと32pxの2枚を持つICOを生成して
        // アセットとして配置します。
        LamaPon::IconImage smallIcon;
        smallIcon.width = 16;
        smallIcon.height = 16;
        smallIcon.bgraPixels.resize(
            16 * 16 * 4,
            std::byte{ 0x7F });
        LamaPon::IconImage largeIcon;
        largeIcon.width = 32;
        largeIcon.height = 32;
        largeIcon.bgraPixels.resize(
            32 * 32 * 4,
            std::byte{ 0x3F });
        const auto icoBytes = LamaPon::BuildIcoFileBytes(
            { smallIcon, largeIcon });
        Require(
            icoBytes.size() > 6 + 16 * 2
                && icoBytes[0] == std::byte{ 0 }
                && icoBytes[2] == std::byte{ 1 }
                && icoBytes[4] == std::byte{ 2 },
            "Generated .ico header is malformed.");
        {
            std::filesystem::create_directories(
                assetDirectory / "icons");
            std::ofstream icoOutput(
                assetDirectory / "icons" / "game.ico",
                std::ios::binary | std::ios::trunc);
            icoOutput.write(
                reinterpret_cast<const char*>(
                    icoBytes.data()),
                static_cast<std::streamsize>(
                    icoBytes.size()));
        }
        projectSettings.gameIcon =
            std::filesystem::path("icons") / "game.ico";

        // .icoはヘッダー検証の上そのまま使われます。
        Require(
            LamaPon::BuildIcoFromImageFile(
                assetDirectory / "icons" / "game.ico")
                == icoBytes,
            ".ico passthrough altered the bytes.");
        const auto projectGameModule =
            root / "project" / ".lamapon" / "bin"
            / "LamaPonGameModule.dll";
        WriteFile(
            projectGameModule,
            "project-game-module");
        const auto projectScript =
            assetDirectory / "scripts" / "TestScript.cpp";
        WriteFile(
            projectScript,
            "// Export freshness test source.\n");
        WriteFile(
            projectGameModule.parent_path()
                / "middleware.dll",
            "project-middleware");
        // 平文漏れ検査の目印は長い一意な文字列にします。短い文字列
        // （"{}"など）は暗号化後のバイト列に偶然出現する確率が
        // 無視できず、テストがまれに失敗します。
        constexpr const char* sceneMarker =
            R"({"marker":"LAMAPON_PLAINTEXT_SCENE_MARKER"})";
        constexpr const char* textureMarker =
            "LAMAPON_PLAINTEXT_TEXTURE_MARKER";
        WriteFile(
            assetDirectory / startupScene,
            sceneMarker);
        WriteFile(
            assetDirectory / "textures" / "sample.bin",
            textureMarker);

        // 実際にInk Ridgeで起きた回帰: Engineを更新した後も古いGame
        // Moduleを梱包でき、起動するとNative Scriptが全て未解決のまま
        // 背景だけ表示されました。配布前に必ず拒否します。
        const auto runtimeWriteTime =
            std::filesystem::last_write_time(
                runtimeDirectory / "LamaPonRuntime.dll");
        std::filesystem::last_write_time(
            projectScript,
            runtimeWriteTime - std::chrono::minutes(2));
        std::filesystem::last_write_time(
            projectGameModule,
            runtimeWriteTime - std::chrono::minutes(1));
        bool staleModuleRejected = false;
        try
        {
            static_cast<void>(LamaPon::ExportGamePackage(
                LamaPon::GameExportOptions{
                    runtimeDirectory,
                    assetDirectory,
                    root / "dist" / "StaleModule",
                    projectSettings,
                    projectGameModule
                }));
        }
        catch (const std::exception&)
        {
            staleModuleRejected = true;
        }
        Require(
            staleModuleRejected,
            "An export with a Game Module older than the Runtime was accepted.");

        std::filesystem::last_write_time(
            projectGameModule,
            runtimeWriteTime + std::chrono::minutes(1));
        std::filesystem::last_write_time(
            projectScript,
            runtimeWriteTime + std::chrono::minutes(2));
        bool staleSourceRejected = false;
        try
        {
            static_cast<void>(LamaPon::ExportGamePackage(
                LamaPon::GameExportOptions{
                    runtimeDirectory,
                    assetDirectory,
                    root / "dist" / "StaleSource",
                    projectSettings,
                    projectGameModule
                }));
        }
        catch (const std::exception&)
        {
            staleSourceRejected = true;
        }
        Require(
            staleSourceRejected,
            "An export with C++ sources newer than the Game Module was accepted.");
        std::filesystem::last_write_time(
            projectScript,
            runtimeWriteTime);

        const auto first = LamaPon::ExportGamePackage(
            LamaPon::GameExportOptions{
                runtimeDirectory,
                assetDirectory,
                outputDirectory,
                projectSettings,
                projectGameModule
            });
        Require(
            CountExportedFilesExcludingShaderCache(
                outputDirectory) == 9,
            "Unexpected exported file count.");
        // 実行ファイルはゲーム名を反映した名前になります。
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / L"日本語ゲーム.exe"),
            "Game executable was not exported with the game name.");
        Require(
            first.executablePath
                == outputDirectory / L"日本語ゲーム.exe",
            "Result did not report the renamed executable.");
        Require(
            !std::filesystem::exists(
                outputDirectory / "LamaPonGame.exe"),
            "Executable kept its default name.");
        RequireEmbeddedIcon(first.executablePath, 2);
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "vcruntime140.dll")
                && std::filesystem::is_regular_file(
                    outputDirectory / "msvcp140.dll"),
            "VC++ runtime DLLs were not bundled.");
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "LamaPonRuntime.dll"),
            "Runtime DLL was not exported.");
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "xaudio2_9redist.dll"),
            "XAudio2 Redistributable was not exported.");
        Require(
            std::filesystem::is_regular_file(
                outputDirectory
                    / "LamaPonGameModule.dll"),
            "Game Module was not exported.");
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "middleware.dll"),
            "Project-local runtime DLL was not exported.");
        {
            std::ifstream moduleInput(
                outputDirectory / "LamaPonGameModule.dll",
                std::ios::binary);
            const std::string moduleContents{
                std::istreambuf_iterator<char>{ moduleInput },
                std::istreambuf_iterator<char>{}
            };
            Require(
                moduleContents == "project-game-module",
                "Export did not prefer the project Game Module.");
        }
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "assets.tpak"),
            "Encrypted asset archive was not exported.");
        // 書き出しはゲームごとに鍵を作り、配布した
        // LamaPonRuntime.dllへ焼き込みます。以降の検証は、その鍵を
        // 取り出してから行います。
        const auto exportedRuntime =
            outputDirectory / "LamaPonRuntime.dll";
        const auto embeddedKey =
            ReadEmbeddedArchiveKey(exportedRuntime);
        {
            // 事前コンパイル済みシェーダーの同梱。無いと、
            // プレイヤーの初回起動で全部コンパイルすることになります。
            const auto shaderCache =
                outputDirectory / "shader-cache";
            Require(
                std::filesystem::is_directory(shaderCache),
                "Precompiled shader cache was not exported.");
            std::size_t byteCodeCount{};
            std::size_t manifestCount{};
            for (const auto& entry :
                std::filesystem::directory_iterator(
                    shaderCache))
            {
                if (entry.path().extension() == ".cso")
                {
                    ++byteCodeCount;
                }
                else if (entry.path().extension() == ".deps")
                {
                    ++manifestCount;
                }
            }
            Require(
                byteCodeCount > 0,
                "No compiled shader bytecode was exported.");
            Require(
                manifestCount >= byteCodeCount,
                "Compiled shaders were exported without their"
                    " dependency manifests.");

            // shader_featureのストリップ。EXPORT_MULTI_ONは
            // multi_compileなので必ず2通り焼かれ、
            // EXPORT_FEATURE_ONはどのシーンも使っていないので
            // 落ちます。つまりバリアントは2通りのはずです。
            // 落ちていなければ4通りぶんの.depsが出ます。
            std::size_t exportManifests{};
            for (const auto& entry :
                std::filesystem::directory_iterator(
                    shaderCache))
            {
                if (entry.path().extension() != ".deps")
                {
                    continue;
                }
                std::istringstream manifest(
                    ReadSealedText(entry.path(), embeddedKey));
                std::string line;
                if (std::getline(manifest, line)
                    && line.find("TestExport.hlsl")
                        != std::string::npos)
                {
                    ++exportManifests;
                }
            }
            // 入口の総当たり本数×バリアント数。入口の数はエンジンの
            // 都合で変わるので、ここでは「4通りぶんには届かない」
            // ことだけを見ます。
            const auto entryPoints =
                LamaPon::KnownShaderEntryPoints().size();
            Require(
                exportManifests == entryPoints * 2,
                "shader_feature variants that no material uses"
                    " must be stripped from the export.");
        }
        Require(
            !std::filesystem::exists(
                outputDirectory / "assets"),
            "Loose assets were exported instead of the encrypted archive.");
        // --- 書き出したゲームだけの鍵になっているか ---
        Require(
            embeddedKey != LamaPon::Crypto::ArchiveKey(),
            "The export must embed its own archive key, not the"
                " key that ships inside the engine.");
        Require(
            !ContainsKeySlotMarker(exportedRuntime),
            "The key slot marker must be erased from the"
                " exported runtime; otherwise the key can be"
                " located by pattern.");
        {
            // エンジン既定の鍵では開けないこと。ここが通ってしまうと
            // 「1本解けば全ゲーム開く」状態に戻っています。
            bool openedWithEngineKey = true;
            try
            {
                static_cast<void>(
                    LamaPon::AssetArchive::Open(
                        outputDirectory / "assets.tpak"));
            }
            catch (const std::exception&)
            {
                openedWithEngineKey = false;
            }
            Require(
                !openedWithEngineKey,
                "The exported archive must not open with the"
                    " engine's built-in key.");
        }
        {
            // 索引を1バイト書き換えたら開けないこと（改ざん検知）。
            const auto tampered =
                outputDirectory.parent_path() / "tampered.tpak";
            auto bytes = ReadBytes(
                outputDirectory / "assets.tpak");
            constexpr std::size_t headerSize = 8 + 8 + 16 + 32;
            Require(
                bytes.size() > headerSize,
                "Archive is too small to tamper with.");
            bytes[headerSize] =
                static_cast<std::uint8_t>(
                    bytes[headerSize] ^ 0xff);
            WriteBytes(tampered, bytes);
            bool openedTampered = true;
            try
            {
                static_cast<void>(
                    LamaPon::AssetArchive::Open(
                        tampered,
                        embeddedKey));
            }
            catch (const std::exception&)
            {
                openedTampered = false;
            }
            Require(
                !openedTampered,
                "A tampered archive index must be rejected.");
        }
        {
            // 同梱した事前コンパイル済みシェーダーが暗号化されて
            // いること（HLSLを外してもDXBCが素で置いてあれば読めます）。
            const auto index = ReadBytes(
                outputDirectory / "shader-cache" / "index.txt");
            Require(
                LamaPon::Crypto::IsSealed(
                    index.data(),
                    index.size()),
                "The bundled shader cache must be encrypted.");
        }
        {
            const auto archive = LamaPon::AssetArchive::Open(
                outputDirectory / "assets.tpak",
                embeddedKey);
            Require(
                archive != nullptr,
                "Encrypted asset archive could not be opened.");
            Require(
                archive->Contains(startupScene),
                "Startup scene is missing from the encrypted archive.");
            const auto decryptedScene =
                archive->TryRead(startupScene);
            Require(
                decryptedScene.has_value()
                    && std::string(
                        decryptedScene->begin(),
                        decryptedScene->end()) == sceneMarker,
                "Startup scene did not decrypt to its original contents.");

            const auto texturePath =
                std::filesystem::path("textures")
                    / "sample.bin";
            Require(
                archive->Contains(texturePath),
                "Texture asset is missing from the encrypted archive.");
            const auto decryptedTexture =
                archive->TryRead(texturePath);
            Require(
                decryptedTexture.has_value()
                    && std::string(
                        decryptedTexture->begin(),
                        decryptedTexture->end())
                        == textureMarker,
                "Texture asset did not decrypt to its original contents.");

            std::ifstream rawArchive(
                outputDirectory / "assets.tpak",
                std::ios::binary);
            const std::string rawArchiveBytes{
                std::istreambuf_iterator<char>{ rawArchive },
                std::istreambuf_iterator<char>{}
            };
            Require(
                rawArchiveBytes.find(textureMarker)
                        == std::string::npos
                    && rawArchiveBytes.find(
                        "LAMAPON_PLAINTEXT_SCENE_MARKER")
                        == std::string::npos,
                "Encrypted archive contains plaintext asset content.");
        }

        nlohmann::json settings;
        {
            std::ifstream input(
                outputDirectory / "LamaPonGame.json",
                std::ios::binary);
            input >> settings;
        }
        Require(
            settings.at("startupScene").get<std::string>()
                == "scenes/日本語.scene.json",
            "Startup scene setting was not exported.");
        Require(
            !settings.at("splashScreenEnabled").get<bool>(),
            "Startup splash setting was not exported.");
        Require(
            settings.at("gameName").get<std::string>()
                == "日本語ゲーム",
            "Game name was not exported.");
        Require(
            settings.at("window").at("width").get<int>()
                == 1600
                && settings.at("window").at("height").get<int>()
                    == 900,
            "Window size was not exported.");
        Require(
            settings.at("graphics").at("preset")
                .get<std::string>() == "Ultra"
                && settings.at("graphics")
                    .at("shadowResolution")
                    .get<int>() == 4096
                && settings.at("graphics")
                    .at("antiAliasingEnabled")
                    .get<bool>()
                && settings.at("graphics")
                    .at("targetFrameRate")
                    .get<int>() == 144,
            "Graphics quality was not exported.");
        Require(
            settings.at("inputActions").at(0)
                .at("name").get<std::string>() == "Dash"
                && settings.at("inputActions").at(0)
                    .at("bindings").at(1)
                    .at("control").get<std::string>()
                    == "GamePadRightShoulder",
            "Input actions were not exported.");
        const auto loadedSettings =
            LamaPon::LoadProjectSettings(
                outputDirectory / "LamaPonGame.json");
        Require(
            loadedSettings.gameName == projectSettings.gameName
                && loadedSettings.windowWidth == 1600
                && loadedSettings.windowHeight == 900
                && loadedSettings.startupScene == startupScene
                && !loadedSettings.splashScreenEnabled
                && loadedSettings.graphics.preset
                    == LamaPon::GraphicsQualityPreset::Ultra
                && loadedSettings.graphics.shadowResolution
                    == 4096
                && loadedSettings.graphics.shadowCascadeLimit
                    == 4
                && loadedSettings.graphics.targetFrameRate
                    == 144
                && loadedSettings.inputActions.size() == 1
                && loadedSettings.inputActions[0].name
                    == "Dash"
                && loadedSettings.inputActions[0]
                    .bindings[0].control
                    == LamaPon::InputControl::
                        KeyboardLeftShift,
            "Exported project settings did not round-trip.");

        bool invalidSettingsRejected = false;
        try
        {
            auto invalidSettings = projectSettings;
            invalidSettings.windowWidth = 100;
            LamaPon::ValidateProjectSettings(
                invalidSettings);
        }
        catch (const std::exception&)
        {
            invalidSettingsRejected = true;
        }
        Require(
            invalidSettingsRejected,
            "Invalid project settings were accepted.");

        invalidSettingsRejected = false;
        try
        {
            auto invalidSettings = projectSettings;
            invalidSettings.graphics.renderScale =
                0.25f;
            LamaPon::ValidateProjectSettings(
                invalidSettings);
        }
        catch (const std::exception&)
        {
            invalidSettingsRejected = true;
        }
        Require(
            invalidSettingsRejected,
            "Invalid graphics settings were accepted.");

        // 1.0を超える描画スケールはスーパーサンプリングとして有効です
        // （2.0が上限）。ここが再び1.0で弾かれないよう固定します。
        {
            auto supersampledSettings = projectSettings;
            supersampledSettings.graphics.renderScale = 2.0f;
            LamaPon::ValidateProjectSettings(
                supersampledSettings);
        }

        invalidSettingsRejected = false;
        try
        {
            auto invalidSettings = projectSettings;
            invalidSettings.graphics.renderScale = 2.5f;
            LamaPon::ValidateProjectSettings(
                invalidSettings);
        }
        catch (const std::exception&)
        {
            invalidSettingsRejected = true;
        }
        Require(
            invalidSettingsRejected,
            "Render scale above 2.0 was accepted.");

        invalidSettingsRejected = false;
        try
        {
            auto invalidSettings = projectSettings;
            invalidSettings.graphics.targetFrameRate =
                10;
            LamaPon::ValidateProjectSettings(
                invalidSettings);
        }
        catch (const std::exception&)
        {
            invalidSettingsRejected = true;
        }
        Require(
            invalidSettingsRejected,
            "Invalid target frame rate was accepted.");

        WriteFile(
            outputDirectory / "stale-file.txt",
            "stale");
        const auto second = LamaPon::ExportGamePackage(
            LamaPon::GameExportOptions{
                runtimeDirectory,
                assetDirectory,
                outputDirectory,
                projectSettings,
                projectGameModule
            });
        Require(
            CountExportedFilesExcludingShaderCache(
                outputDirectory) == 9,
            "Re-export produced an unexpected file count.");
        Require(
            !std::filesystem::exists(
                outputDirectory / "stale-file.txt"),
            "Re-export did not replace stale output.");

        // ZIP作成オプション: 出力フォルダーの隣へ.zipができます。
        LamaPon::GameExportOptions zipOptions{
            runtimeDirectory,
            assetDirectory,
            outputDirectory,
            projectSettings,
            projectGameModule
        };
        zipOptions.createZipArchive = true;
        const auto zipped = LamaPon::ExportGamePackage(
            zipOptions);
        Require(
            !zipped.zipPath.empty()
                && std::filesystem::is_regular_file(
                    zipped.zipPath)
                && std::filesystem::file_size(
                    zipped.zipPath) > 22,
            "Distribution zip was missing or empty.");
        Require(
            zipped.zipPath.parent_path()
                == outputDirectory.parent_path(),
            "Distribution zip is not beside the output folder.");

        // ゲーム名→ファイル名の整形規則。
        Require(
            LamaPon::SanitizeGameFileName("My: Game?")
                == L"My_ Game_",
            "Invalid characters were not replaced.");
        Require(
            LamaPon::SanitizeGameFileName(" . ")
                == L"LamaPonGame",
            "Empty sanitized names must fall back.");
        Require(
            LamaPon::SanitizeGameFileName("CON")
                == L"_CON",
            "Reserved device names must be escaped.");
        Require(
            LamaPon::SanitizeGameFileName("日本語ゲーム")
                == L"日本語ゲーム",
            "Japanese game names must pass through.");

        bool unsafeDestinationRejected = false;
        try
        {
            static_cast<void>(
                LamaPon::ExportGamePackage(
                    LamaPon::GameExportOptions{
                        runtimeDirectory,
                        assetDirectory,
                        assetDirectory / "export",
                        projectSettings
                    }));
        }
        catch (const std::exception&)
        {
            unsafeDestinationRejected = true;
        }
        Require(
            unsafeDestinationRejected,
            "Unsafe destination inside assets was accepted.");

        // HLSLソースを外す設定。配布物にはバイトコードと索引だけが
        // 入り、.hlslはアーカイブから消えることを確かめます。
        {
            auto strippedSettings = projectSettings;
            strippedSettings.stripShaderSourceOnExport = true;
            const auto strippedOutput =
                root / "dist" / "Stripped";
            const auto stripped =
                LamaPon::ExportGamePackage(
                    LamaPon::GameExportOptions{
                        runtimeDirectory,
                        assetDirectory,
                        strippedOutput,
                        strippedSettings
                    });
            static_cast<void>(stripped);

            const auto strippedKey = ReadEmbeddedArchiveKey(
                strippedOutput / "LamaPonRuntime.dll");
            Require(
                strippedKey != embeddedKey,
                "Every export must get a fresh archive key.");
            const auto archive = LamaPon::AssetArchive::Open(
                strippedOutput / "assets.tpak",
                strippedKey);
            Require(
                archive != nullptr,
                "Stripped export produced no archive.");
            Require(
                !archive->Contains(
                    std::filesystem::path{ "shaders" }
                        / "TestExport.hlsl"),
                "HLSL source must be excluded when"
                    " stripShaderSourceOnExport is set.");
            Require(
                !archive->Contains(
                    std::filesystem::path{ "shaders" }
                        / "TestCommon.hlsli"),
                "HLSL includes must be excluded too.");
            Require(
                archive->Contains(startupScene),
                "Stripping shaders must not drop other"
                    " assets.");
            // ソースが無いとハッシュからキーを作れないので、
            // パスから引ける索引が要ります。
            Require(
                std::filesystem::is_regular_file(
                    strippedOutput
                    / "shader-cache"
                    / "index.txt"),
                "A stripped export must ship the shader cache"
                    " index; without it the bytecode cannot be"
                    " looked up at runtime.");
        }

        std::cout << "Game exporter tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
