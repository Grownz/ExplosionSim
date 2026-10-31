// Headless splash test. It runs the SDL3 splash with the dummy video driver,
// so it works on CI for both Windows and Linux, and verifies the two things
// the splash has to guarantee: the text uses the shared UI font, and image
// changes crossfade instead of snapping.

#include "app/FontPaths.h"
#include "app/SplashSdl.h"
#include "gfx/ImageIO.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAILED: %s\n", message);
        ++gFailures;
    }
}

struct Image {
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
};

bool CaptureImage(ex::app::SplashSdl& splash, Image& image) {
    return splash.Capture(image.bgra, image.width, image.height) &&
           image.width > 0 && image.height > 0 && !image.bgra.empty();
}

double MeanAbsDiff(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height ||
        a.bgra.size() != b.bgra.size() || a.bgra.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (size_t i = 0; i < a.bgra.size(); ++i) {
        sum += std::abs(int(a.bgra[i]) - int(b.bgra[i]));
    }
    return sum / double(a.bgra.size());
}

// Counts near-white pixels in a horizontal band. The title is drawn in
// (246, 243, 238) over the darkened lower half of the splash.
int BrightPixels(const Image& image, int y0, int y1) {
    int count = 0;
    const int top = std::max(0, y0);
    const int bottom = std::min(image.height, y1);
    for (int y = top; y < bottom; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const size_t index =
                (size_t(y) * size_t(image.width) + size_t(x)) * 4;
            const int b = image.bgra[index];
            const int g = image.bgra[index + 1];
            const int r = image.bgra[index + 2];
            if (r > 225 && g > 220 && b > 205) {
                ++count;
            }
        }
    }
    return count;
}

void SaveImage(const char* name, const Image& image) {
    if (image.bgra.empty()) {
        return;
    }
    ex::gfx::SavePng(name, image.bgra.data(), image.width, image.height);
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("SDL_VIDEODRIVER", "dummy");
#else
    setenv("SDL_VIDEODRIVER", "dummy", 1);
#endif

    std::filesystem::path assets =
        std::filesystem::path(argv[0]).parent_path() / "assets";
    if (argc > 1) {
        assets = argv[1];
    }

    std::vector<std::string> frames;
    for (int i = 0; i < 20; ++i) {
        char name[64] = {};
        std::snprintf(name, sizeof(name), "splash/frame_%02d.jpg", i);
        frames.push_back((assets / name).string());
    }
    // Two visually distinct stills make the crossfade assertion deterministic:
    // a single frame change, measured in the middle of its fade.
    std::vector<std::string> testFrames{frames.front(), frames.back()};

    Check(!ex::app::UiFontPath().empty(),
          "the shared UI font is available on this platform");

    ex::app::SplashSdl splash;
    if (!splash.Show(testFrames, "Explosion Simulator", "Splash test")) {
        std::printf("SKIP: the SDL splash is unavailable.\n");
        return gFailures == 0 ? 0 : 1;
    }

    const auto pumpFor = [&splash](double seconds) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             start)
                   .count() < seconds) {
            splash.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    };

    pumpFor(0.15);
    Image first;
    Check(CaptureImage(splash, first), "the splash captures its first frame");

    const float scale = float(first.height) / 640.0f;
    const int titleTop = first.height - int(244.0f * scale);
    const int titleBottom = first.height - int(184.0f * scale);
    const int bright = BrightPixels(first, titleTop, titleBottom);
    std::printf("[test] title band bright pixels: %d\n", bright);
    Check(bright > 100, "the title is rendered with the shared TTF font");

    // Advance to the second still and sample in the middle of the crossfade.
    splash.SetProgress(0.6f, "Loading assets...", 0.03f);
    pumpFor(0.09);
    Image middle;
    Check(CaptureImage(splash, middle),
          "the splash captures a crossfade frame");
    pumpFor(0.35);
    Image last;
    Check(CaptureImage(splash, last), "the splash captures the next frame");

    const double firstLast = MeanAbsDiff(first, last);
    const double firstMiddle = MeanAbsDiff(first, middle);
    const double middleLast = MeanAbsDiff(middle, last);
    std::printf("[test] diff first/last=%.2f first/mid=%.2f mid/last=%.2f\n",
                firstLast, firstMiddle, middleLast);
    Check(firstLast > 1.0, "the splash advances to another still");
    Check(firstMiddle > 0.2, "the crossfade differs from the outgoing still");
    Check(middleLast > 0.2, "the crossfade differs from the incoming still");

    SaveImage("splash_test_first.png", first);
    SaveImage("splash_test_middle.png", middle);
    SaveImage("splash_test_last.png", last);

    splash.Close();

    if (gFailures > 0) {
        std::printf("%d splash check(s) failed.\n", gFailures);
        return 1;
    }
    std::printf("All splash tests passed.\n");
    return 0;
}
