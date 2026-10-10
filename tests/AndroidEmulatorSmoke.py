"""Exercise an Android native game on the 16 KB CI emulator."""
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_android_screenshot import count_green_marker

PACKAGE = "com.lamapon.game.android_game_x86_64"
EMULATOR_LAUNCHER = "com.google.android.apps.nexuslauncher"
ACTIVITY = PACKAGE + "/com.lamapon.runtime.GameActivity"
APK = ROOT / "test-output/platform-core/android-apk-build-x86_64/app/outputs/apk/debug/app-debug.apk"
INITIAL_DISPLAY_SIZE = "720x1560"
RESIZED_DISPLAY_SIZE = "600x1300"
SCREENSHOT_WAIT_SECONDS = 90
DIAGNOSTICS = ROOT / "test-output/platform-core/android-smoke-diagnostics"


def adb(*arguments, check=True):
    result = subprocess.run(["adb", *arguments], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
    if check and result.returncode:
        raise RuntimeError("adb failed: " + result.stdout + result.stderr)
    return result


def saved_values():
    result = adb("shell", "run-as", PACKAGE, "cat", "files/values.json", check=False)
    if result.returncode:
        return None
    try:
        return json.loads(result.stdout)
    except (ValueError, TypeError):
        return None


def saved_start_count():
    values = saved_values()
    if values is None:
        return None
    try:
        return int(values.get("native-probe-starts", "0"))
    except (ValueError, TypeError):
        return None


def saved_button_clicks():
    values = saved_values()
    if values is None:
        return None, None
    try:
        return (int(values.get("native-button-clicks", "0")),
                values.get("native-button-sender"))
    except (ValueError, TypeError):
        return None, None


def wait_for_start_count(minimum):
    for _ in range(45):
        count = saved_start_count()
        if count is not None and count >= minimum:
            return count
        time.sleep(1)
    raise RuntimeError("Android game did not persist its startup count; "
                       + startup_diagnostics())


def wait_for_resume(initial_count, initial_pid):
    count = None
    pid = ""
    for _ in range(45):
        count = saved_start_count()
        pid = game_pid()
        if pid == initial_pid or (count is not None and count != initial_count):
            return count, pid
        time.sleep(1)
    return count, pid


def game_pid():
    return adb("shell", "pidof", PACKAGE, check=False).stdout.strip()


def wait_for_game_pid():
    pid = ""
    for _ in range(30):
        pid = game_pid()
        if pid:
            return pid
        time.sleep(1)
    return pid


def startup_diagnostics():
    activity_result = adb("shell", "dumpsys", "activity", "activities",
                          check=False)
    activity_lines = (activity_result.stdout + activity_result.stderr).splitlines()
    activity = " | ".join(line.strip() for line in activity_lines
                          if PACKAGE in line or "mResumedActivity" in line
                          or "topResumedActivity" in line)
    # Filter after reading: emulator noise can otherwise hide the startup logs.
    log_result = adb("logcat", "-d", check=False)
    log_lines = (log_result.stdout + log_result.stderr).splitlines()
    markers = ("lamapon", "sdl", "androidruntime", "fatal signal", "linker",
               "crash", "anr", "libgame")
    relevant_logs = [line.strip() for line in log_lines
                     if any(marker in line.lower() for marker in markers)]
    devices = adb("devices", check=False)
    return "pid={!r}, activity={!r}, logcat={!r}, adb={!r}".format(
        game_pid(), activity, relevant_logs[-80:],
        (devices.stdout + devices.stderr).strip())


def collect_failure_diagnostics():
    """Capture before emulator-runner shuts down, preserving the original failure."""
    DIAGNOSTICS.mkdir(parents=True, exist_ok=True)
    commands = {
        "logcat.txt": ("logcat", "-d"),
        "activity.txt": ("shell", "dumpsys", "activity", "activities"),
        "window.txt": ("shell", "dumpsys", "window"),
        "power.txt": ("shell", "dumpsys", "power"),
        "surfaceflinger.txt": ("shell", "dumpsys", "SurfaceFlinger"),
        "saves.txt": ("shell", "run-as", PACKAGE, "cat", "files/values.json"),
    }
    for filename, arguments in commands.items():
        try:
            result = adb(*arguments, check=False)
            (DIAGNOSTICS / filename).write_text(result.stdout + result.stderr, encoding="utf-8")
        except Exception as error:
            print("Android diagnostic {} failed: {}".format(filename, error), file=sys.stderr)
    try:
        capture = subprocess.run(["adb", "exec-out", "screencap", "-p"],
                                 capture_output=True, timeout=10)
        if capture.stdout:
            (DIAGNOSTICS / "failure.png").write_bytes(capture.stdout)
    except Exception as error:
        print("Android failure screenshot could not be captured: " + str(error), file=sys.stderr)


def prepare_headless_emulator():
    # A headless AVD can report boot completion before Android finishes its first-run setup.
    # ActivityTaskManager then skips the game task and leaves the launcher in the foreground.
    adb("shell", "settings", "put", "global", "device_provisioned", "1")
    adb("shell", "settings", "put", "secure", "user_setup_complete", "1")
    device_provisioned = adb(
        "shell", "settings", "get", "global", "device_provisioned").stdout.strip()
    user_setup_complete = adb(
        "shell", "settings", "get", "secure", "user_setup_complete").stdout.strip()
    if device_provisioned != "1" or user_setup_complete != "1":
        raise RuntimeError(
            "Android emulator setup did not complete: device_provisioned={!r}, "
            "user_setup_complete={!r}; {}".format(
                device_provisioned, user_setup_complete, startup_diagnostics()))
    # A resumed Activity alone does not establish that the display is awake/unlocked.
    adb("shell", "svc", "power", "stayon", "true")
    adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
    adb("shell", "wm", "dismiss-keyguard")


def dismiss_emulator_launcher_anr():
    # Cold boot on the software-rendered CI device can leave a launcher ANR dialog
    # above the resumed game. Close only that process; never dismiss a game ANR.
    windows = adb("shell", "dumpsys", "window", "windows").stdout.splitlines()
    if any("mCurrentFocus=" in line
           and "Application Not Responding: " + EMULATOR_LAUNCHER + "}" in line
           for line in windows):
        adb("shell", "am", "force-stop", EMULATOR_LAUNCHER)
        print("Closed the emulator Pixel Launcher ANR dialog; game process left running.")
        return True
    return False


def screenshot_render():
    screenshot = subprocess.run(["adb", "exec-out", "screencap", "-p"],
                                capture_output=True, timeout=10)
    if screenshot.returncode:
        raise RuntimeError("adb screencap failed: "
                           + screenshot.stderr.decode("utf-8", errors="replace"))
    try:
        rendered = count_green_marker(screenshot.stdout)
    except ValueError:
        rendered = {"greenMarkerPixels": 0}
    return screenshot.stdout, rendered


def wait_for_scene_screenshot(stage, dimensions_different_from=None):
    started = time.monotonic()
    deadline = started + SCREENSHOT_WAIT_SECONDS
    attempts = 0
    last_capture_seconds = 0.0
    last_capture_error = None
    screenshot = b""
    rendered = {"greenMarkerPixels": 0}
    dimensions = None
    while time.monotonic() < deadline:
        attempts += 1
        capture_started = time.monotonic()
        try:
            dismiss_emulator_launcher_anr()
            screenshot, rendered = screenshot_render()
            last_capture_error = None
        except subprocess.TimeoutExpired:
            screenshot = b""
            rendered = {"greenMarkerPixels": 0}
            last_capture_error = "adb screencap exceeded 10 seconds"
        last_capture_seconds = time.monotonic() - capture_started
        if len(screenshot) >= 24:
            dimensions = struct.unpack(">II", screenshot[16:24])
            dimensions_match = (dimensions_different_from is None
                                or dimensions != dimensions_different_from)
            if rendered["greenMarkerPixels"] >= 1000 and dimensions_match:
                elapsed = time.monotonic() - started
                print("Android {} scene screenshot passed after {} capture(s) in {:.1f}s: {}".format(
                    stage, attempts, elapsed, json.dumps(rendered)))
                return screenshot, rendered
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(1, remaining))

    elapsed = time.monotonic() - started
    raise RuntimeError(
        "Android screenshot did not contain the exported scene green UI marker "
        "during {}: {}; attempts={}, elapsedSeconds={:.1f}, "
        "lastCaptureSeconds={:.1f}, dimensions={}, lastCaptureError={!r}; {}".format(
            stage, json.dumps(rendered), attempts, elapsed,
            last_capture_seconds, dimensions, last_capture_error,
            startup_diagnostics()))


def run_smoke():
    page_size = adb("shell", "getconf", "PAGE_SIZE").stdout.strip()
    assert page_size == "16384", "Expected a 16 KB emulator, got " + page_size
    assert APK.is_file(), "The generated x86_64 APK is missing"
    prepare_headless_emulator()
    adb("shell", "wm", "size", INITIAL_DISPLAY_SIZE)
    adb("install", "-r", str(APK))
    adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    initial_count = wait_for_start_count(1)
    assert initial_count == 1, "Fresh install did not start from a clean save"
    initial_pid = wait_for_game_pid()
    if not initial_pid:
        raise RuntimeError("Android game process did not remain available after startup; "
                           + startup_diagnostics())

    screenshot, rendered = wait_for_scene_screenshot("startup")

    screen_width, screen_height = struct.unpack(">II", screenshot[16:24])
    adb("shell", "input", "tap", str(screen_width // 2), str(screen_height // 2))
    for _ in range(45):
        click_count, click_sender = saved_button_clicks()
        if click_count == 1:
            break
        time.sleep(1)
    if click_count != 1 or click_sender != "Button":
        values = saved_values() or {}
        raise AssertionError(
            "Android touchscreen did not dispatch one UIButton clickEvent with its sender; "
            "clicks={!r}, sender={!r}, pointer={!r}".format(
                click_count, click_sender,
                values.get("native-pointer-diagnostic")))

    adb("shell", "input", "keyevent", "KEYCODE_HOME")
    time.sleep(2)
    adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    resumed_count, resumed_pid = wait_for_resume(initial_count, initial_pid)
    if resumed_count != initial_count or resumed_pid != initial_pid:
        raise AssertionError(
            "Android changed game state during foreground resume; "
            "count={!r}->{!r}, pid={!r}->{!r}; {}".format(
                initial_count, resumed_count, initial_pid, resumed_pid,
                startup_diagnostics()))

    try:
        adb("shell", "wm", "size", RESIZED_DISPLAY_SIZE)
        resized_screenshot, resized_render = wait_for_scene_screenshot(
            "display resize", (screen_width, screen_height))
        resized_width, resized_height = struct.unpack(">II", resized_screenshot[16:24])
        resized_pid = wait_for_game_pid()
        if resized_pid != initial_pid:
            raise AssertionError(
                "Display resize changed the Android game process; "
                "pid={!r}->{!r}; {}".format(
                    initial_pid, resized_pid, startup_diagnostics()))
        assert saved_start_count() == resumed_count, "Display resize restarted the game scene"
    finally:
        adb("shell", "wm", "size", INITIAL_DISPLAY_SIZE, check=False)

    adb("shell", "am", "force-stop", PACKAGE)
    adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    restarted_count = wait_for_start_count(resumed_count + 1)
    assert saved_start_count() == restarted_count
    assert saved_button_clicks() == (1, "Button"), (
        "Touch click event did not persist across game restart")
    print(f"Android game started on a {page_size}-byte page-size emulator; touchscreen UIButton event, "
          f"same-process activity resume, display resize and save restart passed "
          f"({resumed_count} -> {restarted_count}).")


def main():
    try:
        run_smoke()
    except Exception:
        try:
            collect_failure_diagnostics()
        except Exception as error:
            print("Android failure diagnostics could not be saved: " + str(error), file=sys.stderr)
        raise
    finally:
        adb("shell", "wm", "size", "reset", check=False)


if __name__ == "__main__":
    main()
