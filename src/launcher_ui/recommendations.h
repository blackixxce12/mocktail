#ifndef MOCKTAIL_LAUNCHER_UI_RECOMMENDATIONS_H_
#define MOCKTAIL_LAUNCHER_UI_RECOMMENDATIONS_H_

#include <string>

#include "launcher_ui/machine_profile.h"

namespace mocktail::launcher_ui {

// Machine-aware recommendations, kept free of GTK and of wording so they
// are unit-tested; the pages turn the reason into a translated sentence.

enum class BackendRecommendationReason {
  // A Vulkan driver for this GPU is installed: direct Vulkan.
  kVulkanDriver,
  // No hardware Vulkan driver was found: OpenGL ES works with any
  // OpenGL ES 3.0 driver.
  kNoVulkanDriver,
  // Detection has not finished; the default is recommended.
  kUnknown,
};

struct BackendRecommendation {
  // A canonical graphics.backend value: direct-vulkan or opengl.
  std::string value;
  BackendRecommendationReason reason = BackendRecommendationReason::kUnknown;
};

BackendRecommendation RecommendGraphicsBackend(const MachineProfile& machine);

}  // namespace mocktail::launcher_ui

#endif  // MOCKTAIL_LAUNCHER_UI_RECOMMENDATIONS_H_
