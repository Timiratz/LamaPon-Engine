#include "LamaPon/Editor/ScriptEditorDetection.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Editor/PackageNativeDependencies.h"

#include <Windows.h>
#include <OleAuto.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr auto ProjectGuid = "{7F4354EE-0E42-4F28-87EF-05D258B9DD85}";

    std::string Xml(std::string_view value)
    {
        std::string result;
        for (const char ch : value)
        {
            switch (ch)
            {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += ch; break;
            }
        }
        return result;
    }

    // MSBuildの特殊文字は項目名として解釈されないようにする。
    std::string MsBuild(std::string_view value)
    {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string result;
        for (const unsigned char ch : value)
        {
            if (ch == '%' || ch == '$' || ch == '@' || ch == '\''
                || ch == ';' || ch == '?' || ch == '*')
            {
                result += '%';
                result += hex[ch >> 4];
                result += hex[ch & 15];
            }
            else result += static_cast<char>(ch);
        }
        return result;
    }

    std::string WindowsPath(const std::filesystem::path& path)
    {
        auto value = LamaPon::PathToUtf8(path);
        std::ranges::replace(value, '/', '\\');
        return value;
    }

    void WriteIfChanged(const std::filesystem::path& path, const std::string& content)
    {
        std::ifstream input(path, std::ios::binary);
        if (input && std::string(std::istreambuf_iterator<char>(input), {}) == content)
            return;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output)
            throw std::runtime_error("Visual Studioプロジェクトを書き込めません: " + LamaPon::PathToUtf8(path));
    }

    std::wstring QuoteArgument(const std::wstring& value)
    {
        std::wstring result = L"\"";
        std::size_t slashes = 0;
        for (const wchar_t ch : value)
        {
            if (ch == L'\\') { ++slashes; continue; }
            result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            result += ch;
        }
        result.append(slashes * 2, L'\\');
        return result + L'"';
    }

    bool Invoke(IDispatch* object, const wchar_t* name, WORD flags,
        VARIANT* arguments, UINT count, VARIANT* result)
    {
        auto mutableName = const_cast<wchar_t*>(name);
        DISPID id{};
        if (FAILED(object->GetIDsOfNames(IID_NULL, &mutableName, 1,
                LOCALE_USER_DEFAULT, &id))) return false;
        DISPPARAMS parameters{ arguments, nullptr, count, 0 };
        return SUCCEEDED(object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
            flags, &parameters, result, nullptr, nullptr));
    }

    Microsoft::WRL::ComPtr<IDispatch> GetDispatchProperty(
        IDispatch* object, const wchar_t* name)
    {
        VARIANT value;
        VariantInit(&value);
        Microsoft::WRL::ComPtr<IDispatch> result;
        if (Invoke(object, name, DISPATCH_PROPERTYGET, nullptr, 0, &value)
            && value.vt == VT_DISPATCH && value.pdispVal != nullptr)
            result = value.pdispVal;
        VariantClear(&value);
        return result;
    }
}

namespace LamaPon
{
    bool IsVisualStudioEditor(const std::filesystem::path& editor)
    {
        auto name = editor.filename().wstring();
        std::ranges::transform(name, name.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return name == L"devenv.exe";
    }

    std::wstring BuildVisualStudioProjectArguments(
        const std::filesystem::path& solution,
        const std::filesystem::path& source)
    {
        // /Commandの引数はWindowsのコマンドライン規則に従って二重に引用する。
        return QuoteArgument(solution.wstring()) + L" /Command "
            + QuoteArgument(L"File.OpenFile \"" + source.wstring() + L"\"");
    }

    bool OpenInRunningVisualStudio(const std::filesystem::path& solution,
        const std::filesystem::path& source)
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
        bool opened = false;
        {
            Microsoft::WRL::ComPtr<IRunningObjectTable> table;
            Microsoft::WRL::ComPtr<IEnumMoniker> enumerator;
            Microsoft::WRL::ComPtr<IBindCtx> context;
            if (SUCCEEDED(GetRunningObjectTable(0, &table))
                && SUCCEEDED(table->EnumRunning(&enumerator))
                && SUCCEEDED(CreateBindCtx(0, &context)))
            {
                Microsoft::WRL::ComPtr<IMoniker> moniker;
                while (!opened && enumerator->Next(1, &moniker, nullptr) == S_OK)
                {
                    LPOLESTR displayName{};
                    const auto displayResult = moniker->GetDisplayName(context.Get(), nullptr, &displayName);
                    const bool isVisualStudio = SUCCEEDED(displayResult) && displayName
                        && std::wstring_view(displayName).starts_with(L"!VisualStudio.DTE.");
                    CoTaskMemFree(displayName);
                    if (isVisualStudio)
                    {
                        Microsoft::WRL::ComPtr<IUnknown> unknown;
                        Microsoft::WRL::ComPtr<IDispatch> dte;
                        if (SUCCEEDED(table->GetObject(moniker.Get(), &unknown))
                            && SUCCEEDED(unknown.As(&dte)))
                        {
                            auto loadedSolution = GetDispatchProperty(dte.Get(), L"Solution");
                            VARIANT fullName;
                            VariantInit(&fullName);
                            if (loadedSolution && Invoke(loadedSolution.Get(), L"FullName", DISPATCH_PROPERTYGET, nullptr, 0, &fullName)
                                && fullName.vt == VT_BSTR && fullName.bstrVal
                                && CompareStringOrdinal(fullName.bstrVal, -1, solution.c_str(), -1, TRUE) == CSTR_EQUAL)
                            {
                                VARIANT args[2];
                                VariantInit(&args[0]);
                                VariantInit(&args[1]);
                                args[0].vt = VT_BSTR;
                                args[0].bstrVal = SysAllocString((L"\"" + source.wstring() + L"\"").c_str());
                                args[1].vt = VT_BSTR;
                                args[1].bstrVal = SysAllocString(L"File.OpenFile");
                                if (args[0].bstrVal && args[1].bstrVal)
                                    opened = Invoke(dte.Get(), L"ExecuteCommand", DISPATCH_METHOD, args, 2, nullptr);
                                VariantClear(&args[0]);
                                VariantClear(&args[1]);
                                if (opened)
                                {
                                    auto window = GetDispatchProperty(dte.Get(), L"MainWindow");
                                    if (window) Invoke(window.Get(), L"Activate", DISPATCH_METHOD, nullptr, 0, nullptr);
                                }
                            }
                            VariantClear(&fullName);
                        }
                    }
                    moniker.Reset();
                }
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        return opened;
    }

    std::filesystem::path UpdateVisualStudioProject(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& engineRoot)
    {
        const auto assetRoot = projectRoot / "assets";
        const auto directory = projectRoot / ".lamapon" / "visual-studio";
        std::filesystem::create_directories(directory);

        struct Item { std::filesystem::path relative; std::string kind; };
        std::vector<Item> items;
        std::set<std::string> filters;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot))
        {
            if (!entry.is_regular_file()) continue;
            auto extension = entry.path().extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            std::string kind;
            if (extension == L".cpp" || extension == L".cxx" || extension == L".cc") kind = "ClCompile";
            else if (extension == L".h" || extension == L".hpp" || extension == L".hxx") kind = "ClInclude";
            else if (extension == L".hlsl" || extension == L".hlsli" || extension == L".fx") kind = "None";
            else continue;
            auto relative = entry.path().lexically_relative(projectRoot);
            for (auto folder = relative.parent_path(); !folder.empty(); folder = folder.parent_path())
                filters.insert(WindowsPath(folder));
            items.push_back({std::move(relative), std::move(kind)});
        }
        std::ranges::sort(items, {}, &Item::relative);

        std::string includes;
        const auto addInclude = [&includes](const std::filesystem::path& path) {
            includes += Xml(MsBuild(WindowsPath(path))) + ";";
        };
        addInclude(engineRoot / "src");
        if (!std::filesystem::exists(engineRoot / "src/LamaPon/Core/Version.h"))
        {
            const auto includeRoot = directory / "include";
            const auto versionDirectory = includeRoot / "LamaPon/Core";
            std::filesystem::create_directories(versionDirectory);
            std::ostringstream version;
            version << "#pragma once\n#include <cstdint>\n#include <string_view>\nnamespace LamaPon {\n"
                << "inline constexpr std::uint32_t VersionMajor = " << VersionMajor << ";\n"
                << "inline constexpr std::uint32_t VersionMinor = " << VersionMinor << ";\n"
                << "inline constexpr std::uint32_t VersionPatch = " << VersionPatch << ";\n"
                << "inline constexpr std::string_view VersionString{\"" << VersionString << "\"};\n}\n";
            WriteIfChanged(versionDirectory / "Version.h", version.str());
            addInclude(includeRoot);
        }
        addInclude(engineRoot / "third_party" / "nlohmann");
        addInclude(engineRoot / "third_party" / "DirectXTK" / "include");
        addInclude(engineRoot / "third_party" / "XAudio2Redist" / "include");
        const auto packages = SelectAvailablePackageNativeDependencies(
            ScanPackageNativeDependencies(assetRoot).packages);
        std::string defines = "NOMINMAX;WIN32_LEAN_AND_MEAN;UNICODE;_UNICODE;_WIN32_WINNT=0x0A00;USING_XAUDIO2_REDIST;";
        for (const auto& package : packages.available)
        {
            for (const auto& path : package.includeDirectories) addInclude(path);
            for (const auto& define : package.defines) defines += Xml(MsBuild(define)) + ";";
        }

        // エディターの自動ビルドと同じ中間ファイルを同時に変更させない。
        constexpr auto command = "echo Build scripts from the LamaPon Editor or CLI &amp; exit /b 1";
        std::ostringstream project;
        project << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            << "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
            << "  <ItemGroup Label=\"ProjectConfigurations\"><ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>\n"
            << "  <PropertyGroup Label=\"Globals\"><ProjectGuid>" << ProjectGuid << "</ProjectGuid><Keyword>MakeFileProj</Keyword><RootNamespace>LamaPonScripts</RootNamespace><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>\n"
            << "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n"
            << "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\" Label=\"Configuration\"><ConfigurationType>Makefile</ConfigurationType><UseDebugLibraries>false</UseDebugLibraries></PropertyGroup>\n"
            << "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n"
            << "  <PropertyGroup><NMakeBuildCommandLine>" << command << "</NMakeBuildCommandLine><NMakeReBuildCommandLine>" << command
            << "</NMakeReBuildCommandLine><NMakeCleanCommandLine>echo LamaPon manages build files</NMakeCleanCommandLine>"
            << "<NMakeIncludeSearchPath>" << includes << "$(IncludePath)</NMakeIncludeSearchPath><NMakePreprocessorDefinitions>" << defines
            << "$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions><AdditionalOptions>/std:c++20</AdditionalOptions></PropertyGroup>\n"
            << "  <ItemGroup>\n";
        for (const auto& item : items)
            project << "    <" << item.kind << " Include=\"" << Xml(MsBuild("..\\..\\" + WindowsPath(item.relative))) << "\" />\n";
        project << "  </ItemGroup>\n  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n</Project>\n";

        std::ostringstream filterFile;
        filterFile << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n  <ItemGroup>\n";
        for (const auto& filter : filters)
            filterFile << "    <Filter Include=\"" << Xml(MsBuild(filter)) << "\" />\n";
        filterFile << "  </ItemGroup>\n  <ItemGroup>\n";
        for (const auto& item : items)
            filterFile << "    <" << item.kind << " Include=\"" << Xml(MsBuild("..\\..\\" + WindowsPath(item.relative))) << "\"><Filter>"
                << Xml(MsBuild(WindowsPath(item.relative.parent_path()))) << "</Filter></" << item.kind << ">\n";
        filterFile << "  </ItemGroup>\n</Project>\n";

        const std::string solution = std::string{
            "Microsoft Visual Studio Solution File, Format Version 12.00\r\n"
            "# Visual Studio Version 17\r\n"
            "Project(\"{BC8A1FFA-BEE3-4634-8014-F334798102B3}\") = \"LamaPonScripts\", \"LamaPonScripts.vcxproj\", \""} + ProjectGuid + "\"\r\nEndProject\r\n"
            "Global\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n\t\tRelease|x64 = Release|x64\r\n\tEndGlobalSection\r\n"
            "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n\t\t" + ProjectGuid + ".Release|x64.ActiveCfg = Release|x64\r\n\t\t" + ProjectGuid + ".Release|x64.Build.0 = Release|x64\r\n\tEndGlobalSection\r\nEndGlobal\r\n";

        WriteIfChanged(directory / "LamaPonScripts.vcxproj", project.str());
        WriteIfChanged(directory / "LamaPonScripts.vcxproj.filters", filterFile.str());
        WriteIfChanged(directory / "LamaPonScripts.sln", solution);
        return directory / "LamaPonScripts.sln";
    }
}
