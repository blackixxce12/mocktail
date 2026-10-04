#ifndef MOCKTAIL_LAUNCHER_UI_PAGE_WIDGETS_H_
#define MOCKTAIL_LAUNCHER_UI_PAGE_WIDGETS_H_

#include <adwaita.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "launcher_ui/launcher_context.h"
#include "launcher_ui/page_rules.h"

// Building blocks shared by the Integrations, Network & Updates, Advanced
// and About pages: buttons and rows that run an action, a file chooser for
// path entries, a choice row for settings spread over several keys, and the
// Fleasion/proxy state both the Integrations and the Network page check.
namespace mocktail::launcher_ui {

// Runs `fn` after every setting or machine change until `owner` is
// destroyed (rows built inside dialogs die before the context).
void FollowContext(LauncherContext* context, GtkWidget* owner,
                   std::function<void()> fn);

// A flat button with `label`, vertically centred for a row suffix.
GtkWidget* NewRowButton(const std::string& label, std::function<void()> fn);
// A flat circular icon button; `label` is its tooltip and accessible name.
GtkWidget* NewRowIconButton(const char* icon_name, const std::string& label,
                            std::function<void()> fn);

// An AdwActionRow that runs `fn` when activated (click, Enter), ending in
// `icon_name` (go-next-symbolic for a subpage, folder-open-symbolic, ...).
GtkWidget* NewActionRow(const char* icon_name, std::function<void()> fn);

// Adds a "Choose File…" button to a bound AdwEntryRow: the file picked is
// written to `key` (no file dialog during the self-test).
void AddFileChooserButton(LauncherContext* context, GtkWidget* entry_row,
                          std::string key, std::string dialog_title);

struct Choice {
  std::string label;
  // One line, shown under the label in the list.
  std::string description;
  // Why it cannot be chosen now; empty when it can.
  std::function<std::string(LauncherContext& context)> unavailable;
};

// An AdwComboRow for a choice stored in several keys (the proxy mode).
// `current` tells which choice the settings select now; `choose` applies a
// choice. Decorate it with DecorateRow() for the hint, badge and search.
GtkWidget* NewChoiceRow(
    LauncherContext* context, std::vector<Choice> choices,
    std::function<int(LauncherContext& context)> current,
    std::function<void(LauncherContext& context, int)> choose);

// Fleasion and the network proxy as config.yaml will hold them.
FleasionInputs FleasionInputsFrom(const LauncherContext& context);
// Why Fleasion cannot be combined with the proxy settings (translated);
// empty for FleasionConflict::kNone.
std::string DescribeFleasionConflict(FleasionConflict conflict);

// `path` with the home folder written as "~".
std::string DisplayPath(const std::filesystem::path& path,
                        const std::filesystem::path& home);

// The pseudo key RobloxStatus notifies when the installed or newest Roblox
// version changes (rows showing them refresh like on a setting change).
inline constexpr char kRobloxStatusKey[] = "@roblox-status";

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_PAGE_WIDGETS_H_
