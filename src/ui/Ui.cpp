#include "ui/Ui.h"

#include "Version.h"

#include <imgui.h>

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <sstream>
#include <vector>

namespace ex::ui {

namespace {

constexpr ImVec4 kAccent(1.00f, 0.52f, 0.16f, 1.00f);
constexpr ImVec4 kAccentDim(0.80f, 0.36f, 0.10f, 1.00f);
constexpr ImVec4 kWarning(1.00f, 0.78f, 0.35f, 1.00f);
constexpr ImVec4 kMuted(0.62f, 0.65f, 0.70f, 1.00f);

// Dedicated font for the changelog viewer: two points larger than the UI font.
ImFont* gChangelogFont = nullptr;

struct ChangelogFontGuard {
    bool pushed = false;

    explicit ChangelogFontGuard(ImFont* font) {
        if (font) {
            ImGui::PushFont(font);
            pushed = true;
        }
    }
    ~ChangelogFontGuard() {
        if (pushed) {
            ImGui::PopFont();
        }
    }
};

void SectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::SeparatorText(label);
    ImGui::Spacing();
}

void Readout(const char* label, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    ImGui::TextUnformatted(label);
    const float textWidth = ImGui::CalcTextSize(buffer).x;
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, rightEdge - textWidth));
    ImGui::TextUnformatted(buffer);
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

// Removes the inline markdown markers the changelog uses. The default font has
// the Latin-1 glyphs, so only typographic punctuation is normalised.
std::string StripInline(std::string text) {
    ReplaceAll(text, "**", "");
    ReplaceAll(text, "`", "");
    for (size_t open = text.find('['); open != std::string::npos;
         open = text.find('[', open + 1)) {
        const size_t close = text.find(']', open + 1);
        if (close == std::string::npos) {
            break;
        }
        if (close + 1 < text.size() && text[close + 1] == '(') {
            const size_t end = text.find(')', close + 2);
            if (end != std::string::npos) {
                text.replace(open, end - open + 1, text.substr(open + 1, close - open - 1));
                continue;
            }
        }
    }
    ReplaceAll(text, "\xE2\x80\x93", "-"); // en dash
    ReplaceAll(text, "\xE2\x80\x94", "-"); // em dash
    ReplaceAll(text, "\xE2\x80\x99", "'"); // right single quote
    ReplaceAll(text, "\xE2\x80\x9C", "\""); // left double quote
    ReplaceAll(text, "\xE2\x80\x9D", "\""); // right double quote
    return text;
}

std::vector<std::string> SplitTableRow(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    for (char character : line) {
        if (character == '|') {
            cells.push_back(cell);
            cell.clear();
        } else {
            cell += character;
        }
    }
    cells.push_back(cell);

    if (!cells.empty() && cells.front().find_first_not_of(" \t") == std::string::npos) {
        cells.erase(cells.begin());
    }
    if (!cells.empty() && cells.back().find_first_not_of(" \t") == std::string::npos) {
        cells.pop_back();
    }
    for (std::string& value : cells) {
        const size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos) {
            value.clear();
            continue;
        }
        const size_t last = value.find_last_not_of(" \t");
        value = value.substr(first, last - first + 1);
    }
    return cells;
}

bool IsTableSeparator(const std::vector<std::string>& cells) {
    if (cells.empty()) {
        return false;
    }
    for (const std::string& cell : cells) {
        if (!cell.empty() && cell.find_first_not_of("-: ") != std::string::npos) {
            return false;
        }
    }
    return true;
}

// Renders the Markdown subset used by CHANGELOG.md: headings, bullets, bold and
// inline code, fenced code, and pipe tables.
void DrawMarkdown(const std::string& text) {
    ChangelogFontGuard fontGuard(gChangelogFont);
    std::istringstream stream(text);
    std::string line;
    std::vector<std::string> table;
    int tableId = 0;

    const auto flushTable = [&]() {
        if (table.empty()) {
            return;
        }
        std::vector<std::vector<std::string>> rows;
        rows.reserve(table.size());
        for (const std::string& row : table) {
            rows.push_back(SplitTableRow(row));
        }
        table.clear();
        if (rows.empty() || rows.front().empty()) {
            return;
        }

        const size_t columns = rows.front().size();
        size_t firstData = 1;
        if (rows.size() > 1 && IsTableSeparator(rows[1])) {
            firstData = 2;
        }

        std::vector<std::string> headers;
        headers.reserve(columns);
        for (size_t column = 0; column < columns; ++column) {
            headers.push_back(StripInline(rows.front()[column]));
        }

        ImGui::PushID(tableId++);
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##md_table", int(columns), flags)) {
            for (size_t column = 0; column < columns; ++column) {
                ImGui::TableSetupColumn(headers[column].c_str(),
                                        ImGuiTableColumnFlags_WidthStretch);
            }
            ImGui::TableHeadersRow();
            for (size_t row = firstData; row < rows.size(); ++row) {
                ImGui::TableNextRow();
                for (size_t column = 0; column < columns; ++column) {
                    ImGui::TableSetColumnIndex(int(column));
                    const std::string cell =
                        column < rows[row].size() ? rows[row][column] : std::string{};
                    ImGui::TextWrapped("%s", StripInline(cell).c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    };

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line.front() == '|') {
            table.push_back(line);
            continue;
        }
        flushTable();

        if (line.empty()) {
            ImGui::Spacing();
            continue;
        }
        if (line.front() == '[' && line.find("]:") != std::string::npos) {
            continue; // link definition
        }
        if (line.rfind("### ", 0) == 0) {
            ImGui::Spacing();
            ImGui::TextColored(kAccent, "%s", StripInline(line.substr(4)).c_str());
        } else if (line.rfind("## ", 0) == 0) {
            ImGui::Spacing();
            ImGui::SeparatorText(StripInline(line.substr(3)).c_str());
        } else if (line.rfind("# ", 0) == 0) {
            ImGui::TextColored(kAccent, "%s", StripInline(line.substr(2)).c_str());
        } else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", StripInline(line.substr(2)).c_str());
        } else {
            ImGui::TextWrapped("%s", StripInline(line).c_str());
        }
    }
    flushTable();
}

void DrawUpdateBanner(UiContext& context, UiActions& actions) {
    const UpdateUiState& update = context.update;
    if (update.downloading) {
        ImGui::Text("Downloading %s...", update.version.c_str());
        ImGui::ProgressBar(update.progress, ImVec2(-1.0f, 0.0f));
        if (update.totalBytes > 0) {
            ImGui::Text("%.1f / %.1f MB",
                        double(update.downloadedBytes) / (1024.0 * 1024.0),
                        double(update.totalBytes) / (1024.0 * 1024.0));
        }
        if (ImGui::Button("Cancel##banner_cancel", ImVec2(-1.0f, 0.0f))) {
            actions.cancelDownload = true;
        }
        ImGui::Separator();
        return;
    }
    if (update.available) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        ImGui::TextWrapped("Version %s is available.", update.version.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("Download##banner_download", ImVec2(-1.0f, 0.0f))) {
            actions.downloadUpdate = true;
        }
        ImGui::Separator();
    } else if (update.downloaded) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextWrapped("Version %s downloaded.", update.version.c_str());
        ImGui::PopStyleColor();
        if (update.verified) {
            ImGui::TextUnformatted("SHA-256 verified.");
        } else {
            ImGui::TextUnformatted("No checksum was published.");
        }
        if (ImGui::Button("Install and restart##banner_install", ImVec2(-1.0f, 0.0f))) {
            actions.installUpdate = true;
        }
        if (ImGui::Button("Show in folder##banner_folder", ImVec2(-1.0f, 0.0f))) {
            actions.openDownloadFolder = true;
        }
        ImGui::Separator();
    }
}

void DrawUpdates(UiContext& context, app::AppSettings& settings, UiActions& actions) {
    const UpdateUiState& update = context.update;
    ImGui::Checkbox("Check for updates automatically", &settings.update.checkForUpdates);
    ImGui::BeginDisabled(update.checking || update.downloading);
    if (ImGui::Button("Check now", ImVec2(-1.0f, 0.0f))) {
        actions.checkForUpdates = true;
    }
    ImGui::EndDisabled();

    static char proxy[256] = {};
    static bool proxyInitialized = false;
    if (!proxyInitialized) {
        std::snprintf(proxy, sizeof(proxy), "%s", settings.update.proxy.c_str());
        proxyInitialized = true;
    }
    if (ImGui::InputText("HTTP proxy", proxy, sizeof(proxy))) {
        settings.update.proxy = proxy;
    }
    Hint("Leave empty to use the system proxy. Applied to the next check or download.");

    if (update.checking) {
        ImGui::TextUnformatted("Checking GitHub for a newer release...");
    } else if (update.failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        ImGui::TextWrapped("Update failed: %s", update.error.c_str());
        ImGui::PopStyleColor();
        if (update.downloadFailed) {
            if (ImGui::Button("Retry download", ImVec2(-1.0f, 0.0f))) {
                actions.downloadUpdate = true;
            }
        } else if (ImGui::Button("Retry check", ImVec2(-1.0f, 0.0f))) {
            actions.checkForUpdates = true;
        }
    } else if (update.upToDate) {
        ImGui::TextUnformatted("You are running the latest version.");
    }

    if (update.available) {
        ImGui::Spacing();
        ImGui::Text("Version %s is available.", update.version.c_str());
        if (ImGui::Button("Download", ImVec2(-1.0f, 0.0f))) {
            actions.downloadUpdate = true;
        }
        if (ImGui::Button("Skip this version", ImVec2(-1.0f, 0.0f))) {
            actions.skipUpdateVersion = true;
        }
    }
    if (update.downloading) {
        ImGui::ProgressBar(update.progress, ImVec2(-1.0f, 0.0f));
        if (ImGui::Button("Cancel download", ImVec2(-1.0f, 0.0f))) {
            actions.cancelDownload = true;
        }
    }
    if (update.downloaded) {
        ImGui::TextUnformatted(update.verified
                                   ? "The update zip is verified (SHA-256)."
                                   : "The update zip is ready (no checksum published).");
        if (ImGui::Button("Install and restart", ImVec2(-1.0f, 0.0f))) {
            actions.installUpdate = true;
        }
        if (ImGui::Button("Show in folder", ImVec2(-1.0f, 0.0f))) {
            actions.openDownloadFolder = true;
        }
    }

    Hint("Checks github.com/Grownz/ExplosionSim once per day. Downloads are saved to "
         "%LOCALAPPDATA%\\ExplosionSim\\updates and are not installed automatically. "
         "Install restarts the app, replaces the executable and assets and keeps "
         "settings.ini.");
}

void DrawUpdateModals(UiContext& context, UiActions& actions) {
    static bool whatsNewOpened = false;
    if (context.showWhatsNew && !whatsNewOpened) {
        ImGui::OpenPopup("What's new##whatsnew");
        whatsNewOpened = true;
    } else if (!context.showWhatsNew) {
        whatsNewOpened = false;
    }

    bool whatsNewOpen = true;
    ImGui::SetNextWindowSize(ImVec2(880.0f, 640.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("What's new##whatsnew", &whatsNewOpen,
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(kAccent, "Explosion Simulator %s", EXPLOSION_VERSION_STRING);
        ImGui::Separator();
        ImGui::BeginChild("##whatsnew_scroll",
                          ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), false);
        if (context.currentReleaseNotes && !context.currentReleaseNotes->empty()) {
            DrawMarkdown(*context.currentReleaseNotes);
        } else {
            ImGui::TextWrapped("No changelog is bundled with this build.");
        }
        ImGui::EndChild();
        if (ImGui::Button("Close", ImVec2(140.0f, 0.0f))) {
            whatsNewOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!whatsNewOpen) {
        actions.dismissWhatsNew = true;
        whatsNewOpened = false;
    }

    if (actions.showChangelog) {
        ImGui::OpenPopup("Changelog##changelog");
    }
    ImGui::SetNextWindowSize(ImVec2(940.0f, 700.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Changelog##changelog", nullptr,
                               ImGuiWindowFlags_NoSavedSettings)) {
        static bool showAll = true;
        if (ImGui::RadioButton("Current release", !showAll)) {
            showAll = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("All releases", showAll)) {
            showAll = true;
        }
        ImGui::Separator();
        ImGui::BeginChild("##changelog_scroll",
                          ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), false);
        const std::string* text =
            showAll ? context.changelogText : context.currentReleaseNotes;
        if (text && !text->empty()) {
            DrawMarkdown(*text);
        } else {
            ImGui::TextWrapped("No changelog is bundled with this build.");
        }
        ImGui::EndChild();
        if (ImGui::Button("Open on GitHub", ImVec2(180.0f, 0.0f))) {
            ShellExecuteW(nullptr, L"open",
                          L"https://github.com/Grownz/ExplosionSim/releases", nullptr,
                          nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(140.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Compact TNT-equivalent label for the on-screen data box.
void FormatMass(double kilograms, char* buffer, size_t size) {
    if (kilograms >= 1.0e6) {
        snprintf(buffer, size, "%.4g kt TNT", kilograms / 1.0e6);
    } else if (kilograms >= 1.0e3) {
        snprintf(buffer, size, "%.4g t TNT", kilograms / 1.0e3);
    } else {
        snprintf(buffer, size, "%.4g kg TNT", kilograms);
    }
}

// Bottom-left map scale for the active grid overlay. The bar length is snapped
// to a 1-2-5 sequence and its pixel size is kept within a readable range as the
// camera zooms.
void DrawScaleBar(const gfx::Camera& camera, const ImVec2& displaySize) {
    if (displaySize.x < 200.0f || displaySize.y < 200.0f) {
        return;
    }

    const float distance = std::max(camera.Distance(), 0.1f);
    const float worldHeight = 2.0f * distance * std::tan(camera.FovY() * 0.5f);
    const float pixelsPerMeter = displaySize.y / std::max(worldHeight, 1e-4f);
    if (!std::isfinite(pixelsPerMeter) || pixelsPerMeter <= 0.0f) {
        return;
    }

    const float desiredPixels = std::clamp(displaySize.y * 0.13f, 80.0f, 190.0f);
    const float rawMeters = desiredPixels / pixelsPerMeter;
    const float exponent = std::floor(std::log10(std::max(rawMeters, 1e-5f)));
    float base = std::pow(10.0f, exponent);

    const float mantissa = rawMeters / base;
    float nice = 1.0f;
    if (mantissa >= 7.0f) {
        nice = 10.0f;
    } else if (mantissa >= 3.0f) {
        nice = 5.0f;
    } else if (mantissa >= 1.5f) {
        nice = 2.0f;
    }

    float meters = nice * base;
    float barPixels = meters * pixelsPerMeter;

    for (int i = 0; i < 8 && barPixels > 280.0f; ++i) {
        if (nice > 5.0f) {
            nice = 5.0f;
        } else if (nice > 2.0f) {
            nice = 2.0f;
        } else if (nice > 1.0f) {
            nice = 1.0f;
        } else {
            nice = 5.0f;
            base *= 0.1f;
        }
        meters = nice * base;
        barPixels = meters * pixelsPerMeter;
    }

    for (int i = 0; i < 8 && barPixels < 60.0f; ++i) {
        if (nice < 2.0f) {
            nice = 2.0f;
        } else if (nice < 5.0f) {
            nice = 5.0f;
        } else if (nice < 10.0f) {
            nice = 10.0f;
        } else {
            nice = 1.0f;
            base *= 10.0f;
        }
        meters = nice * base;
        barPixels = meters * pixelsPerMeter;
    }

    char label[64];
    if (meters >= 1000.0f) {
        std::snprintf(label, sizeof(label), "%.3g km", meters / 1000.0f);
    } else {
        std::snprintf(label, sizeof(label), "%.3g m", meters);
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const float fontSize = ImGui::GetFontSize();
    const float margin = std::max(20.0f, fontSize * 1.2f);
    const float baseline = displaySize.y - margin - fontSize * 1.2f;
    const float x0 = margin;
    const float x1 = x0 + barPixels;

    const ImU32 color = IM_COL32(238, 241, 245, 235);
    const ImU32 shadow = IM_COL32(0, 0, 0, 160);

    drawList->AddRectFilled(ImVec2(x0 - 12.0f, baseline - fontSize * 2.4f),
                            ImVec2(x1 + 12.0f, baseline + 12.0f),
                            IM_COL32(8, 10, 14, 140), 6.0f);
    drawList->AddLine(ImVec2(x0, baseline), ImVec2(x1, baseline), shadow, 4.0f);
    drawList->AddLine(ImVec2(x0, baseline), ImVec2(x1, baseline), color, 2.0f);
    drawList->AddLine(ImVec2(x0, baseline - 7.0f), ImVec2(x0, baseline + 7.0f), color, 2.0f);
    drawList->AddLine(ImVec2(x1, baseline - 7.0f), ImVec2(x1, baseline + 7.0f), color, 2.0f);

    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 textPosition((x0 + x1) * 0.5f - textSize.x * 0.5f,
                              baseline - textSize.y - 9.0f);
    drawList->AddText(ImVec2(textPosition.x + 1.0f, textPosition.y + 1.0f), shadow, label);
    drawList->AddText(textPosition, color, label);
}

bool PhysSlider(const char* label, double* value, float minValue, float maxValue,
                const char* format, ImGuiSliderFlags flags, UiActions& actions) {
    float temporary = float(*value);
    const bool changed = ImGui::SliderFloat(label, &temporary, minValue, maxValue, format, flags);
    if (changed) {
        *value = double(temporary);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }
    return changed;
}

void DrawDetonation(app::AppSettings& settings, const sim::Simulation& simulation,
                    UiActions& actions) {
    SectionHeader("DETONATION");

    double massKg = settings.simulation.chargeMassKg;
    const double minMass = 0.01;
    const double maxMass = 1.0e8; // 100 000 t
    const char* massFormat = massKg < 100.0 ? "%.3f kg TNT" : "%.0f kg TNT";
    if (ImGui::SliderScalar("Charge", ImGuiDataType_Double, &massKg, &minMass,
                            &maxMass, massFormat, ImGuiSliderFlags_Logarithmic)) {
        settings.simulation.chargeMassKg = std::clamp(massKg, 0.01, 1.0e8);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    const char* presetLabels[] = {"20 g", "200 g", "1 kg", "50 kg", "1 t"};
    const double presetValues[] = {0.02, 0.2, 1.0, 50.0, 1000.0};
    const float presetWidth = ImGui::GetContentRegionAvail().x / 5.0f - 2.0f;
    for (int i = 0; i < 5; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(presetLabels[i], ImVec2(presetWidth, 0))) {
            settings.simulation.chargeMassKg = presetValues[i];
            actions.applySimulationSettings = true;
            actions.restart = true;
        }
    }

    const char* largeLabels[] = {"10 t", "100 t", "1000 t", "10000 t"};
    const double largeValues[] = {1.0e4, 1.0e5, 1.0e6, 1.0e7};
    const float largeWidth = ImGui::GetContentRegionAvail().x / 4.0f - 2.0f;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        if (ImGui::Button(largeLabels[i], ImVec2(largeWidth, 0))) {
            settings.simulation.chargeMassKg = largeValues[i];
            actions.applySimulationSettings = true;
            actions.restart = true;
        }
    }

    // Same range as Settings.cpp accepts, so a value loaded from settings.ini
    // is not silently clamped by the slider.
    PhysSlider("Height of burst", &settings.simulation.heightOfBurstM, 0.01, 5000.0,
               "%.2f m", ImGuiSliderFlags_Logarithmic, actions);

    float fuse = float(settings.simulation.fuseSeconds);
    if (ImGui::SliderFloat("Fuse", &fuse, 0.0f, 10.0f, "%.1f s")) {
        settings.simulation.fuseSeconds = double(fuse);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    const bool rendering = !simulation.IsArmed();
    const char* mainLabel =
        rendering ? "REARM##main_detonate" : "DETONATE##main_detonate";
    if (ImGui::Button(mainLabel, ImVec2(-1.0f, ImGui::GetFontSize() * 2.2f))) {
        if (rendering) {
            actions.rearm = true;
        } else {
            actions.detonateNow = true;
        }
    }
    ImGui::PopStyleColor(3);

    // Pause and frame controls live below the main action, in the same style.
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    const float actionHeight = ImGui::GetFontSize() * 2.2f;
    const float halfWidth = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
    if (ImGui::Button(settings.paused ? "RESUME##main_pause" : "PAUSE##main_pause",
                      ImVec2(halfWidth, actionHeight))) {
        settings.paused = !settings.paused;
    }
    ImGui::SameLine();
    if (ImGui::Button("FRAME##main_frame", ImVec2(halfWidth, actionHeight))) {
        actions.frameCamera = true;
    }
    ImGui::PopStyleColor(3);

    if (rendering) {
        Hint("Stops the current solver and resets the scene. The next click starts "
             "the new explosion.");
    } else {
        Hint("Equivalent TNT energy: the charge mass is converted with 4.184 MJ/kg.");
    }
}

void DrawAtmosphere(app::AppSettings& settings, UiActions& actions) {
    SectionHeader("ATMOSPHERE");

    double temperatureK = settings.simulation.ambientTemperatureK;
    float temperatureC = float(temperatureK - 273.15);
    if (ImGui::SliderFloat("Temperature", &temperatureC, -20.0f, 45.0f, "%.1f °C")) {
        settings.simulation.ambientTemperatureK = double(temperatureC) + 273.15;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    double pressurePa = settings.simulation.ambientPressurePa;
    float pressureHpa = float(pressurePa / 100.0);
    if (ImGui::SliderFloat("Pressure", &pressureHpa, 850.0f, 1050.0f, "%.0f hPa")) {
        settings.simulation.ambientPressurePa = double(pressureHpa) * 100.0;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    PhysSlider("Wind speed", &settings.simulation.windSpeedMs, 0.0, 30.0, "%.1f m/s",
               ImGuiSliderFlags_None, actions);
    PhysSlider("Wind direction", &settings.simulation.windDirectionDeg, 0.0, 360.0, "%.0f°",
               ImGuiSliderFlags_None, actions);

    float humidity = float(settings.simulation.relativeHumidity * 100.0);
    if (ImGui::SliderFloat("Humidity", &humidity, 0.0f, 100.0f, "%.0f %%")) {
        settings.simulation.relativeHumidity = double(humidity) / 100.0;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    // Derived quantities show the physical effect of the atmosphere.
    sim::Environment environment{};
    environment.pressurePa = settings.simulation.ambientPressurePa;
    environment.temperatureK = settings.simulation.ambientTemperatureK;
    environment.relativeHumidity = settings.simulation.relativeHumidity;
    Readout("Air density", "%.4f kg/m³", environment.AirDensity());
    Readout("Speed of sound", "%.1f m/s", environment.SoundSpeed());
    Readout("Vapour pressure", "%.0f Pa", environment.VaporPressurePa());
    Readout("Virtual temperature", "%.1f K", environment.VirtualTemperatureK());

    Hint("Density and sound speed drive the blast integration (Sachs scaling). "
         "Humidity lowers the density and raises the sound speed; wind advects the "
         "rising cloud and particles.");
}

void DrawPlayback(app::AppSettings& settings, UiActions& actions, const sim::Simulation& simulation) {
    SectionHeader("PLAYBACK");

    if (ImGui::Checkbox("Pause##playback", &settings.paused)) {
    }
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        actions.stepOnce = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rearm")) {
        actions.rearm = true;
    }

    float timeScale = float(settings.simulation.timeScale);
    if (ImGui::SliderFloat("Time scale", &timeScale, 0.005f, 2.0f, "×%.3f",
                           ImGuiSliderFlags_Logarithmic)) {
        settings.simulation.timeScale = double(timeScale);
        actions.applySimulationSettings = true;
    }

    if (ImGui::Button("Set real time (1x)", ImVec2(-1.0f, 0.0f))) {
        settings.simulation.timeScale = 1.0;
        actions.applySimulationSettings = true;
    }
    Hint("Playback speed: 1.0 plays one simulated second per real second. The "
         "high-fidelity solver follows this scale directly.");

    if (ImGui::Checkbox("Loop simulation", &settings.simulation.loop)) {
        actions.applySimulationSettings = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame camera")) {
        actions.frameCamera = true;
    }

    if (simulation.IsArmed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        if (simulation.IsFuseHeld()) {
            Readout("State", "Ready to detonate");
        } else {
            Readout("Fuse", "%.2f s", simulation.FuseRemaining());
        }
        ImGui::PopStyleColor();
        if (ImGui::Button("Detonate now", ImVec2(-1.0f, 0.0f))) {
            actions.detonateNow = true;
        }
    } else {
        Readout("Simulation time", "%.4f s", simulation.Time());
    }
    Readout("Real time", "%.1f s", simulation.RealTime());
}

void DrawEnvironment(app::AppSettings& settings, const sim::Simulation& simulation,
                     UiActions& actions) {
    SectionHeader("ENVIRONMENT");

    auto& scene = settings.simulation.scene;
    if (ImGui::Checkbox("Procedural scene", &scene.enabled)) {
        actions.applySimulationSettings = true;
    }

    int seed = int(scene.seed);
    if (ImGui::InputInt("Scene seed", &seed)) {
        scene.seed = uint32_t(std::max(0, seed));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    float density = scene.density;
    if (ImGui::SliderFloat("Scene density", &density, 0.0f, 2.0f, "%.2f")) {
        scene.density = density;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }

    if (ImGui::Checkbox("Buildings", &scene.buildings)) {
        actions.applySimulationSettings = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Vehicles", &scene.vehicles)) {
        actions.applySimulationSettings = true;
    }
    if (ImGui::Checkbox("Vegetation", &scene.vegetation)) {
        actions.applySimulationSettings = true;
    }

    const char* qualityLabels[] = {"Low", "Medium", "High"};
    int quality = std::clamp(scene.quality, 0, 2);
    if (ImGui::Combo("Scene quality", &quality, qualityLabels, 3)) {
        scene.quality = quality;
        actions.applySimulationSettings = true;
    }

    if (ImGui::Checkbox("Voronoi chunks", &scene.voronoiChunks)) {
        actions.applySimulationSettings = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Convex Voronoi cells clipped to the building box (default). "
                          "Disable to use the cheaper jittered grid instead.");
    }

    ImGui::BeginDisabled(!scene.voronoiChunks);
    float chunkScale = scene.chunkScale;
    if (ImGui::SliderFloat("Chunk size", &chunkScale, 0.25f, 2.0f, "%.2fx",
                           ImGuiSliderFlags_Logarithmic)) {
        scene.chunkScale = chunkScale;
    }
    // Rebuild once when the drag ends instead of on every intermediate frame.
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        actions.applySimulationSettings = true;
    }
    ImGui::EndDisabled();
    Hint("Average Voronoi chunk size: 0.25x = four times smaller, 2x = twice as "
         "large. Very small chunks are limited by the per-building cell cap.");
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("Global scene budget: %d chunks.", scene::kSceneChunkBudget);
    ImGui::PopStyleColor();

    if (ImGui::Button("Regenerate scene")) {
        scene.seed += 1;
        actions.applySimulationSettings = true;
    }

    Readout("Objects", "%d", int(simulation.Scene().Objects().size()));
    Readout("Chunks", "%d", simulation.Scene().Stats().chunks);
    Readout("Scene radius", "%.0f m", simulation.Scene().Extent());
    Hint("Buildings, vehicles and trees are generated deterministically around ground "
         "zero. The scene reacts to the analytic blast field.");
}

void DrawPhysics(app::AppSettings& settings, const sim::Simulation& simulation,
                 gfx::Renderer& renderer, UiActions& actions) {
    SectionHeader("SIMULATION MODEL");

    int modelIndex = int(settings.simulation.mode) == 0
                         ? 0
                         : (settings.simulation.solverHemisphere ? 2 : 1);
    const char* modeLabels[] = {"Reduced-order (analytic)",
                                "High-fidelity (cube)",
                                "High-fidelity (hemisphere)"};
    if (ImGui::Combo("Model", &modelIndex, modeLabels, 3)) {
        settings.simulation.mode =
            modelIndex == 0 ? sim::SimulationMode::ReducedOrder
                            : sim::SimulationMode::HighFidelity;
        settings.simulation.solverHemisphere = modelIndex == 2;
        actions.applySimulationSettings = true;
    }

    if (settings.simulation.mode == sim::SimulationMode::HighFidelity) {
        const int solverResolutions[] = {64, 96, 128, 192, 256, 512};
        const char* solverResolutionLabels[] = {"64³", "96³", "128³", "192³", "256³", "512³"};
        int current = 2;
        for (int i = 0; i < 6; ++i) {
            if (solverResolutions[i] == settings.simulation.solverResolution) {
                current = i;
            }
        }
        if (ImGui::Combo("Solver grid", &current, solverResolutionLabels, 6)) {
            settings.simulation.solverResolution = solverResolutions[current];
            actions.applySimulationSettings = true;
        }

        if (ImGui::SliderInt("Max steps/frame", &settings.simulation.solverStepsPerFrame, 8, 256)) {
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.applySimulationSettings = true;
        }
        Hint("Caps the high-fidelity solver work per frame. Lower values keep the frame "
             "rate up but play the explosion slower; the playback time scale is the target.");

        if (ImGui::Checkbox("Solver auto-scale", &settings.simulation.solverAutoScale)) {
            actions.applySimulationSettings = true;
        }
        if (settings.simulation.solverHemisphere) {
            Hint("Hemisphere mode masks the Cartesian solver volume to a semi-ellipsoidal "
                 "dome. The 3D texture stays cubic, so memory use is unchanged; the dome "
                 "uses the radial sponge and auto-scale.");
        }

        if (settings.simulation.solverResolution >= 256) {
            Hint("Solver grids of 256³ and 512³ need several GB of VRAM and are not "
                 "real-time; lower the time scale or grid.");
        }

        const gfx::SolverStats& stats = renderer.Solver();
        Readout("Solver time", "%.4f s", stats.time);
        Readout("Time step", "%.2f us", stats.dt * 1e6f);
        Readout("Max signal speed", "%.0f m/s", stats.maxSpeed);
        Readout("Solver steps", "%lld", stats.steps);
        Readout("Domain", "%.2f m half", stats.domainHalfExtent);
        Readout("Domain top", "%.2f m", stats.domainTop);
        Readout("Voxel size", "%.1f mm", stats.voxelSize * 1000.0f);
        Readout("Accuracy", "x%.2f", stats.accuracyFactor);
        Readout("Expansions", "%d", stats.expansions);
        if (stats.fieldBoundsValid) {
            Readout("Plume top", "%.2f m", stats.plumeTop);
            Readout("Plume radius", "%.2f m", stats.plumeRadius);
            if (settings.simulation.solverHemisphere) {
                Readout("Dome fill", "%.2f", stats.plumeDomeFraction);
            }
            if (stats.plumeTop > stats.domainTop * 0.9f) {
                Hint("The plume is close to the top of the solver domain. Auto-scale "
                     "expands it until the accuracy factor reaches x16.");
            }
        }

        if (renderer.SolverBudgetHit()) {
            Hint("Solver step budget reached: playback is slower than the requested "
                 "time scale. Raise Max steps/frame or lower the solver grid.");
        }

        Hint("Compressible Euler solver (HLLC + MUSCL) running on the GPU. It uses the "
             "moist-air density and virtual temperature; wind drifts the rendered volume. "
             "The solver supplies the fireball and smoke; scene fires on burning objects "
             "are still rendered.");
    }

    SectionHeader("BLAST PHYSICS");

    const sim::BlastModel& blast = simulation.Blast();
    const sim::BlastState& state = blast.State();

    Readout("Shock radius", "%.1f m", state.shockRadius);
    Readout("Shock speed", "%.0f m/s (M %.2f)", state.shockSpeed, state.shockMach);
    Readout("Peak overpressure", "%.2f kPa", state.shockOverpressurePa / 1000.0);
    Readout("Fireball radius", "%.1f m", state.fireballRadius);
    Readout("Fireball surface", "%.0f K", state.fireballTemperatureK);
    Readout("Cloud top", "%.0f m", blast.CloudTopHeightM());
    Readout("Cloud radius", "%.1f m", state.cloudRadius);
    Readout("Dust ring", "%.0f m", state.dustRingRadius);

    SectionHeader("PREDICTED EFFECTS");

    const double psi5 = blast.RadiusForOverpressure(34474.0);
    const double psi1 = blast.RadiusForOverpressure(6895.0);
    const double burn = blast.RadiusForThermalFluence(80000.0);

    Readout("5 psi (heavy damage)", "%.0f m", psi5);
    Readout("1 psi (window damage)", "%.0f m", psi1);
    Readout("3rd degree burns", "%.0f m", burn);

    SectionHeader("OBSERVER");

    double distance = settings.observerDistanceM;
    float distanceFloat = float(distance);
    if (ImGui::SliderFloat("Distance", &distanceFloat, 1.0f, 3000.0f, "%.1f m",
                           ImGuiSliderFlags_Logarithmic)) {
        distance = double(distanceFloat);
        settings.observerDistanceM = distance;
    }

    const double slant = blast.ObserverSlantRange(distance);
    const double arrival = blast.ArrivalTimeS(slant);
    const double peak = blast.PeakOverpressureAtDistance(slant);
    const double duration = blast.PositivePhaseDurationS(slant);

    Readout("Slant range", "%.0f m", slant);
    if (arrival < 0.0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        Readout("Shock arrival", "not yet");
        ImGui::PopStyleColor();
    } else {
        Readout("Shock arrival", "%.3f s", arrival);
    }
    Readout("Peak overpressure", "%.2f kPa (%.2f psi)", peak / 1000.0, peak / 6894.76);
    Readout("Positive phase", "%.1f ms", duration * 1000.0);

    // Friedlander pressure history at the observer.
    std::vector<float> waveform(192);
    float maxPressure = 0.0f;
    if (arrival >= 0.0) {
        const double start = std::max(0.0, arrival - duration * 0.25);
        const double end = arrival + duration * 1.8;
        for (size_t i = 0; i < waveform.size(); ++i) {
            const double t = start + (end - start) * double(i) / double(waveform.size() - 1);
            const float pressure = float(blast.OverpressureAtTime(slant, t) / 1000.0);
            waveform[i] = pressure;
            maxPressure = std::max(maxPressure, pressure);
        }
        maxPressure = std::max(maxPressure, 0.5f);
        ImGui::PlotLines("##waveform", waveform.data(), int(waveform.size()), 0, "kPa",
                         0.0f, maxPressure * 1.15f, ImVec2(-1.0f, ImGui::GetFontSize() * 4.5f));
    }

    Hint("Brode overpressure fit, Rankine-Hugoniot shock speed and a Friedlander "
         "waveform. Free-air burst, idealised.");
}

void DrawRendering(app::AppSettings& settings, UiActions& actions, const gfx::Renderer& renderer,
                   const sim::Simulation& simulation) {
    SectionHeader("RENDERING");

    gfx::RenderSettings& render = settings.render;
    ImGui::SliderFloat("Exposure", &render.exposure, 0.1f, 4.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Bloom strength", &render.bloomStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderInt("Ray steps", &render.raymarchSteps, 24, 192);

    static const int kAnisoValues[] = {0, 2, 4, 8};
    static const char* kAnisoLabels[] = {"Off", "2x", "4x", "8x"};
    int anisoIndex = 0;
    for (int i = 0; i < 4; ++i) {
        if (kAnisoValues[i] == render.anisotropicFiltering) {
            anisoIndex = i;
        }
    }
    if (ImGui::Combo("Anisotropic filtering", &anisoIndex, kAnisoLabels, 4)) {
        render.anisotropicFiltering = kAnisoValues[anisoIndex];
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Anisotropic sampling for the volumetric raymarch and the "
                          "procedural surfaces. Higher settings keep facades sharp at "
                          "grazing angles.");
    }

    const int resolutions[] = {64, 96, 128, 160, 192, 256, 512, 1024};
    const char* resolutionLabels[] = {"64³", "96³", "128³", "160³",
                                      "192³", "256³", "512³", "1024³"};
    int resolutionIndex = 2;
    for (int i = 0; i < 8; ++i) {
        if (resolutions[i] == settings.simulation.volumeResolution) {
            resolutionIndex = i;
        }
    }
    if (ImGui::Combo("Volume resolution", &resolutionIndex, resolutionLabels, 8)) {
        settings.simulation.volumeResolution = resolutions[resolutionIndex];
        actions.applySimulationSettings = true;
    }
    if (settings.simulation.volumeResolution >= 512) {
        Hint("Resolutions of 512³ and 1024³ require several GB of RAM/VRAM and are "
             "baked at a reduced rate.");
    }

    ImGui::Checkbox("Volume", &render.enableVolume);
    ImGui::SameLine();
    ImGui::Checkbox("Particles", &render.enableParticles);
    ImGui::Checkbox("Ground", &render.enableGround);
    ImGui::SameLine();
    ImGui::Checkbox("Bloom", &render.enableBloom);
    ImGui::Checkbox("Grid overlay", &render.showGrid);

    const bool highFidelity = settings.simulation.mode == sim::SimulationMode::HighFidelity;
    ImGui::BeginDisabled(highFidelity);
    if (ImGui::Checkbox("Auto-scale volume", &settings.simulation.autoScaleVolume)) {
        actions.applySimulationSettings = true;
    }
    if (ImGui::Checkbox("GPU volume bake", &settings.simulation.gpuVolumeBake)) {
        actions.applySimulationSettings = true;
    }
    if (ImGui::Checkbox("GPU particles", &settings.simulation.gpuParticles)) {
        actions.applySimulationSettings = true;
    }
    ImGui::EndDisabled();

    if (highFidelity) {
        Readout("Solver grid", "%d³", renderer.Solver().resolution);
        Hint("The Euler solver uses a fixed domain sized at detonation; auto-scale "
             "applies to the reduced-order volume.");
    } else {
        const sim::VolumeFrame& frame = simulation.Volume().Current();
        if (frame.resolution > 0) {
            const float sizeX = frame.bounds.max.x - frame.bounds.min.x;
            const float sizeY = frame.bounds.max.y - frame.bounds.min.y;
            const float sizeZ = frame.bounds.max.z - frame.bounds.min.z;
            Readout("Volume box", "%.1f × %.1f × %.1f m", sizeX, sizeY, sizeZ);
            Readout("Voxel size", "%.1f mm", (sizeX / float(frame.resolution)) * 1000.0f);
        }
        Hint("Auto-scale grows the baked volume to keep the fireball and smoke cloud "
             "inside it. The voxel count stays fixed, so the effective resolution drops "
             "as the box grows.");
    }

    ImGui::SliderFloat("Sun azimuth", &render.sunAzimuthDeg, 0.0f, 360.0f, "%.0f°");
    ImGui::SliderFloat("Sun elevation", &render.sunElevationDeg, 1.0f, 80.0f, "%.0f°");
    ImGui::SliderFloat("Particle scale", &render.particleScale, 0.25f, 2.0f, "%.2f");

    Readout("Volume texture", "%d³", renderer.VolumeTextureResolution());
}

void DrawPerformance(UiContext& context) {
    SectionHeader("PERFORMANCE");

    static const int kFpsChoices[] = {30, 60, 90, 120, 144};
    static const char* kFpsLabels[] = {"30", "60", "90", "120", "144"};
    int fpsIndex = 4;
    for (int i = 0; i < 5; ++i) {
        if (kFpsChoices[i] == context.settings.maxFps) {
            fpsIndex = i;
        }
    }
    if (ImGui::Combo("Max FPS", &fpsIndex, kFpsLabels, 5)) {
        context.settings.maxFps = kFpsChoices[fpsIndex];
    }
    Hint("Caps the interactive frame rate; lower caps leave more GPU time for the "
         "high-fidelity solver.");

    Readout("Frame rate", "%.1f fps", context.fps);
    Readout("Frame time", "%.2f ms", context.frameMs);
    Readout("CPU render", "%.2f ms", context.renderer.Info().cpuRenderMs);
    Readout("Volume bake", "%s", context.renderer.GpuVolumeBakeActive()
                                       ? "GPU"
                                       : "CPU");
    if (!context.renderer.GpuVolumeBakeActive()) {
        Readout("Bake time", "%.2f ms", context.simulation.Volume().LastBakeMilliseconds());
    }
    Readout("Model", "%s", context.renderer.Info().highFidelity ? "High-fidelity (GPU)"
                                                                : "Reduced-order");
    Readout("Worker threads", "%d", context.workerThreads);
    Readout("Particles", "%d / %d", context.simulation.Particles().AliveCount(),
            context.simulation.Particles().Capacity());
    Readout("Particle backend", "%s",
            context.simulation.Particles().GpuMode() ? "GPU" : "CPU");
    Readout("Debris", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Debris));
    Readout("Sparks", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Spark));
    Readout("Fire", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Fire));
    Readout("Smoke", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Smoke));
    Readout("Dust", "%d", context.simulation.Particles().AliveCount(sim::ParticleKind::Dust));
    Readout("GPU", "%s", context.adapterName);

    Hint("Particle integration and volume baking are distributed over a "
         "work-stealing thread pool; rendering runs on the GPU via Direct3D 11.");
}

} // namespace

void ApplyTheme(float dpiScale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(9.0f, 5.0f);
    style.ItemSpacing = ImVec2(9.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.ScrollbarRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.043f, 0.047f, 0.056f, 0.94f);
    style.Colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.055f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.13f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.055f, 0.065f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.065f, 0.075f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 0.52f, 0.16f, 0.55f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 0.52f, 0.16f, 0.80f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.52f, 0.16f, 0.85f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.62f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.52f, 0.16f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.55f, 0.60f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(1.0f, 0.58f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.72f, 0.35f, 1.0f);

    style.ScaleAllSizes(dpiScale);
}

void LoadFonts(float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();
    gChangelogFont = nullptr;
    const float fontSize = std::round(16.5f * dpiScale);

    ImFontConfig config{};
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = true;

    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", fontSize, &config,
                                               io.Fonts->GetGlyphRangesDefault());
    if (!font) {
        io.Fonts->AddFontDefault();
    }

    // The changelog viewer uses a font two points larger than the UI.
    const float changelogSize = std::round((16.5f + 2.0f) * dpiScale);
    gChangelogFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf", changelogSize, &config,
        io.Fonts->GetGlyphRangesDefault());
}

UiActions Draw(UiContext& context) {
    UiActions actions;

    app::AppSettings& settings = context.settings;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float panelWidth = ImGui::GetFontSize() * 25.0f;
    const float margin = ImGui::GetFontSize() * 0.85f;

    // Animate the control panel to the right when it is collapsed. The handle
    // rides on the panel's left edge, so it moves with the menu and still sits
    // at the window edge when the panel is fully closed.
    static bool panelSlideInitialized = false;
    static float panelSlide = 0.0f;
    if (!panelSlideInitialized) {
        panelSlide = settings.showUi ? 0.0f : 1.0f;
        panelSlideInitialized = true;
    }
    const float targetSlide = settings.showUi ? 0.0f : 1.0f;
    const float slideSpeed = std::min(1.0f, ImGui::GetIO().DeltaTime * 12.0f);
    panelSlide += (targetSlide - panelSlide) * slideSpeed;
    if (std::abs(targetSlide - panelSlide) < 0.001f) {
        panelSlide = targetSlide;
    }
    const float panelOffset = panelSlide * (panelWidth + margin);
    const float panelLeft =
        viewport->WorkPos.x + viewport->WorkSize.x - panelWidth - margin + panelOffset;
    const bool drawPanel = settings.showUi || panelSlide < 0.999f;

    if (drawPanel) {
        ImGui::SetNextWindowPos(ImVec2(panelLeft, viewport->WorkPos.y + margin),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelWidth, viewport->WorkSize.y - margin * 2.0f));

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
        ImGui::Begin("##control_panel", nullptr, flags);
        ImGui::PopStyleVar();

    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("EXPLOSION SIMULATOR");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::Text("v%s  ·  Physically based blast visualisation", EXPLOSION_VERSION_STRING);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    DrawUpdateBanner(context, actions);
    ImGui::Separator();

    // Scrollable content with a pinned footer so the changelog button is always
    // reachable at the very bottom of the panel.
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 3.0f;
    ImGui::BeginChild("##panel_content", ImVec2(0.0f, -footerHeight), false);

    if (ImGui::CollapsingHeader("Detonation", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawDetonation(settings, context.simulation, actions);
    }
    if (ImGui::CollapsingHeader("Atmosphere")) {
        DrawAtmosphere(settings, actions);
    }
    if (ImGui::CollapsingHeader("Environment")) {
        DrawEnvironment(settings, context.simulation, actions);
    }
    if (ImGui::CollapsingHeader("Playback")) {
        DrawPlayback(settings, actions, context.simulation);
    }
    if (ImGui::CollapsingHeader("Physics & Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawPhysics(settings, context.simulation, context.renderer, actions);
    }
    if (ImGui::CollapsingHeader("Rendering")) {
        DrawRendering(settings, actions, context.renderer, context.simulation);
    }
    if (ImGui::CollapsingHeader("Performance")) {
        DrawPerformance(context);
    }
    if (ImGui::CollapsingHeader("Updates")) {
        DrawUpdates(context, settings, actions);
    }
    ImGui::EndChild();

    ImGui::Separator();
    Hint("Space pause | D detonate | R rearm | F frame | H cycle UI");
    if (ImGui::Button("Changelog##footer", ImVec2(-1.0f, 0.0f))) {
        actions.showChangelog = true;
    }
    ImGui::End();
    }

    // --- Panel handle ------------------------------------------------------
    // Attached to the panel's left edge instead of the window edge, so it
    // travels with the menu while collapsing and expanding.
    const float handleWidth = ImGui::GetFontSize() * 1.15f;
    const float handleHeight = ImGui::GetFontSize() * 5.2f;
    const ImVec2 handlePos(panelLeft - handleWidth,
                           viewport->WorkPos.y + viewport->WorkSize.y * 0.5f -
                               handleHeight * 0.5f);
    ImGui::SetNextWindowPos(handlePos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(handleWidth, handleHeight), ImGuiCond_Always);
    const ImGuiWindowFlags handleFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##panel_handle", nullptr, handleFlags);
    ImGui::InvisibleButton("##panel_handle_click", ImVec2(handleWidth, handleHeight));
    const bool handleHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        settings.showUi = !settings.showUi;
    }
    if (handleHovered) {
        ImGui::SetTooltip(settings.showUi ? "Collapse menu" : "Open menu");
    }
    ImDrawList* handleDraw = ImGui::GetWindowDrawList();
    const ImVec2 handleMax(handlePos.x + handleWidth, handlePos.y + handleHeight);
    handleDraw->AddRectFilled(handlePos, handleMax, IM_COL32(28, 30, 36, 235), 6.0f);
    if (handleHovered) {
        handleDraw->AddRect(handlePos, handleMax, IM_COL32(255, 132, 40, 210), 6.0f,
                            0, 1.5f);
    }
    const float gripX = handlePos.x + handleWidth * 0.5f;
    const float gripY = handlePos.y + handleHeight * 0.5f;
    for (int i = -1; i <= 1; ++i) {
        const float y = gripY + float(i) * ImGui::GetFontSize() * 0.5f;
        handleDraw->AddLine(ImVec2(gripX - ImGui::GetFontSize() * 0.28f, y),
                            ImVec2(gripX + ImGui::GetFontSize() * 0.28f, y),
                            IM_COL32(220, 226, 236, 230), 1.5f);
    }
    ImGui::End();
    ImGui::PopStyleVar();

    if (settings.showOverlays && settings.render.showGrid) {
        DrawScaleBar(context.camera, ImGui::GetIO().DisplaySize);
    }

    // --- On-screen overlays -------------------------------------------------
    const ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;

    if (settings.showOverlays) {
    if (context.simulation.IsArmed()) {
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                   viewport->WorkPos.y + viewport->WorkSize.y * 0.12f),
            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::Begin("##armed_overlay", nullptr,
                     overlayFlags | ImGuiWindowFlags_NoBackground);
        ImGui::SetWindowFontScale(1.1f);
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        ImGui::TextUnformatted(context.simulation.IsFuseHeld() ? "CHARGE READY"
                                                              : "CHARGE ARMED");
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(2.4f);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        if (context.simulation.IsFuseHeld()) {
            ImGui::TextUnformatted("READY");
        } else {
            ImGui::Text("DETONATION IN %.2f s", context.simulation.FuseRemaining());
        }
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
    }

    // Data box while overlays are enabled; the countdown is a separate centred
    // overlay above.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + margin, viewport->WorkPos.y + margin),
                            ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.115f, 0.125f, 0.88f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##timecode", nullptr, overlayFlags);

    char massText[64];
    FormatMass(context.settings.simulation.chargeMassKg, massText, sizeof(massText));
    char timeText[64];
    snprintf(timeText, sizeof(timeText), "%.4f s", context.simulation.Time());
    char shockText[64];
    snprintf(shockText, sizeof(shockText), "%.1f m",
             context.simulation.Blast().State().shockRadius);
    char fpsText[64];
    snprintf(fpsText, sizeof(fpsText), "%.1f fps", context.fps);

    const float labelWidth = ImGui::GetFontSize() * 6.8f;
    const auto dataRow = [&](const char* label, const char* value) {
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SameLine(labelWidth);
        ImGui::TextUnformatted(value);
    };
    dataRow("Time", timeText);
    dataRow("TNT", massText);
    dataRow("Shock front", shockText);
    dataRow("FPS", fpsText);

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    ImGui::End();
    }

    // Scene generation is synchronous on the worker thread; keep the window
    // responsive and show a full-width progress strip along the bottom edge.
    if (context.simulation.SceneGenerating()) {
        ImDrawList* foreground = ImGui::GetForegroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float height = display.y * 0.05f;
        const float top = display.y - height;
        foreground->AddRectFilled(ImVec2(0.0f, top), display,
                                  IM_COL32(14, 16, 19, 235));
        const float progress =
            std::clamp(context.simulation.SceneProgress(), 0.0f, 1.0f);
        if (progress > 0.0f) {
            foreground->AddRectFilled(ImVec2(0.0f, top),
                                      ImVec2(display.x * progress, display.y),
                                      IM_COL32(52, 199, 89, 255));
        }
        char label[64];
        snprintf(label, sizeof(label), "Generating scene  %d%%",
                 int(progress * 100.0f + 0.5f));
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        foreground->AddText(ImVec2((display.x - textSize.x) * 0.5f,
                                   top + (height - textSize.y) * 0.5f),
                            IM_COL32(245, 248, 250, 255), label);
    }

    DrawUpdateModals(context, actions);

    return actions;
}

} // namespace ex::ui
