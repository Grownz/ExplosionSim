#include "app/Splash.h"

#include "gfx/ImageIO.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ex::app {

namespace {

constexpr wchar_t kSplashClass[] = L"ExplosionSimSplash";
constexpr UINT_PTR kAnimationTimer = 1;

int Scale(int value, int dpi) {
    return MulDiv(value, dpi, 96);
}

} // namespace

Splash::~Splash() {
    Close();
}

bool Splash::Show(const std::wstring& imagePath, const std::wstring& title,
                  const std::wstring& subtitle) {
    imagePath_ = imagePath;
    title_ = title;
    subtitle_ = subtitle;

    // WIC is loaded on the main thread (COM is already initialised there); the
    // splash thread only uploads the pixels into a DIB.
    imagePixels_.clear();
    imageWidth_ = 0;
    imageHeight_ = 0;
    gfx::LoadImageBgra(imagePath_, imagePixels_, imageWidth_, imageHeight_);

    running_.store(true);
    thread_ = std::thread(&Splash::ThreadMain, this);

    // Wait for the window to be on screen before the caller starts the heavy
    // initialisation, so the splash is visible from the first moment.
    for (int i = 0; i < 300 && !ready_.load(); ++i) {
        Sleep(10);
    }
    return window_.load() != nullptr;
}

void Splash::SetProgress(float progress, const std::wstring& status,
                         float expectedSeconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = std::clamp(progress, 0.0f, 1.0f);
        targetDuration_ = std::max(expectedSeconds, 0.01f);
        if (!status.empty()) {
            status_ = status;
        }
        stageChanged_ = true;
    }
    // InvalidateRect is safe from another thread; the splash thread paints it.
    if (HWND window = window_.load()) {
        InvalidateRect(window, nullptr, FALSE);
    }
}

void Splash::Close() {
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Splash::ThreadMain() {
    dpi_ = std::max(96, int(GetDpiForSystem()));
    width_ = Scale(980, dpi_);
    height_ = Scale(640, dpi_);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    width_ = std::min(width_, int(workWidth * 0.92f));
    height_ = std::min(height_, int(workHeight * 0.92f));
    const int x = work.left + (workWidth - width_) / 2;
    const int y = work.top + (workHeight - height_) / 2;

    std::vector<uint8_t> pixels;
    int imageWidth = imageWidth_;
    int imageHeight = imageHeight_;
    pixels.swap(imagePixels_);
    if (imageWidth > 0 && imageHeight > 0 && !pixels.empty()) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = imageWidth;
        info.bmiHeader.biHeight = -imageHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        background_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (background_ && bits) {
            memcpy(bits, pixels.data(), size_t(imageWidth) * size_t(imageHeight) * 4);
            backgroundDc_ = CreateCompatibleDC(nullptr);
            if (backgroundDc_) {
                SelectObject(backgroundDc_, background_);
                imageWidth_ = imageWidth;
                imageHeight_ = imageHeight;
            }
        }
    }

    CreateFonts();
    CreateOverlay();

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!classRegistered_) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &Splash::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kSplashClass;
        if (!RegisterClassExW(&windowClass)) {
            ready_.store(true);
            running_.store(false);
            ReleaseResources();
            return;
        }
        classRegistered_ = true;
    }

    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kSplashClass, title_.c_str(), WS_POPUP, x,
                                  y, width_, height_, nullptr, nullptr, instance, this);
    if (!window) {
        ready_.store(true);
        running_.store(false);
        ReleaseResources();
        return;
    }

    window_.store(window);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);
    SetTimer(window, kAnimationTimer, 33, nullptr);
    ready_.store(true);

    MSG message{};
    while (running_.load(std::memory_order_acquire)) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running_.store(false);
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(8);
    }

    KillTimer(window, kAnimationTimer);
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    window_.store(nullptr);
    ReleaseResources();
    ready_.store(false);
}

void Splash::CreateFonts() {
    auto makeFont = [this](int points, int weight, const wchar_t* face) {
        return CreateFontW(-MulDiv(points, dpi_, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    };
    titleFont_ = makeFont(34, FW_SEMIBOLD, L"Segoe UI Semibold");
    subtitleFont_ = makeFont(18, FW_SEMIBOLD, L"Segoe UI Semibold");
    statusFont_ = makeFont(15, FW_NORMAL, L"Segoe UI");
    percentFont_ = makeFont(15, FW_SEMIBOLD, L"Segoe UI Semibold");
}

void Splash::CreateOverlay() {
    if (overlay_ || width_ <= 0 || height_ <= 0) {
        return;
    }
    // Full-size overlay instead of a 1-pixel column: GDI's AlphaBlend leaves a
    // visible horizontal seam when the source is stretched across the width.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    overlay_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!overlay_ || !bits) {
        return;
    }
    auto* pixels = static_cast<uint8_t*>(bits);
    const float start = 0.40f;
    for (int y = 0; y < height_; ++y) {
        const float t = std::clamp((float(y) / float(height_) - start) / (1.0f - start),
                                   0.0f, 1.0f);
        const float eased = t * t;
        const uint8_t alpha = uint8_t(225.0f * eased);
        for (int x = 0; x < width_; ++x) {
            const size_t index = (size_t(y) * size_t(width_) + size_t(x)) * 4;
            // AlphaBlend expects premultiplied source colour; a straight-alpha
            // overlay produced a hard step at its first non-zero row.
            pixels[index + 0] = uint8_t(8 * alpha / 255);
            pixels[index + 1] = uint8_t(9 * alpha / 255);
            pixels[index + 2] = uint8_t(11 * alpha / 255);
            pixels[index + 3] = alpha;
        }
    }
    overlayDc_ = CreateCompatibleDC(nullptr);
    if (overlayDc_) {
        SelectObject(overlayDc_, overlay_);
    }
}

void Splash::Paint(HDC target) {
    HWND window = window_.load();
    if (!target || !window) {
        return;
    }
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1, int(client.right - client.left));
    const int height = std::max(1, int(client.bottom - client.top));

    float progress = 0.0f;
    std::wstring status;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress = displayProgress_;
        status = status_;
    }

    HDC memory = CreateCompatibleDC(target);
    if (!memory) {
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP canvas = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!canvas) {
        DeleteDC(memory);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(memory, canvas);

    if (backgroundDc_ && imageWidth_ > 0 && imageHeight_ > 0) {
        const float scale = std::max(float(width) / float(imageWidth_),
                                     float(height) / float(imageHeight_));
        const int sourceWidth = std::max(1, int(float(width) / scale));
        const int sourceHeight = std::max(1, int(float(height) / scale));
        const int sourceX = std::max(0, (imageWidth_ - sourceWidth) / 2);
        const int sourceY = std::max(0, (imageHeight_ - sourceHeight) / 2);
        SetStretchBltMode(memory, HALFTONE);
        SetBrushOrgEx(memory, 0, 0, nullptr);
        StretchBlt(memory, 0, 0, width, height, backgroundDc_, sourceX, sourceY,
                   sourceWidth, sourceHeight, SRCCOPY);
    } else {
        RECT full{0, 0, width, height};
        HBRUSH brush = CreateSolidBrush(RGB(18, 20, 24));
        FillRect(memory, &full, brush);
        DeleteObject(brush);
    }

    if (overlayDc_) {
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(memory, 0, 0, width, height, overlayDc_, 0, 0, width, height, blend);
    }

    const int margin = Scale(46, dpi_);
    const int barHeight = Scale(9, dpi_);
    const int barY = height - Scale(86, dpi_);
    const int barWidth = width - margin * 2;
    const int statusY = height - Scale(126, dpi_);

    SetBkMode(memory, TRANSPARENT);

    if (titleFont_) {
        SelectObject(memory, titleFont_);
        SetTextColor(memory, RGB(246, 243, 238));
        RECT rect{margin, height - Scale(244, dpi_), width - margin,
                  height - Scale(188, dpi_)};
        DrawTextW(memory, title_.c_str(), -1, &rect,
                  DT_LEFT | DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX);
    }
    if (subtitleFont_) {
        SelectObject(memory, subtitleFont_);
        SetTextColor(memory, RGB(214, 208, 199));
        RECT rect{margin, height - Scale(184, dpi_), width - margin,
                  height - Scale(152, dpi_)};
        DrawTextW(memory, subtitle_.c_str(), -1, &rect,
                  DT_LEFT | DT_SINGLELINE | DT_TOP | DT_NOPREFIX);
    }

    if (statusFont_) {
        SelectObject(memory, statusFont_);
        SetTextColor(memory, RGB(224, 219, 211));
        RECT rect{margin, statusY, width - margin - Scale(70, dpi_), statusY + Scale(24, dpi_)};
        DrawTextW(memory, status.c_str(), -1, &rect,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    if (percentFont_) {
        SelectObject(memory, percentFont_);
        SetTextColor(memory, RGB(240, 236, 230));
        wchar_t percent[16];
        swprintf_s(percent, L"%d%%", int(std::lround(progress * 100.0f)));
        RECT rect{width - margin - Scale(64, dpi_), statusY, width - margin,
                  statusY + Scale(24, dpi_)};
        DrawTextW(memory, percent, -1, &rect,
                  DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }

    // Progress bar: a dark rounded track and an accent fill.
    const int radius = barHeight;
    HBRUSH track = CreateSolidBrush(RGB(40, 38, 35));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(78, 72, 64));
    HGDIOBJ oldBrush = SelectObject(memory, track);
    HGDIOBJ oldPen = SelectObject(memory, border);
    RoundRect(memory, margin, barY, margin + barWidth, barY + barHeight, radius, radius);

    const int fillWidth = int(float(barWidth) * progress);
    if (fillWidth > 1) {
        HBRUSH fill = CreateSolidBrush(RGB(228, 116, 44));
        SelectObject(memory, fill);
        const int right = margin + std::max(fillWidth, barHeight);
        RoundRect(memory, margin, barY, right, barY + barHeight, radius, radius);
        DeleteObject(fill);

        // A slow shimmer inside the filled part shows that the loader is still
        // busy during long stages (shader compilation, town generation).
        if (pulse_ > 0.0f && fillWidth > barHeight * 3) {
            const int highlightWidth = std::min(fillWidth, Scale(74, dpi_));
            const int travel = std::max(1, fillWidth - highlightWidth);
            const int highlightX = margin + int(float(travel) * pulse_);
            HBRUSH highlight = CreateSolidBrush(RGB(247, 163, 88));
            SelectObject(memory, highlight);
            RoundRect(memory, highlightX, barY, highlightX + highlightWidth,
                      barY + barHeight, radius, radius);
            DeleteObject(highlight);
        }
    }
    SelectObject(memory, oldBrush);
    SelectObject(memory, oldPen);
    DeleteObject(track);
    DeleteObject(border);

    BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(canvas);
    DeleteDC(memory);
}

LRESULT CALLBACK Splash::WindowProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Splash*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self) {
        return self->HandleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Splash::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wParam == kAnimationTimer) {
            float target = 0.0f;
            float duration = 0.35f;
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                target = progress_;
                duration = targetDuration_;
                changed = stageChanged_;
                stageChanged_ = false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (changed || !stageActive_) {
                stageStart_ = displayProgress_;
                stageTarget_ = std::max(target, displayProgress_);
                stageDuration_ = std::max(duration, 0.01f);
                stageStartTime_ = now;
                stageActive_ = true;
            }
            const double elapsed =
                std::chrono::duration<double>(now - stageStartTime_).count();
            const float t = std::clamp(float(elapsed) / stageDuration_, 0.0f, 1.0f);
            displayProgress_ = stageStart_ + (stageTarget_ - stageStart_) * t;
            if (t >= 1.0f) {
                displayProgress_ = stageTarget_;
            }
            // Only show the shimmer when the stage runs longer than measured.
            if (elapsed > double(stageDuration_) + 0.25) {
                pulse_ = std::fmod(pulse_ + 0.018f, 1.0f);
            } else {
                pulse_ = 0.0f;
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        Paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        return 0; // the application closes the splash itself
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void Splash::ReleaseResources() {
    if (backgroundDc_) {
        DeleteDC(backgroundDc_);
        backgroundDc_ = nullptr;
    }
    if (background_) {
        DeleteObject(background_);
        background_ = nullptr;
    }
    if (overlayDc_) {
        DeleteDC(overlayDc_);
        overlayDc_ = nullptr;
    }
    if (overlay_) {
        DeleteObject(overlay_);
        overlay_ = nullptr;
    }
    if (titleFont_) {
        DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
    if (subtitleFont_) {
        DeleteObject(subtitleFont_);
        subtitleFont_ = nullptr;
    }
    if (statusFont_) {
        DeleteObject(statusFont_);
        statusFont_ = nullptr;
    }
    if (percentFont_) {
        DeleteObject(percentFont_);
        percentFont_ = nullptr;
    }
    imageWidth_ = 0;
    imageHeight_ = 0;
}

} // namespace ex::app
