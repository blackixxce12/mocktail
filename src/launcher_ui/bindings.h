#ifndef MOCKTAIL_LAUNCHER_UI_BINDINGS_H_
#define MOCKTAIL_LAUNCHER_UI_BINDINGS_H_

#include <adwaita.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "launcher/config_document.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/machine_profile.h"
#include "launcher_ui/pages.h"

// Rows bound to config.yaml keys, with the hint system every setting uses.
//
// A bound row reads the effective value (the draft's value, else the
// first-run template's, else RowSpec::fallback), writes changes into the
// context's draft (never to disk: Save and Play do that), and keeps itself
// up to date when anything changes. Each one automatically gets:
//   - its subtitle from the Hint (current effect, selected option's
//     description, "Recommended for this computer: X" when the value is
//     not X, the overriding environment variable and an inline warning,
//     one per line);
//   - an info button (dialog-information-symbolic, accessible label
//     "Learn more about <title>") opening a popover with the details;
//   - an "ENV" badge while a managed environment variable overrides the key;
//   - an "Overrides Roblox setting" badge while the value takes over one of
//     Roblox's own settings (Hint::overrides_roblox);
//   - a reset button while the value differs from the default;
//   - a search entry (title, subtitle, section, keywords, the config.yaml
//     key and every environment variable that overrides it);
//   - while config.yaml is broken (read-only draft), everything but the
//     info button insensitive, so the hint can still be read.
//
// Wording rules (all strings through _()): titles in sentence case,
// subtitles one short line without a final period, details in plain
// sentences. Never claim an effect the code does not have; put the source
// (file:line or research section) of a non-obvious claim in a comment next
// to the text.
namespace mocktail::launcher_ui {

// What a value takes over from Roblox's own settings (roblox_decides.h).
struct RobloxOverrideNote {
  // One line: the badge's tooltip and accessible description.
  std::string summary;
  // What exactly is overridden, and through what (the popover).
  std::string details;
  // How to give Roblox the setting back (the popover).
  std::string give_back;
};

// What a row tells the user. Everything is optional, but every setting row
// should have a subtitle (fixed or dynamic) and details.
struct Hint {
  // One line under the title. For combo rows the selected option's
  // description is used when this and subtitle_for are empty.
  std::string subtitle;
  // The current effect, from the effective value ("Renders 2560 × 1440
  // at 160 %"). Wins over `subtitle` and the option description.
  std::function<std::string(LauncherContext& context, const std::string& value)>
      subtitle_for;
  // "Learn more": what the setting does in Mocktail/Roblox terms, its effect
  // on performance, image quality, stability and input latency, when to
  // change it, the recommended value and the risks. 2-6 short paragraphs
  // separated by a blank line; a line starting with "• " is a list item.
  // Plain text, no markup.
  std::string details;
  // Extra details computed when the popover opens (what was found on this
  // computer). Appended after `details`.
  std::function<std::string(LauncherContext& context)> details_for;
  // The value recommended for this computer (as written to config.yaml),
  // nullopt when no recommendation applies.
  std::function<std::optional<std::string>(const MachineProfile& machine)>
      recommend;
  // One sentence saying why.
  std::function<std::string(const MachineProfile& machine)> recommend_reason;
  // A warning about the effective value on this computer, empty when none.
  // Shown inline (warning color) and in the popover.
  std::function<std::string(LauncherContext& context, const std::string& value)>
      warning;
  // What the current settings take over from Roblox through this row,
  // nullopt when nothing. The row then shows an "Overrides Roblox setting"
  // badge under its subtitle (style class override-badge) and the popover
  // says what is overridden and how to give it back. Rows use
  // RobloxOverrideHint() (roblox_decides.h), which works it out from the
  // settings the game reads this launch.
  std::function<std::optional<RobloxOverrideNote>(LauncherContext& context)>
      overrides_roblox;
};

struct RowSpec {
  // Dotted config.yaml key ("graphics.vsync"). Empty for rows not bound to
  // a key (DecorateRow).
  std::string key;
  // Translated, sentence case: _("Vertical sync").
  std::string title;
  Hint hint;
  // Search synonyms, English and Russian, lower case ("fps", "кадры"). The
  // key and the environment variables that override it are added.
  std::vector<std::string> keywords;
  // The runtime's own default for a key the template leaves commented out
  // (graphics.vsync: "auto", graphics.frame_rate_limit: "-1"). Used when
  // config.yaml has no value; reset then removes the key.
  std::string fallback;
  // Show the reset button while the value differs from the default.
  bool resettable = true;
  // Why the row cannot be changed right now ("Fleasion cannot be combined
  // with the system proxy"); empty when it can. Everything in the row but
  // its info button is then insensitive, and the reason is its last
  // subtitle line. Re-evaluated on every setting or machine change, so it
  // may depend on other rows. Do not call gtk_widget_set_sensitive() on
  // bound rows or their suffixes yourself: the binding owns their
  // sensitivity.
  std::function<std::string(LauncherContext& context)> unavailable;
};

struct ComboOption {
  // Written to config.yaml when chosen.
  std::string value;
  std::string label;
  // One line: shown under the label in the list and as the row subtitle
  // when selected.
  std::string description;
  // Other config.yaml spellings with the same effect ("gles" for opengl).
  std::vector<std::string> aliases;
  // Computed description (overrides `description`), e.g. naming what was
  // found on this computer.
  std::function<std::string(LauncherContext& context)> describe;
  // Why the option cannot be chosen here; empty when it can. Unavailable
  // options stay listed, insensitive, with the reason as description.
  std::function<std::string(LauncherContext& context)> unavailable;
  // Listed only while it is the current value (spellings that behave
  // differently, diagnostics-only values).
  bool hidden = false;
  // "Custom…": choosing it calls ComboSpec::on_custom and writes nothing.
  bool custom = false;
};

struct ComboSpec {
  std::vector<ComboOption> options;
  // How values are written; ScalarKindFor(key, value) when unset.
  std::optional<launcher::ScalarKind> kind;
  // Search inside the list (long lists such as audio devices).
  bool enable_search = false;
  // The "Custom…" option was chosen (reveal a spin row, open a dialog).
  std::function<void(LauncherContext& context)> on_custom;
  // Options that depend on this computer or on other settings (audio
  // devices, the display's refresh rate). When set, the list is computed
  // again on every refresh and replaces `options`.
  std::function<std::vector<ComboOption>(LauncherContext& context)> options_for;
};

struct SpinSpec {
  double minimum = 0;
  double maximum = 100;
  double step = 1;
  // Shown while config.yaml has no value and there is no default.
  std::optional<double> unset_value;
};

struct EntrySpec {
  // A problem with the text, empty when it can be saved. While there is one
  // the row is marked as an error and Save/Play are disabled.
  std::function<std::string(const std::string& text)> validate;
  // An empty entry removes the key instead of writing "".
  bool empty_unsets = false;
  // How the text is written (free text is double-quoted).
  launcher::ScalarKind kind = launcher::ScalarKind::kString;
};

// ---- bound rows
// ---------------------------------------------------------------- Each returns
// the row widget (an AdwPreferencesRow) to add to a group.

// AdwSwitchRow for a true/false key.
GtkWidget* BindSwitchRow(LauncherContext* context, RowSpec spec);
// AdwComboRow: one option per value; a value config.yaml holds that no
// option matches is listed as "<value> (from config.yaml)".
GtkWidget* BindComboRow(LauncherContext* context, RowSpec spec,
                        ComboSpec combo);
// AdwSpinRow for an integer key.
GtkWidget* BindSpinRow(LauncherContext* context, RowSpec spec, SpinSpec spin);
// AdwEntryRow for a text key.
GtkWidget* BindEntryRow(LauncherContext* context, RowSpec spec,
                        EntrySpec entry);

// For rows the page drives itself (navigation rows, buttons, read-only
// information): adds the subtitle, the info button and the search entry the
// bound rows get. `row` is an AdwActionRow (or subclass) or an
// AdwExpanderRow; spec.key may be empty. Returns `row`.
GtkWidget* DecorateRow(LauncherContext* context, GtkWidget* row, RowSpec spec);

// Re-evaluates every row's subtitle, badges and value (after a page changed
// something the rows depend on outside the draft).
void RefreshRows(LauncherContext* context);

// ---- pages and groups
// --------------------------------------------------------

// An AdwPreferencesPage for `section` with a one-line intro saying what
// the section is for (shown at the top of the page).
GtkWidget* NewPage(LauncherContext* context, Section section,
                   const std::string& intro);
// An AdwPreferencesGroup added to `page`, with a title and a one-line
// intro (the group description).
GtkWidget* AddGroup(GtkWidget* page, const std::string& title,
                    const std::string& intro);
void AddRow(GtkWidget* group, GtkWidget* row);

// ---- building blocks
// -----------------------------------------------------------

// Escapes text for AdwPreferencesRow titles and subtitles, which are Pango
// markup.
std::string Markup(std::string_view text);
// Wraps markup so that a word broken across lines gets no hyphen. Rows and
// hints wrap at any character when a word does not fit, and Pango then
// adds a hyphen, which inside a variable, a path or a Wayland global reads
// as part of it ("MOCKTAIL_GRAPHICS_BACKEND=dire-" / "ct-vulkan").
std::string WithoutHyphens(std::string_view markup);
// A small pill reading "Recommended" (style class recommended-badge).
GtkWidget* NewRecommendedBadge();
// A small pill reading "ENV" (style class env-badge).
GtkWidget* NewEnvBadge();
// A small pill with a game controller icon reading "Overrides Roblox
// setting" (style class override-badge), the ENV badge's sibling.
// `summary` becomes its tooltip and accessible description.
GtkWidget* NewRobloxOverrideBadge(const std::string& summary = {});
void SetRobloxOverrideBadgeSummary(GtkWidget* badge,
                                   const std::string& summary);
// Only the icon, for a badge among a row's suffixes in the narrow layout;
// the accessible label stays.
void SetRobloxOverrideBadgeCompact(GtkWidget* badge, bool compact);
// An inline warning label (style class launcher-warning): wrapped text in
// the theme's warning color with a warning icon, for use outside rows.
GtkWidget* NewWarningLabel(const std::string& text);
// Gives `label` the width its first `characters` characters take as they
// render (all of its text when shorter) as its minimum, so a value up to
// that length is never ellipsized. gtk_label_set_width_chars() counts an
// average Latin character, which cut Cyrillic values of that length
// ("Автоматичес…"). 0 removes the minimum.
void SetMinimumTextWidth(GtkWidget* label, long characters);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_BINDINGS_H_
