#include "LamaPon/Editor/WebExportJob.h"
#include "LamaPon/Core/PathUtils.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    { if (!condition) throw std::runtime_error(message); }
    void Wait(LamaPon::WebExportJob& job)
    {
        const auto started = GetTickCount64();
        while (!job.Poll())
        {
            if (GetTickCount64() - started > 15000) throw std::runtime_error("Export process timed out");
            Sleep(10);
        }
    }
}

// Caller supplies an existing, empty, authorized directory. No folder is
// created elsewhere, and the caller retains control over cleanup.
int wmain(int argc, wchar_t** argv)
{
    try
    {
        Require(argc == 3, "Expected Python executable and authorized empty test directory");
        const auto root = std::filesystem::absolute(argv[2]);
        Require(std::filesystem::is_directory(root) && std::filesystem::is_empty(root),
            "Test directory must already exist and be empty");
        const auto engine = root / L"engine 日本語 & paths";
        std::filesystem::create_directories(engine / L"tools");
        const auto project = root / L"project 日本語 & paths" / L"project.json";
        std::filesystem::create_directories(project.parent_path());
        std::ofstream(project) << "{}";
        std::ofstream(engine / L"tools" / L"editor_native_export.py") << R"PY(
import argparse,json
from pathlib import Path
p=argparse.ArgumentParser()
for flag in ('project','output','result','platform'): p.add_argument('--'+flag,required=True)
p.add_argument('--build-apk',action='store_true')
p.add_argument('--allow-downloads',action='store_true')
for flag in ('android-sdk','java-home','gradle-home','sdl-source-directory'): p.add_argument('--'+flag)
a=p.parse_args()
output=Path(a.output)
output.mkdir(parents=True,exist_ok=True)
(output/'CMakeLists.txt').write_text('project(Probe)',encoding='utf-8')
(output/'native-inspection.json').write_text('{}',encoding='utf-8')
platform='android' if Path(a.project).name=='mismatch.json' else a.platform
result={'ok':True,'message':'generated','platform':platform,'buildProjectPath':str(output)}
if a.build_apk:
    # Process-boundary fixture only: no real Android build or signing occurs.
    for value in (a.android_sdk,a.java_home,a.gradle_home,a.sdl_source_directory):
        assert Path(value).resolve()==Path(__file__).resolve().parents[1]
    assert a.allow_downloads
    mode=Path(a.project).stem
    apk=output/'build/app/outputs/apk/debug/app-debug.apk'
    if mode!='missing-apk':
        apk.parent.mkdir(parents=True,exist_ok=True)
        apk.write_bytes(b'process fixture, not a real APK')
    result.update(built=True,artifactChecksPassed=mode!='unchecked-apk',artifactPath=str(apk))
Path(a.result).write_text(json.dumps(result),encoding='utf-8')
)PY";
        std::ofstream(engine / L"tools" / L"editor_linux_export.py") << R"PY(
import argparse,json
from pathlib import Path
p=argparse.ArgumentParser()
for flag in ('project','output','result','engine-root','sdl-source-directory','platform'): p.add_argument('--'+flag,required=True)
p.add_argument('--distribution',default='')
a=p.parse_args()
assert a.platform=='linux'
assert Path(a.engine_root).resolve()==Path(__file__).resolve().parents[1]
assert Path(a.sdl_source_directory).resolve()==Path(__file__).resolve().parents[1]
output=Path(a.output)
output.mkdir(parents=True,exist_ok=True)
(output/'CMakeLists.txt').write_text('project(Probe)',encoding='utf-8')
(output/'native-inspection.json').write_text('{}',encoding='utf-8')
build=output/'build'; build.mkdir()
artifact=build/'LamaPonLinuxProbe'
mode=Path(a.project).stem
if mode=='escaped-linux-artifact': artifact=output.parent/'escaped-Linux-probe'
if mode!='missing-linux-artifact': artifact.write_bytes(b'\x7fELFprocess fixture, not a real Linux game')
result={'ok':True,'platform':'linux','built':True,'artifactChecksPassed':mode!='unchecked-linux-artifact',
    'buildProjectPath':str(output),'artifactPath':str(artifact),
    'message':'Linux package inspected; execution not verified'}
Path(a.result).write_text(json.dumps(result),encoding='utf-8')
)PY";
        std::ofstream(engine / L"tools" / L"editor_web_export.py") << R"PY(
import argparse,json
from pathlib import Path
p=argparse.ArgumentParser()
for flag in ('project','output','result'): p.add_argument('--'+flag,required=True)
p.add_argument('--emsdk')
a=p.parse_args()
output=Path(a.output)
output.mkdir(parents=True,exist_ok=True)
page=output/'game.html'
page.write_text('<canvas></canvas>',encoding='utf-8')
Path(a.result).write_text(json.dumps({'ok':True,'message':'html generated','htmlPath':str(page)}),encoding='utf-8')
)PY";
        const LamaPon::WebExportTools tools{argv[1], {}};
        LamaPon::WebExportJob job;
        for (const auto platform : {"linux", "android"})
        {
            const auto output = root / LamaPon::PathFromUtf8(platform) / L"output 日本語 & paths";
            job.StartNativeBuildProject(engine, project, output, tools, platform);
            Require(job.Running(), "Native export must remain asynchronous");
            Wait(job);
            Require(job.Succeeded() && job.BuildProjectDirectory() == output && job.HtmlPath().empty(),
                "Native target and Unicode paths did not survive the process boundary");
            Require(!job.Poll(), "Completion must be delivered once");
        }
        job.StartNativeBuildProject(engine, project.parent_path() / L"mismatch.json",
            root / L"mismatch", tools, "linux");
        Wait(job);
        Require(!job.Succeeded(), "Mismatched target OS must not succeed");
        bool rejected{};
        try { job.StartNativeBuildProject(engine, project, root / L"invalid", tools, "macos"); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected && !job.Running(), "Unsupported target must be rejected before process creation");
        const auto webOutput = root / L"web output 日本語 & paths";
        job.Start(engine, project, webOutput, tools);
        Wait(job);
        Require(job.Succeeded() && job.HtmlPath() == webOutput / L"game.html"
            && job.BuildProjectDirectory().empty(), "Web restart retained native result state");
        job.StartNativeBuildProject(engine, project, root / L"native after web", tools, "linux");
        Wait(job);
        Require(job.Succeeded() && job.HtmlPath().empty(), "Native restart retained Web result state");
        const LamaPon::LinuxExportTools linux{"Ubuntu-24.04", engine};
        const auto linuxOutput = root / L"Linux package 日本語 & paths";
        job.StartLinuxBuild(engine, project, linuxOutput, tools, linux);
        Require(job.Running(), "Linux WSL build must remain asynchronous");
        Wait(job);
        Require(job.Succeeded() && job.BuildProjectDirectory() == linuxOutput
            && job.NativeArtifactPath() == linuxOutput / L"build/LamaPonLinuxProbe"
            && job.HtmlPath().empty(), "Linux executable and Unicode output path were not verified");
        for (const auto name : {L"missing-linux-artifact", L"unchecked-linux-artifact", L"escaped-linux-artifact"})
        {
            job.StartLinuxBuild(engine, project.parent_path() / (std::wstring(name) + L".json"), root / name, tools, linux);
            Wait(job);
            Require(!job.Succeeded() && job.NativeArtifactPath().empty(), "Missing or unchecked Linux package must not succeed");
        }
        const LamaPon::AndroidExportTools android{engine, engine, engine, engine, true};
        const auto apkOutput = root / L"APK output 日本語 & paths";
        job.StartAndroidApk(engine, project, apkOutput, tools, android);
        Require(job.Running(), "APK build must remain asynchronous");
        Wait(job);
        Require(job.Succeeded() && job.NativeArtifactPath() == apkOutput / L"build/app/outputs/apk/debug/app-debug.apk"
            && job.HtmlPath().empty(), "APK result and build tool arguments were not preserved");
        const auto relativeEngine = std::filesystem::relative(engine);
        const LamaPon::AndroidExportTools relativeAndroid{relativeEngine, relativeEngine, relativeEngine, relativeEngine, true};
        const auto relativeOutput = std::filesystem::relative(root / L"relative APK output");
        job.StartAndroidApk(engine, std::filesystem::relative(project), relativeOutput, tools, relativeAndroid);
        Wait(job);
        Require(job.Succeeded() && job.NativeArtifactPath() == std::filesystem::absolute(relativeOutput)
            / L"build/app/outputs/apk/debug/app-debug.apk", "Relative project, APK and SDK paths changed with the child working directory");
        for (const auto name : {L"missing-apk", L"unchecked-apk"})
        {
            job.StartAndroidApk(engine, project.parent_path() / (std::wstring(name) + L".json"), root / name, tools, android);
            Wait(job);
            Require(!job.Succeeded() && job.NativeArtifactPath().empty(), "Missing or unchecked APK must not succeed");
        }
        job.StartNativeBuildProject(engine, project, root / L"settings after APK", tools, "android");
        Wait(job);
        Require(job.Succeeded() && job.NativeArtifactPath().empty(), "Settings generation retained an APK result");
        std::cout << "Native editor process boundary tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
