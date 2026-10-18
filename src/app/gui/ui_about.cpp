#include "app/gui/ui_widgets.hpp"
#include "core/version.hpp"

namespace gao::gui {

// ------------------------------------------------------------------ about

void about(const UiState& s, const UiActions& act) {
    begin_card("about");
    text_bold("GPU Auto Optimizer", 1.6f);
    dim(("Version " + std::string(kVersion)).c_str());
    ImGui::Spacing();
    wrapped(kText, "Tunes your NVIDIA card (clocks, power limit, undervolt and fan curve) and keeps the result applied. Every "
                   "setting it writes is read back and checked, a failed apply ends at stock, and a crash journal makes sure a "
                   "setting that froze the machine is never tried again.");
    ImGui::Spacing();
    wrapped(kDim, "Settings and logs: %ProgramData%\\GpuAutoOptimizer (send run.log with a problem report)");
    wrapped(kDim, "Command line: gao.exe --help");
    wrapped(kDim, "Source: github.com/Rovey/gpu-auto-optimizer. MIT License.");
    ImGui::Spacing();
    ImGui::Spacing();
    if (toggle("##updates", s.update_check)) act.set_update_check(!s.update_check);
    ImGui::SameLine(0, em() * 0.8f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Look for a new version when the app starts");
    wrapped(kDim, "One request to github.com at start and once a day; nothing else is sent. A new version is shown at the bottom "
                  "left and installed only when you click it.");
    end_card();
}

}
