"""Android source configuration for the Portable SDL runtime; never downloads or builds SDKs."""
from pathlib import Path, PurePosixPath
import re
from xml.sax.saxutils import escape

from export_web import ExportError

ANDROID_NDK_VERSION = "30.0.16248370"
ANDROID_CMAKE_VERSION = "3.31.6"
ANDROID_BUILD_TOOLS_VERSION = "36.0.0"
ANDROID_COMPILE_SDK_VERSION = 36
ANDROID_TARGET_SDK_VERSION = 36
ANDROID_MIN_GRADLE_VERSION = (9, 6, 0)


def groovy_string(value: str | Path) -> str:
    text = value.as_posix() if isinstance(value, Path) else value
    if not isinstance(text, str) or any(c in text for c in ("\0", "\n", "\r")):
        raise ExportError("Android build settings cannot contain control characters")
    return "'" + text.replace("\\", "\\\\").replace("'", "\\'") + "'"


def android_settings(context: dict) -> dict:
    settings = context["project"].get("export", {}).get("native", {}).get("android", {})
    if not isinstance(settings, dict):
        raise ExportError("export.native.android must be an object")
    default_suffix = re.sub(r"[^a-z0-9_]", "_", context["root"].name.lower()) or "game"
    if not default_suffix[0].isalpha():
        default_suffix = "g_" + default_suffix
    result = {"applicationId": "com.lamapon.game." + default_suffix,
              "compileSdk": ANDROID_COMPILE_SDK_VERSION,
              "targetSdk": ANDROID_TARGET_SDK_VERSION, "minSdk": 26,
              "versionCode": 1, "versionName": "1.0", "abis": ["arm64-v8a", "x86_64"]}
    unknown = set(settings) - set(result)
    if unknown:
        raise ExportError("Unknown Android settings: " + ", ".join(sorted(unknown)))
    result.update(settings)
    if not isinstance(result["applicationId"], str) or not re.fullmatch(
            r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", result["applicationId"]):
        raise ExportError("Android applicationId must contain at least two valid identifier segments")
    for key in ("compileSdk", "targetSdk", "minSdk", "versionCode"):
        if isinstance(result[key], bool) or not isinstance(result[key], int):
            raise ExportError(f"Android {key} must be an integer")
    if not 26 <= result["minSdk"] <= result["targetSdk"] <= result["compileSdk"] <= 37:
        raise ExportError("Android requires 26 <= minSdk <= targetSdk <= compileSdk <= 37")
    if not 1 <= result["versionCode"] <= 2100000000:
        raise ExportError("Android versionCode is out of range")
    if not isinstance(result["versionName"], str) or not result["versionName"]:
        raise ExportError("Android versionName must be a nonempty string")
    groovy_string(result["versionName"])
    abis = result["abis"]
    if not isinstance(abis, list) or not abis or any(abi not in ("arm64-v8a", "x86_64") for abi in abis) \
            or len(set(abis)) != len(abis):
        raise ExportError("Android abis must contain unique arm64-v8a and/or x86_64 entries")
    return result


def project_files(context: dict, engine_root: Path, target: str, name: str) -> dict[str, str]:
    config = android_settings(context)
    q = groovy_string
    if not isinstance(name, str) or any(
            not (character in "\t\n\r" or "\x20" <= character <= "\ud7ff"
                 or "\ue000" <= character <= "\ufffd"
                 or "\U00010000" <= character <= "\U0010ffff") for character in name):
        raise ExportError("Android game name contains a character that XML 1.0 cannot represent")
    include_patterns = []
    for included in context["included"]:
        relative = included.relative_to(context["asset_root"]).as_posix()
        include_patterns.append("**" if relative == "." else relative + "/**" if included.is_dir() else relative)
    patterns = ", ".join(q(pattern) for pattern in include_patterns)
    abis = ", ".join(q(abi) for abi in config["abis"])
    package_license_staging = []
    staged_package_licenses = set()
    for abi in config["abis"]:
        for dependency in context.get("package_dependencies", {}).get("android-" + abi, []):
            for license_file in dependency["licenseFiles"]:
                license_key = (license_file["source"], license_file["destination"])
                if license_key in staged_package_licenses:
                    continue
                staged_package_licenses.add(license_key)
                destination = PurePosixPath(license_file["destination"])
                destination_parent = destination.parent.as_posix()
                destination_name = destination.name
                package_license_staging.append(
                    f"    from(new File(gameRoot, {q(license_file['source'])})) "
                    f"{{ into {q('licenses/' + destination_parent)}; rename {{ {q(destination_name)} }} }}")
    package_license_staging = "\n".join(package_license_staging)
    # app_name is XML text, not a Groovy string; preserve display characters and
    # escape only the XML delimiters handled by saxutils.
    label = escape(name)
    app_gradle = f"""plugins {{ id 'com.android.application' }}
def nativeBuildRoot = providers.gradleProperty('lamaponBuildRoot')
if (nativeBuildRoot.isPresent()) {{
    layout.buildDirectory.set(new File(nativeBuildRoot.get(), 'app'))
}}
def engineRoot = file(providers.gradleProperty('lamaponEngineRoot').getOrElse({q(engine_root)}))
def gameRoot = file(providers.gradleProperty('lamaponProjectRoot').getOrElse({q(context['root'])}))
def sdlRoot = file(providers.gradleProperty('lamaponSdlRoot').getOrElse(''))
if (!providers.gradleProperty('lamaponSdlRoot').isPresent() ||
    !new File(sdlRoot, 'android-project/app/src/main/java/org/libsdl/app/SDLActivity.java').isFile()) {{
    throw new GradleException('Pass -PlamaponSdlRoot=<existing matching SDL 3.4+ sources>')
}}
android {{
    namespace 'com.lamapon.runtime'
    compileSdk {config['compileSdk']}
    buildToolsVersion '{ANDROID_BUILD_TOOLS_VERSION}'
    ndkVersion '{ANDROID_NDK_VERSION}'
    defaultConfig {{
        applicationId {q(config['applicationId'])}
        minSdk {config['minSdk']}
        targetSdk {config['targetSdk']}
        versionCode {config['versionCode']}
        versionName {q(config['versionName'])}
        ndk {{ abiFilters {abis} }}
        externalNativeBuild {{ cmake {{
            arguments '-DANDROID_STL=c++_shared', '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON',
            '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384',
            '-DLAMAPON_ENGINE_ROOT=' + engineRoot.absolutePath,
            '-DLAMAPON_PROJECT_ROOT=' + gameRoot.absolutePath,
            '-DLAMAPON_SDL_SOURCE_DIRECTORY=' + sdlRoot.absolutePath,
            '-DLAMAPON_ANDROID_PACKAGE_LIBS_DIRECTORY=' + layout.buildDirectory.dir('lamaponRuntimeLibs').get().asFile.absolutePath
            targets {q(target)}, 'SDL3-shared'
        }} }}
    }}
    externalNativeBuild {{ cmake {{
        path file('../../CMakeLists.txt'); version '{ANDROID_CMAKE_VERSION}'
        if (nativeBuildRoot.isPresent()) {{ buildStagingDirectory new File(nativeBuildRoot.get(), 'native') }}
    }} }}
    sourceSets {{ main {{
        java.srcDir new File(sdlRoot, 'android-project/app/src/main/java')
        assets.srcDir layout.buildDirectory.dir('lamaponAssets').get().asFile
        jniLibs.srcDir layout.buildDirectory.dir('lamaponRuntimeLibs').get().asFile
    }} }}
    compileOptions {{ sourceCompatibility JavaVersion.VERSION_17; targetCompatibility JavaVersion.VERSION_17 }}
    packaging {{ jniLibs {{ useLegacyPackaging = false }} }}
    buildTypes {{ release {{ minifyEnabled false }} }}
}}
def stageAssets = tasks.register('stageLamaPonAssets', Sync) {{
    duplicatesStrategy = DuplicatesStrategy.FAIL
    into layout.buildDirectory.dir('lamaponAssets')
    def engineLicense = new File(engineRoot, 'LICENSE')
    if (!engineLicense.isFile()) {{ engineLicense = new File(engineRoot, 'licenses/LamaPon.txt') }}
    def ndkRoot = androidComponents.sdkComponents.ndkDirectory
    doFirst {{
        if (!engineLicense.isFile() || !new File(ndkRoot.get().asFile, 'NOTICE').isFile() ||
            !new File(ndkRoot.get().asFile, 'NOTICE.toolchain').isFile()) {{
            throw new GradleException('Engine and NDK notices are required for native game output')
        }}
    }}
    from(engineLicense) {{ into 'licenses'; rename {{ 'LamaPon.txt' }} }}
    from(ndkRoot.map {{ it.file('NOTICE') }}) {{ into 'licenses'; rename {{ 'AndroidNDK.txt' }} }}
    from(ndkRoot.map {{ it.file('NOTICE.toolchain') }}) {{ into 'licenses'; rename {{ 'AndroidNDK-toolchain.txt' }} }}
    from(new File(gameRoot, {q(context['asset_root'].relative_to(context['root']))})) {{
        into 'assets'; include {patterns}
    }}
    from(rootProject.file('../lamapon-input-actions.json')) {{ into 'assets' }}
    from(new File(engineRoot, 'third_party/imgui/misc/fonts/ProggyClean.ttf')) {{
        into 'assets'; rename {{ 'lamapon-default-font.ttf' }}
    }}
    from(new File(engineRoot, 'third_party/imgui/misc/fonts/ProggyClean.LICENSE.txt')) {{ into 'licenses' }}
    from(new File(sdlRoot, 'LICENSE.txt')) {{ into 'licenses'; rename {{ 'SDL3.txt' }} }}
    from(new File(engineRoot, 'third_party/nlohmann/LICENSE.MIT')) {{ into 'licenses'; rename {{ 'nlohmann.txt' }} }}
    from(new File(engineRoot, 'third_party/cgltf/LICENSE.txt')) {{ into 'licenses'; rename {{ 'cgltf.txt' }} }}
    from(new File(engineRoot, 'third_party/stb/LICENSE.txt')) {{ into 'licenses'; rename {{ 'stb.txt' }} }}
    from(new File(engineRoot, 'third_party/imgui/LICENSE.txt')) {{ into 'licenses'; rename {{ 'imgui.txt' }} }}
{package_license_staging}
}}
tasks.named('preBuild').configure {{ dependsOn stageAssets }}
"""
    manifest = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
  <uses-feature android:glEsVersion="0x00030000" android:required="true" />
  <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
  <uses-feature android:name="android.hardware.gamepad" android:required="false" />
  <application android:label="@string/app_name" android:theme="@android:style/Theme.Material.NoActionBar.Fullscreen"
      android:allowBackup="false" android:hardwareAccelerated="true">
    <activity android:name="com.lamapon.runtime.GameActivity" android:exported="true"
        android:configChanges="layoutDirection|locale|grammaticalGender|fontScale|fontWeightAdjustment|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation">
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
    </activity>
  </application>
</manifest>
"""
    activity = """package com.lamapon.runtime;

import org.libsdl.app.SDLActivity;

public final class GameActivity extends SDLActivity {
    @Override
    protected String[] getArguments() {
        return new String[] {
            "--data-dir", getFilesDir().getAbsolutePath(),
            "--cache-dir", getCacheDir().getAbsolutePath()
        };
    }
}
"""
    return {
        "android/settings.gradle": "pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }\n"
                                   "dependencyResolutionManagement { repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS); "
                                   "repositories { google(); mavenCentral() } }\nrootProject.name = 'LamaPonGame'\ninclude ':app'\n",
        "android/build.gradle": "plugins { id 'com.android.application' version '9.4.0' apply false }\n",
        "android/gradle.properties": "org.gradle.jvmargs=-Xmx2048m -Dfile.encoding=UTF-8\n"
                                     "android.builder.sdkDownload=false\n",
        "android/app/build.gradle": app_gradle,
        "android/app/src/main/AndroidManifest.xml": manifest,
        "android/app/src/main/res/values/strings.xml": f'<resources><string name="app_name">{label}</string></resources>\n',
        "android/app/src/main/java/com/lamapon/runtime/GameActivity.java": activity,
        "android/README.txt": "Android Gradle configuration is not yet build/device verified.\n"
                              "Use existing Gradle 9.6.0+, JDK 17, Android SDK and matching SDL 3.4+ sources.\n"
                              "gradle --no-daemon -PlamaponSdlRoot=<SDL sources> :app:assembleDebug\n"
                              "Debug APK: app/build/outputs/apk/debug/app-debug.apk\n"
                              "Release APK: :app:assembleRelease (unsigned); store/release signing is not configured.\n"
                              "Gradle may retrieve plugin artifacts when building; automatic SDK retrieval is disabled.\n"
                              "Authorize those downloads and build/cache locations before running in a restricted workspace.\n"
                              "No Gradle wrapper, engine clone, SDK download or APK is created by this generator.\n",
    }
