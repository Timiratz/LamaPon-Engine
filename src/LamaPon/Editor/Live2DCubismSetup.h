#pragma once

#include <filesystem>
#include <string>

namespace LamaPon
{
    struct Live2DCubismSdkInspection final
    {
        std::filesystem::path sdkRoot;
        std::filesystem::path includeDirectory;
        std::filesystem::path importLibrary;
        std::filesystem::path runtimeLibrary;
        std::string error;
        bool usesCubism53{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return error.empty()
                && !includeDirectory.empty()
                && !importLibrary.empty()
                && !runtimeLibrary.empty();
        }
    };

    // 公式SDK展開先のCoreヘッダー、x64 DLL、MSVC /MD import libraryを検査します。
    [[nodiscard]] Live2DCubismSdkInspection InspectLive2DCubismSdk(
        const std::filesystem::path& sdkRoot);

    // 検査済みCoreをパッケージへ配置し、native依存の設定を有効にします。
    void InstallLive2DCubismSdk(
        const Live2DCubismSdkInspection& inspection,
        const std::filesystem::path& packageRoot);

    // model3 JSONと必須MOC・テクスチャを検証し、assets/Live2Dへコピーします。
    [[nodiscard]] std::filesystem::path ImportLive2DModel(
        const std::filesystem::path& model3Json,
        const std::filesystem::path& assetRoot);
}
