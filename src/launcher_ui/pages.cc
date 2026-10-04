#include "launcher_ui/pages.h"

#include <cstring>

#include "launcher_ui/i18n.h"

namespace mocktail::launcher_ui {

const std::array<SectionInfo, kSectionCount>& Sections() {
  // Icons: Adwaita symbolic icons outside the legacy set (research/ux.md
  // 4.1 lists the intent; view-fullscreen and network-transmit-receive
  // replace the legacy preferences-desktop-display and the Wi-Fi-only
  // network-wireless).
  static const std::array<SectionInfo, kSectionCount> sections = {{
      {Section::kGraphics, "graphics", N_("Graphics"), "video-display-symbolic",
       0, BuildGraphicsPage},
      {Section::kDisplay, "display", N_("Display"), "view-fullscreen-symbolic",
       0, BuildDisplayPage},
      {Section::kPerformance, "performance", N_("Performance"),
       "power-profile-performance-symbolic", 0, BuildPerformancePage},
      {Section::kAudio, "audio", N_("Audio"), "audio-speakers-symbolic", 0,
       BuildAudioPage},
      {Section::kAccounts, "accounts", N_("Accounts"),
       "avatar-default-symbolic", 1, BuildAccountsPage},
      {Section::kIntegrations, "integrations", N_("Integrations"),
       "application-x-addon-symbolic", 1, BuildIntegrationsPage},
      {Section::kNetworkUpdates, "network-updates", N_("Network & Updates"),
       "network-transmit-receive-symbolic", 1, BuildNetworkUpdatesPage},
      {Section::kAdvanced, "advanced", N_("Advanced"),
       "preferences-other-symbolic", 1, BuildAdvancedPage},
      {Section::kAbout, "about", N_("About"), "help-about-symbolic", 2,
       BuildAboutPage},
  }};
  return sections;
}

const SectionInfo& GetSectionInfo(Section section) {
  return Sections()[static_cast<std::size_t>(section)];
}

const SectionInfo* FindSection(const char* id) {
  if (id == nullptr) return nullptr;
  for (const SectionInfo& info : Sections()) {
    if (std::strcmp(info.id, id) == 0) return &info;
  }
  return nullptr;
}

}  // namespace mocktail::launcher_ui
