#include "launcher_ui/recommendations.h"

namespace mocktail::launcher_ui {

BackendRecommendation RecommendGraphicsBackend(const MachineProfile& machine) {
  if (!machine.detected) {
    // direct-vulkan is the default (runtime_config.h).
    return {"direct-vulkan", BackendRecommendationReason::kUnknown};
  }
  // ANGLE is never recommended: it adds a translation layer, is
  // experimental on NVIDIA (upstream issue #149) and depends on a browser's
  // libraries (research/graphics.md 1.3, 1.7).
  if (machine.has_vulkan_driver()) {
    return {"direct-vulkan", BackendRecommendationReason::kVulkanDriver};
  }
  return {"opengl", BackendRecommendationReason::kNoVulkanDriver};
}

}  // namespace mocktail::launcher_ui
