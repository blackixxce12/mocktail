#ifndef MOCKTAIL_LAUNCHER_UI_SETTING_KINDS_H_
#define MOCKTAIL_LAUNCHER_UI_SETTING_KINDS_H_

#include <string_view>

#include "launcher/config_document.h"

namespace mocktail::launcher_ui {

// How a value for config.yaml key `key` is written: booleans as true/false,
// numbers plain, free text (titles, device names, paths, Discord texts)
// double-quoted, everything else as a plain token. Keys that take a number
// or a word (graphics.frame_rate_limit, engine.graphics_quality) follow the
// value. Unknown keys are treated as tokens unless the value is a number.
launcher::ScalarKind ScalarKindFor(std::string_view key,
                                   std::string_view value);

// True for the keys whose values are free text.
bool IsTextKey(std::string_view key);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_SETTING_KINDS_H_
