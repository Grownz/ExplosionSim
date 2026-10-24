#include "app/App.h"
#include "core/Log.h"

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

using ex::LogError;
using ex::LogInfo;

struct ParsedArguments {
    LaunchOptions options{};
    bool wantsHelp = false;
    bool valid = true;
    std::string error;
};

void PrintUsage() {
    LogInfo("Explosion Simulator - physically based 3D blast visualisation");
    LogInfo("");
    LogInfo("Usage: ExplosionSim.exe [options]");
    LogInfo("");
    LogInfo("  --screenshot <path>   Render a deterministic PNG and exit (headless).");
    LogInfo("  --time <seconds>      Simulation time of the screenshot (default 0.12).");
    LogInfo("  --width <pixels>      Output width (default 1600).");
    LogInfo("  --height <pixels>     Output height (default 900).");
    LogInfo("  --charge <kg>         TNT equivalent mass in kilograms.");
    LogInfo("  --hob <metres>        Height of burst above ground.");
    LogInfo("  --azimuth <degrees>   Camera azimuth for headless captures.");
    LogInfo("  --elevation <degrees> Camera elevation for headless captures.");
    LogInfo("  --distance-scale <x>  Multiplier for the framing distance.");
    LogInfo("  --benchmark           Render frames offscreen and print timing.");
    LogInfo("  --frames <count>      Number of benchmark frames (default 120).");
    LogInfo("  --high-fidelity       Use the GPU Euler solver for the volume.");
    LogInfo("  --solver-resolution <n>  Solver grid resolution (64, 96 or 128).");
    LogInfo("  --solver-steps <n>    Solver steps per frame (default 24).");
    LogInfo("  --solver-time <s>     Solver time for headless captures.");
    LogInfo("  --volume-resolution <n>  Volume grid (64, 96, 128, 160, 192, 256, 512, 1024).");
    LogInfo("  --no-vsync            Disable vertical synchronisation.");
    LogInfo("  --debug               Enable the D3D11 debug layer.");
    LogInfo("  --help                Show this help.");
}

bool NeedsValue(int index, int count) {
    return index + 1 < count;
}

ParsedArguments ParseCommandLine() {
    ParsedArguments result;

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) {
        result.valid = false;
        result.error = "CommandLineToArgvW failed.";
        return result;
    }

    auto next = [&](int& index) -> const wchar_t* {
        if (!NeedsValue(index, argumentCount)) {
            result.valid = false;
            result.error = "Missing value for command line option.";
            return L"";
        }
        return arguments[++index];
    };

    for (int i = 1; i < argumentCount && result.valid; ++i) {
        const std::wstring argument = arguments[i];
        auto parseDouble = [&](double& target) {
            const wchar_t* value = next(i);
            wchar_t* end = nullptr;
            const double parsed = std::wcstod(value, &end);
            if (end == value) {
                result.valid = false;
                result.error = "Invalid numeric value.";
                return;
            }
            target = parsed;
        };
        auto parseInt = [&](int& target) {
            const wchar_t* value = next(i);
            wchar_t* end = nullptr;
            const long parsed = std::wcstol(value, &end, 10);
            if (end == value) {
                result.valid = false;
                result.error = "Invalid integer value.";
                return;
            }
            target = int(parsed);
        };
        auto parseFloat = [&](float& target) {
            double parsed = double(target);
            parseDouble(parsed);
            target = float(parsed);
        };

        if (argument == L"--help" || argument == L"-h") {
            result.wantsHelp = true;
        } else if (argument == L"--screenshot") {
            result.options.screenshotPath = next(i);
            result.options.headless = true;
        } else if (argument == L"--time") {
            parseDouble(result.options.captureTime);
        } else if (argument == L"--width") {
            parseInt(result.options.width);
        } else if (argument == L"--height") {
            parseInt(result.options.height);
        } else if (argument == L"--charge") {
            parseDouble(result.options.chargeMassKg);
        } else if (argument == L"--hob") {
            parseDouble(result.options.heightOfBurstM);
        } else if (argument == L"--azimuth") {
            parseFloat(result.options.azimuth);
        } else if (argument == L"--elevation") {
            parseFloat(result.options.elevation);
        } else if (argument == L"--distance-scale") {
            parseFloat(result.options.distanceScale);
        } else if (argument == L"--benchmark") {
            result.options.benchmark = true;
            result.options.headless = true;
        } else if (argument == L"--frames") {
            parseInt(result.options.benchmarkFrames);
        } else if (argument == L"--high-fidelity") {
            result.options.highFidelity = true;
        } else if (argument == L"--solver-resolution") {
            parseInt(result.options.solverResolution);
        } else if (argument == L"--solver-steps") {
            parseInt(result.options.solverSteps);
        } else if (argument == L"--solver-time") {
            parseDouble(result.options.solverTime);
        } else if (argument == L"--volume-resolution") {
            parseInt(result.options.volumeResolution);
        } else if (argument == L"--no-vsync") {
            result.options.vsync = false;
        } else if (argument == L"--debug") {
            result.options.debugLayer = true;
        } else {
            result.valid = false;
            result.error = "Unknown command line option.";
        }
    }

    LocalFree(arguments);
    return result;
}

void AttachParentConsole() {
    const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing != nullptr && existing != INVALID_HANDLE_VALUE) {
        return; // Output is already redirected to a pipe or file.
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    AttachParentConsole();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const ParsedArguments parsed = ParseCommandLine();
    if (parsed.wantsHelp) {
        PrintUsage();
        CoUninitialize();
        return 0;
    }
    if (!parsed.valid) {
        LogError("%s", parsed.error.c_str());
        PrintUsage();
        CoUninitialize();
        return 1;
    }

    ex::app::App app;
    const int result = app.Run(parsed.options);

    CoUninitialize();
    return result;
}
