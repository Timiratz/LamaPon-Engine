#include "LamaPon/Editor/Live2DCubismSetup.h"

#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace LamaPon
{
    namespace
    {
        namespace fs = std::filesystem;

        // ヘッダー内で関数宣言があるか、関数名直後の空白を許容して調べます(header: Coreヘッダー, functionName: 関数名)。
        [[nodiscard]] bool HasFunctionDeclaration(
            const std::string& header,
            const std::string_view functionName)
        {
            std::size_t position = 0;
            while ((position = header.find(functionName, position))
                != std::string::npos)
            {
                position += functionName.size();
                while (position < header.size()
                    && std::isspace(
                        static_cast<unsigned char>(header[position])))
                {
                    ++position;
                }
                if (position < header.size() && header[position] == '(')
                {
                    return true;
                }
            }
            return false;
        }

        // 指定ファイルが64-bit Windows PE DLLかを調べます(path: SDKのRuntime DLL)。
        [[nodiscard]] bool IsAmd64Pe(const fs::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return false;
            }

            std::uint16_t dosMagic{};
            std::uint32_t peOffset{};
            stream.seekg(0, std::ios::beg);
            stream.read(
                reinterpret_cast<char*>(&dosMagic),
                sizeof(dosMagic));
            stream.seekg(0x3c, std::ios::beg);
            stream.read(
                reinterpret_cast<char*>(&peOffset),
                sizeof(peOffset));
            if (!stream || dosMagic != 0x5a4d || peOffset < 64)
            {
                return false;
            }

            std::uint32_t signature{};
            std::uint16_t machine{};
            stream.seekg(static_cast<std::streamoff>(peOffset));
            stream.read(
                reinterpret_cast<char*>(&signature),
                sizeof(signature));
            stream.read(
                reinterpret_cast<char*>(&machine),
                sizeof(machine));
            return stream
                && signature == 0x00004550
                && machine == IMAGE_FILE_MACHINE_AMD64;
        }

        // 指定フォルダーから名前が一致する最初の通常ファイルを返します(root: 検索フォルダー, fileName: ファイル名)。
        [[nodiscard]] fs::path FindFileRecursively(
            const fs::path& root,
            const std::wstring_view fileName)
        {
            std::error_code error;
            if (!fs::is_directory(root, error) || error)
            {
                return {};
            }
            std::vector<fs::path> matches;
            for (fs::recursive_directory_iterator iterator{
                    root,
                    fs::directory_options::skip_permission_denied,
                    error };
                !error && iterator != fs::recursive_directory_iterator{};
                iterator.increment(error))
            {
                if (iterator->is_regular_file(error)
                    && _wcsicmp(
                        iterator->path().filename().c_str(),
                        std::wstring{ fileName }.c_str()) == 0)
                {
                    matches.push_back(iterator->path());
                }
            }
            if (error)
            {
                throw std::runtime_error(
                    "SDKフォルダーを読み取れませんでした。");
            }
            std::ranges::sort(matches);
            return matches.empty() ? fs::path{} : matches.front();
        }

        // manifest内の文字列配列へ値を重複なく追加します(values: 更新する配列, value: 追加する相対パスまたは定義名)。
        void AddUniqueJsonString(
            nlohmann::json& values,
            const std::string_view value)
        {
            if (!values.is_array())
            {
                values = nlohmann::json::array();
            }
            if (std::ranges::none_of(
                    values,
                    [value](const nlohmann::json& entry)
                    {
                        return entry.is_string()
                            && entry.get<std::string>() == value;
                    }))
            {
                values.push_back(value);
            }
        }

        // 相対パスがルート内にあるかを正規化後に調べます(root: 許可するルート, relative: 参照する相対ファイル)。
        [[nodiscard]] fs::path ResolveModelReference(
            const fs::path& root,
            const std::string& relative)
        {
            const fs::path relativePath = PathFromUtf8(relative);
            if (relative.empty()
                || relativePath.is_absolute()
                || !relativePath.root_name().empty()
                || !relativePath.root_directory().empty())
            {
                throw std::runtime_error(
                    "モデル参照にassetsフォルダー外のパスがあります。");
            }

            std::error_code error;
            const fs::path resolved = fs::weakly_canonical(
                root / relativePath,
                error);
            if (error)
            {
                throw std::runtime_error(
                    "モデル参照先のパスを確認できませんでした。");
            }
            const fs::path canonicalRoot = fs::weakly_canonical(
                root,
                error);
            if (error)
            {
                throw std::runtime_error(
                    "モデルフォルダーのパスを確認できませんでした。");
            }
            const fs::path relativeResolved =
                resolved.lexically_relative(canonicalRoot);
            if (relativeResolved.empty()
                || relativeResolved.is_absolute()
                || *relativeResolved.begin() == L".."
                || !fs::is_regular_file(resolved, error)
                || error)
            {
                throw std::runtime_error(
                    "モデル3 JSONが参照するMOCまたはテクスチャが見つかりません。"
                    "ファイルがすべて同じモデルフォルダー内にあるか確認してください。");
            }
            return resolved;
        }

        // 候補パスがルート自身またはその子かを判定します(path: 調べるパス, root: 比較するルート)。
        [[nodiscard]] bool IsPathWithin(
            const fs::path& path,
            const fs::path& root)
        {
            const fs::path relative = path.lexically_relative(root);
            return relative.empty()
                ? path == root
                : !relative.is_absolute()
                    && *relative.begin() != L"..";
        }

        // Windowsのフォルダー名として安全な表示名を作ります(name: モデルフォルダー名)。
        [[nodiscard]] std::wstring SafeModelDirectoryName(
            std::wstring name)
        {
            for (auto& character : name)
            {
                if (character < 32
                    || character == L'<'
                    || character == L'>'
                    || character == L':'
                    || character == L'"'
                    || character == L'/'
                    || character == L'\\'
                    || character == L'|'
                    || character == L'?'
                    || character == L'*')
                {
                    character = L'_';
                }
            }
            while (!name.empty()
                && (name.back() == L'.' || name.back() == L' '))
            {
                name.pop_back();
            }
            if (name.empty())
            {
                name = L"Live2DModel";
            }
            const std::wstring upper = [&name]
            {
                std::wstring value = name;
                std::ranges::transform(
                    value,
                    value.begin(),
                    [](const wchar_t character)
                    {
                        return static_cast<wchar_t>(
                            std::towupper(character));
                    });
                return value;
            }();
            constexpr std::array<std::wstring_view, 4> reserved{
                L"CON", L"PRN", L"AUX", L"NUL" };
            if (std::ranges::find(reserved, upper)
                    != reserved.end()
                || (upper.size() == 4
                    && (upper.starts_with(L"COM")
                        || upper.starts_with(L"LPT"))
                    && upper.back() >= L'1'
                    && upper.back() <= L'9'))
            {
                name.insert(name.begin(), L'_');
            }
            return name;
        }

        // モデルフォルダーをリンクを除外して複製します(source: model3 JSONがあるフォルダー, destination: assets内の新規フォルダー)。
        void CopyModelDirectory(
            const fs::path& source,
            const fs::path& destination)
        {
            std::error_code error;
            fs::create_directories(destination, error);
            if (error)
            {
                throw std::runtime_error(
                    "Live2Dモデルの保存フォルダーを作成できませんでした。");
            }

            try
            {
                for (fs::recursive_directory_iterator iterator{
                        source,
                        fs::directory_options::skip_permission_denied,
                        error };
                    !error
                        && iterator != fs::recursive_directory_iterator{};
                    iterator.increment(error))
                {
                    const fs::file_status status =
                        iterator->symlink_status(error);
                    if (error)
                    {
                        break;
                    }
                    if (fs::is_symlink(status))
                    {
                        if (iterator->is_directory(error))
                        {
                            iterator.disable_recursion_pending();
                        }
                        continue;
                    }

                    const fs::path relative =
                        iterator->path().lexically_relative(source);
                    const fs::path target = destination / relative;
                    if (fs::is_directory(status))
                    {
                        fs::create_directories(target, error);
                    }
                    else if (fs::is_regular_file(status))
                    {
                        fs::create_directories(target.parent_path(), error);
                        if (!error)
                        {
                            fs::copy_file(
                                iterator->path(),
                                target,
                                fs::copy_options::none,
                                error);
                        }
                    }
                    if (error)
                    {
                        break;
                    }
                }
                if (error)
                {
                    throw std::runtime_error(
                        "モデルファイルをコピーできませんでした。");
                }
            }
            catch (...)
            {
                std::error_code ignored;
                fs::remove_all(destination, ignored);
                throw;
            }
        }
    }

    Live2DCubismSdkInspection InspectLive2DCubismSdk(
        const std::filesystem::path& sdkRoot)
    {
        Live2DCubismSdkInspection result;
        std::error_code error;
        result.sdkRoot = fs::weakly_canonical(sdkRoot, error);
        if (error || !fs::is_directory(result.sdkRoot, error) || error)
        {
            result.error = "Cubism SDKの展開フォルダーを確認できません。";
            return result;
        }

        const fs::path coreRoot = result.sdkRoot / L"Core";
        result.includeDirectory = coreRoot / L"include";
        const fs::path header =
            result.includeDirectory / L"Live2DCubismCore.h";
        if (!fs::is_regular_file(header, error) || error)
        {
            result.error =
                "選択したフォルダーにCore/include/Live2DCubismCore.hがありません。"
                "Cubism SDK for Nativeの展開先を選んでください。";
            return result;
        }

        std::ifstream headerStream(header, std::ios::binary);
        const std::string headerText{
            std::istreambuf_iterator<char>{ headerStream },
            std::istreambuf_iterator<char>{} };
        if (!headerStream.eof() && headerStream.fail())
        {
            result.error = "Cubism Coreヘッダーを読み取れませんでした。";
            return result;
        }

        if (HasFunctionDeclaration(
                headerText,
                "csmGetRenderOrders"))
        {
            if (!HasFunctionDeclaration(
                    headerText,
                    "csmGetOffscreenCount"))
            {
                result.error =
                    "このCubism Coreは5.3の描画APIがそろっていません。"
                    "公式SDKの最新Native版を選んでください。";
                return result;
            }
            result.usesCubism53 = true;
        }
        else if (!HasFunctionDeclaration(
            headerText,
            "csmGetDrawableRenderOrders"))
        {
            result.error =
                "このCubism Coreの描画APIには対応していません。";
            return result;
        }

        const fs::path libraryRoot =
            coreRoot / L"lib" / L"windows" / L"x86_64";
        if (!fs::is_directory(libraryRoot, error) || error)
        {
            result.error =
                "Windows x64用のCubism Coreライブラリフォルダーがありません。";
            return result;
        }
        std::error_code findError;
        std::vector<fs::path> importLibraries;
        for (fs::recursive_directory_iterator iterator{
                libraryRoot,
                fs::directory_options::skip_permission_denied,
                findError };
            !findError && iterator != fs::recursive_directory_iterator{};
            iterator.increment(findError))
        {
            const std::wstring fileName =
                iterator->path().filename().wstring();
            if (iterator->is_regular_file(findError)
                && fileName.size() >= 7
                && _wcsicmp(
                    fileName.c_str() + fileName.size() - 7,
                    L"_MD.lib") == 0)
            {
                importLibraries.push_back(iterator->path());
            }
        }
        if (findError)
        {
            result.error = "Cubism Coreのライブラリを検索できませんでした。";
            return result;
        }
        std::ranges::sort(importLibraries);
        if (importLibraries.empty())
        {
            result.error =
                "Windows x64用のMSVC /MD import libraryが見つかりません。";
            return result;
        }
        result.importLibrary = importLibraries.front();

        result.runtimeLibrary = FindFileRecursively(
            coreRoot / L"dll" / L"windows" / L"x86_64",
            L"Live2DCubismCore.dll");
        if (result.runtimeLibrary.empty())
        {
            result.error = "Windows x64用のLive2DCubismCore.dllが見つかりません。";
            return result;
        }
        if (!IsAmd64Pe(result.runtimeLibrary))
        {
            result.error =
                "Core DLLはWindows x64用ではありません。"
                "Cubism SDKのwindows/x86_64版を選んでください。";
            return result;
        }
        return result;
    }

    void InstallLive2DCubismSdk(
        const Live2DCubismSdkInspection& inspection,
        const std::filesystem::path& packageRoot)
    {
        if (!inspection.IsValid())
        {
            throw std::invalid_argument(
                inspection.error.empty()
                    ? "Cubism Coreの検査が完了していません。"
                    : inspection.error);
        }

        const auto currentInspection = InspectLive2DCubismSdk(
            inspection.sdkRoot);
        if (!currentInspection.IsValid())
        {
            throw std::invalid_argument(currentInspection.error);
        }

        const fs::path coreDestination =
            packageRoot / L"sdk" / L"Core";
        const fs::path includeDestination =
            coreDestination / L"include";
        const fs::path libraryDestination =
            coreDestination / L"lib" / L"windows" / L"x86_64";
        const fs::path runtimeDestination =
            coreDestination / L"dll" / L"windows" / L"x86_64";
        std::error_code error;
        fs::create_directories(includeDestination, error);
        if (error)
        {
            throw std::runtime_error(
                "プロジェクト内にCubism Coreの保存先を作成できませんでした。"
                "書き込み権限を確認してください。");
        }
        fs::create_directories(libraryDestination, error);
        if (error)
        {
            throw std::runtime_error(
                "Cubism Coreのライブラリ保存先を作成できませんでした。");
        }
        fs::create_directories(runtimeDestination, error);
        if (error)
        {
            throw std::runtime_error(
                "Cubism Core DLLの保存先を作成できませんでした。");
        }

        std::error_code equivalentError;
        const bool alreadyInPackage = fs::equivalent(
            currentInspection.sdkRoot / L"Core",
            coreDestination,
            equivalentError);
        if (!alreadyInPackage || equivalentError)
        {
            fs::copy(
                currentInspection.includeDirectory,
                includeDestination,
                fs::copy_options::recursive
                    | fs::copy_options::overwrite_existing);
            fs::copy_file(
                currentInspection.importLibrary,
                libraryDestination / L"Live2DCubismCore_MD.lib",
                fs::copy_options::overwrite_existing);
            fs::copy_file(
                currentInspection.runtimeLibrary,
                runtimeDestination / L"Live2DCubismCore.dll",
                fs::copy_options::overwrite_existing);
        }

        const fs::path manifestPath = packageRoot / L"package.json";
        std::ifstream manifestStream(manifestPath, std::ios::binary);
        if (!manifestStream)
        {
            throw std::runtime_error(
                "Live2D拡張のpackage.jsonが見つかりません。"
                "Live2D拡張パッケージを先にインストールしてください。");
        }
        auto manifest = nlohmann::json::parse(manifestStream);
        if (!manifest.is_object())
        {
            throw std::runtime_error(
                "Live2D拡張のpackage.jsonが正しい形式ではありません。");
        }
        auto& native = manifest["native"];
        if (!native.is_object())
        {
            native = nlohmann::json::object();
        }
        AddUniqueJsonString(
            native["includeDirectories"],
            "sdk/Core/include");
        AddUniqueJsonString(
            native["libraries"],
            "sdk/Core/lib/windows/x86_64/Live2DCubismCore_MD.lib");
        AddUniqueJsonString(
            native["runtimeFiles"],
            "sdk/Core/dll/windows/x86_64/Live2DCubismCore.dll");
        auto& defines = native["defines"];
        if (!defines.is_array())
        {
            defines = nlohmann::json::array();
        }
        for (auto iterator = defines.begin();
            iterator != defines.end();)
        {
            if (iterator->is_string()
                && iterator->get<std::string>()
                    == "LAMAPON_LIVE2D_CUBISM_53")
            {
                iterator = defines.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        AddUniqueJsonString(
            defines,
            "LAMAPON_LIVE2D_CUBISM_ENABLED");
        if (currentInspection.usesCubism53)
        {
            AddUniqueJsonString(
                defines,
                "LAMAPON_LIVE2D_CUBISM_53");
        }

        std::ofstream manifestOutput(
            manifestPath,
            std::ios::binary | std::ios::trunc);
        if (!manifestOutput)
        {
            throw std::runtime_error(
                "拡張設定を保存できませんでした。"
                "プロジェクトの書き込み権限を確認してください。");
        }
        manifestOutput << manifest.dump(2) << '\n';
        if (!manifestOutput)
        {
            throw std::runtime_error(
                "Cubism Coreの設定をpackage.jsonへ書き込めませんでした。");
        }
    }

    std::filesystem::path ImportLive2DModel(
        const std::filesystem::path& model3Json,
        const std::filesystem::path& assetRoot)
    {
        std::error_code error;
        const fs::path modelPath = fs::canonical(model3Json, error);
        if (error || !fs::is_regular_file(modelPath, error) || error)
        {
            throw std::runtime_error(
                "選択したmodel3 JSONファイルを開けませんでした。");
        }
        const std::wstring modelFileName =
            modelPath.filename().wstring();
        if (modelFileName.size() < 12
            || _wcsicmp(
                modelFileName.c_str() + modelFileName.size() - 12,
                L".model3.json") != 0)
        {
            throw std::runtime_error(
                "Live2Dのmodel3 JSONファイルを選んでください。");
        }

        std::ifstream modelStream(modelPath, std::ios::binary);
        if (!modelStream)
        {
            throw std::runtime_error(
                "model3 JSONファイルを読み取れませんでした。");
        }
        const auto model = nlohmann::json::parse(modelStream);
        if (!model.is_object()
            || !model.contains("FileReferences")
            || !model["FileReferences"].is_object())
        {
            throw std::runtime_error(
                "model3 JSONにFileReferencesがありません。");
        }
        const auto& references = model["FileReferences"];
        if (!references.contains("Moc")
            || !references["Moc"].is_string())
        {
            throw std::runtime_error(
                "model3 JSONにMOCファイルの参照がありません。");
        }
        if (!references.contains("Textures")
            || !references["Textures"].is_array()
            || references["Textures"].empty())
        {
            throw std::runtime_error(
                "model3 JSONにテクスチャの参照がありません。");
        }

        const fs::path modelDirectory = modelPath.parent_path();
        static_cast<void>(ResolveModelReference(
            modelDirectory,
            references["Moc"].get<std::string>()));
        for (const auto& texture : references["Textures"])
        {
            if (!texture.is_string())
            {
                throw std::runtime_error(
                    "model3 JSONのテクスチャ参照が正しくありません。");
            }
            static_cast<void>(ResolveModelReference(
                modelDirectory,
                texture.get<std::string>()));
        }

        const fs::path canonicalAssetRoot = fs::weakly_canonical(
            assetRoot,
            error);
        if (error || !fs::is_directory(canonicalAssetRoot, error) || error)
        {
            throw std::runtime_error(
                "プロジェクトのassetsフォルダーを確認できません。");
        }
        const fs::path targetRoot = canonicalAssetRoot / L"Live2D";
        const fs::path canonicalModelDirectory =
            fs::weakly_canonical(modelDirectory, error);
        if (error)
        {
            throw std::runtime_error(
                "Live2Dモデルフォルダーを確認できませんでした。");
        }
        const fs::path canonicalTargetRoot = fs::weakly_canonical(
            targetRoot,
            error);
        if (error || IsPathWithin(
                canonicalTargetRoot,
                canonicalModelDirectory))
        {
            throw std::runtime_error(
                "モデル全体をassets/Live2Dへコピーできるよう、"
                "model3 JSONを専用フォルダー内に置いてください。");
        }

        const std::wstring baseName = SafeModelDirectoryName(
            modelDirectory.filename().wstring());
        fs::path destination;
        for (std::uint32_t suffix = 1; suffix < 10000; ++suffix)
        {
            const std::wstring candidateName = suffix == 1
                ? baseName
                : baseName + L"-" + std::to_wstring(suffix);
            const fs::path candidate = targetRoot / candidateName;
            if (!fs::exists(candidate, error))
            {
                destination = candidate;
                break;
            }
        }
        if (destination.empty())
        {
            throw std::runtime_error(
                "同じ名前のLive2Dモデルが多すぎます。"
                "モデルフォルダーの名前を変更してください。");
        }

        fs::create_directories(targetRoot, error);
        if (error)
        {
            throw std::runtime_error(
                "assets/Live2Dを作成できませんでした。");
        }
        CopyModelDirectory(canonicalModelDirectory, destination);

        const fs::path relativeModelPath =
            (destination / modelPath.filename())
                .lexically_relative(canonicalAssetRoot);
        if (relativeModelPath.empty() || relativeModelPath.is_absolute())
        {
            std::error_code ignored;
            fs::remove_all(destination, ignored);
            throw std::runtime_error(
                "コピー後のmodel3 JSONパスを決定できませんでした。");
        }
        return relativeModelPath;
    }
}
