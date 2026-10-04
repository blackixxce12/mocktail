#include <optional>
#include <string>

#include "launcher_ui/bindings.h"
#include "launcher_ui/i18n.h"
#include "launcher_ui/launcher_context.h"
#include "launcher_ui/pages.h"
#include "launcher_ui/recommendations.h"

namespace mocktail::launcher_ui {
namespace {

bool IsVulkanSpelling(const std::string& value) {
  return value == "direct-vulkan" || value == "vulkan" ||
         value == "native-vulkan";
}

// The worked example of a fully hinted row: every option described, the
// recommendation computed from the machine, warnings for what can go wrong
// here, and details that say what was found on this computer. Sources for
// each claim are in the comments; research/graphics.md 1 has the details.
GtkWidget* BuildGraphicsBackendRow(LauncherContext* context) {
  RowSpec spec;
  spec.key = "graphics.backend";
  spec.title = _("Graphics backend");
  // runtime_config.h: direct-vulkan is the built-in default.
  spec.fallback = "direct-vulkan";
  spec.keywords = {"vulkan", "opengl",    "gles",   "angle",      "renderer",
                   "render", "gpu",       "driver", "--graphics", "графика",
                   "рендер", "отрисовка", "движок", "драйвер",    "видеокарта"};
  spec.hint.details =
      // research/graphics.md 1.3-1.5.
      _("Chooses how Roblox draws the game. The change applies the next time "
        "Roblox starts.") +
      std::string("\n\n") +
      // bionic_vulkan_loader_adapter.cc: present modes are chosen only in
      // the Vulkan adapter; vulkan_etc2_emulation.cc decodes ETC2 when the
      // device lacks textureCompressionETC2.
      _("• Vulkan: Roblox's own Vulkan renderer talks to your graphics "
        "driver directly. It is the default, and the only backend where "
        "Vertical sync chooses how frames are presented. When the driver "
        "lacks the ETC2 texture format Android games use (common on desktop "
        "NVIDIA and AMD cards), Mocktail decodes those textures itself.") +
      "\n\n" +
      // stubs/libegl_stub.cc eglSwapInterval is a no-op; Mocktail never
      // calls SDL_GL_SetSwapInterval (research/graphics.md 1.5).
      _("• OpenGL ES: Roblox's OpenGL ES 3.0 renderer on your system's EGL "
        "driver. Use it when Vulkan is missing or crashes. Vertical sync has "
        "no effect here: frames follow the driver's default, which normally "
        "waits for the display.") +
      "\n\n" +
      // window.cc FindInstalledAngleLibraries; upstream issue #149.
      _("• ANGLE on Vulkan: the OpenGL ES renderer, translated to Vulkan by "
        "the ANGLE libraries of a Chromium or Electron installed on this "
        "computer. It adds a translation layer, is experimental on NVIDIA "
        "(upstream issue #149), and can break when that browser updates. Try "
        "it only when Vulkan shows glitches and OpenGL ES does not work.") +
      "\n\n" +
      // config/mocktail.example.yaml updates.automatic: the canaries run
      // with the selected graphics backend.
      _("Automatic Roblox updates test a new Roblox version with the backend "
        "chosen here before switching to it, so a version that fails with it "
        "is not installed.");
  spec.hint.details_for = [](LauncherContext& ctx) {
    const MachineProfile& machine = ctx.machine();
    if (!machine.detected) return std::string();
    std::string text;
    text += machine.has_vulkan_driver()
                ? Format(_("Vulkan driver on this computer: %s"),
                         machine.vulkan_icd.c_str())
                : std::string(_("No Vulkan driver for your graphics card was "
                                "found in the usual places "
                                "(/usr/share/vulkan/icd.d)."));
    text += "\n\n";
    if (machine.angle.has_value()) {
      text += machine.angle->bundled
                  ? Format(_("ANGLE for “ANGLE on Vulkan”: Mocktail's own copy "
                             "in %s."),
                           machine.angle->directory.c_str())
                  : Format(_("ANGLE for “ANGLE on Vulkan”: %s in %s."),
                           machine.angle->label.c_str(),
                           machine.angle->directory.c_str());
    } else {
      text +=
          _("No ANGLE libraries were found, so “ANGLE on Vulkan” is not "
            "available.");
    }
    if (machine.NvidiaDirectVulkanUsesX11()) {
      // video_driver_policy.cc rule 4 and the reason in
      // video_driver_policy.h.
      text += "\n\n";
      text +=
          _("With Vulkan on NVIDIA, the automatic display server runs "
            "the game through XWayland, because NVIDIA's native Wayland "
            "path can hang or lose the display. Display › Display server "
            "can change that.");
    }
    return text;
  };
  spec.hint.recommend = [](const MachineProfile& machine) {
    return std::optional<std::string>(RecommendGraphicsBackend(machine).value);
  };
  spec.hint.recommend_reason = [](const MachineProfile& machine) {
    switch (RecommendGraphicsBackend(machine).reason) {
      case BackendRecommendationReason::kVulkanDriver: {
        const std::string vendors = machine.GpuVendorsLabel();
        return vendors.empty()
                   ? std::string(_("A Vulkan driver is installed, and Vulkan "
                                   "is Mocktail's default and most direct "
                                   "path."))
                   : Format(_("A Vulkan driver for your %s graphics is "
                              "installed, and Vulkan is Mocktail's default "
                              "and most direct path."),
                            vendors.c_str());
      }
      case BackendRecommendationReason::kNoVulkanDriver:
        return std::string(
            _("No Vulkan driver was found, and OpenGL ES works "
              "with any driver that supports OpenGL ES 3.0."));
      case BackendRecommendationReason::kUnknown:
        break;
    }
    return std::string();
  };
  spec.hint.warning = [](LauncherContext& ctx, const std::string& value) {
    const MachineProfile& machine = ctx.machine();
    if (value == "vulkan" || value == "native-vulkan") {
      // window.cc opens the Vulkan window only for the exact name
      // direct-vulkan (research/graphics.md 1.2).
      return std::string(
          _("This spelling starts an OpenGL window instead of "
            "the Vulkan one. Choose Vulkan to fix it."));
    }
    if (IsVulkanSpelling(value) && machine.detected &&
        !machine.has_vulkan_driver()) {
      return std::string(
          _("No Vulkan driver was found for your graphics "
            "card, so Roblox may fail to start. Install one or "
            "choose OpenGL ES."));
    }
    if (value == "angle-vulkan") {
      if (machine.detected && !machine.angle.has_value()) {
        return std::string(
            _("No ANGLE libraries were found, so Roblox cannot "
              "start with this backend."));
      }
      if (machine.gpu.nvidia) {
        return std::string(
            _("Experimental on NVIDIA: ANGLE on Vulkan has "
              "failed there before (upstream issue #149)."));
      }
      if (machine.angle.has_value() && !machine.angle->looks_like_angle) {
        return Format(_("The libraries in %s do not look like ANGLE. Roblox "
                        "checks them at start and stops if they are not."),
                      machine.angle->directory.c_str());
      }
    }
    if (value == "angle-swiftshader") {
      // window.cc: SwiftShader is ANGLE's CPU renderer.
      return std::string(
          _("Software rendering on the processor: very slow, "
            "only for diagnosing problems."));
    }
    if ((value == "opengl" || value == "gles" || value == "system" ||
         value == "auto") &&
        ctx.EffectiveValue("graphics.vsync", "auto") != "auto") {
      return std::string(
          _("Vertical sync is set, but it has no effect with "
            "OpenGL ES."));
    }
    return std::string();
  };

  ComboSpec combo;
  combo.options = {
      {"direct-vulkan",
       _("Vulkan"),
       _("Roblox's Vulkan renderer, straight to your graphics driver"),
       {},
       nullptr,
       nullptr,
       false,
       false},
      {"opengl",
       _("OpenGL ES"),
       _("For a missing or broken Vulkan driver; Vertical sync has no effect"),
       // runtime_config.cc ParseGraphicsBackend: gles is the same strict
       // EGL path.
       {"gles"},
       nullptr,
       nullptr,
       false,
       false},
      {"angle-vulkan",
       _("ANGLE on Vulkan"),
       {},
       {},
       [](LauncherContext& ctx) {
         const MachineProfile& machine = ctx.machine();
         if (machine.angle.has_value() && !machine.angle->bundled) {
           return Format(_("OpenGL ES translated to Vulkan by the ANGLE of "
                           "%s; experimental"),
                         machine.angle->label.c_str());
         }
         return std::string(
             _("OpenGL ES translated to Vulkan by ANGLE; "
               "experimental"));
       },
       [](LauncherContext& ctx) {
         const MachineProfile& machine = ctx.machine();
         return machine.detected && !machine.angle.has_value()
                    ? std::string(_("No Chromium or Electron ANGLE libraries "
                                    "were found on this computer"))
                    : std::string();
       },
       false,
       false},
      // Listed only while config.yaml holds them.
      {"system",
       _("OpenGL ES with ANGLE retry"),
       // window.cc RetryWithAutoAngleFallback; "auto" is treated the same.
       _("OpenGL ES; if the window cannot start, one retry with ANGLE on "
         "Vulkan"),
       {"auto"},
       nullptr,
       nullptr,
       true,
       false},
      {"angle-swiftshader",
       _("ANGLE SwiftShader (software)"),
       _("Draws on the processor instead of the graphics card; very slow"),
       {},
       nullptr,
       nullptr,
       true,
       false},
  };
  return BindComboRow(context, std::move(spec), std::move(combo));
}

}  // namespace

// Graphics. Owned by the graphics page; the backend row is the reference
// for how hints are written.
GtkWidget* BuildGraphicsPage(LauncherContext* context) {
  GtkWidget* page = NewPage(context, Section::kGraphics,
                            _("How Roblox draws the game: the graphics "
                              "backend, frame rate and quality"));
  GtkWidget* renderer =
      AddGroup(page, _("Renderer"),
               _("Which graphics path Roblox uses on this computer"));
  AddRow(renderer, BuildGraphicsBackendRow(context));
  // TODO(graphics page) research/ux.md 4.3 GRAPHICS, research/graphics.md
  // 4, SPEC 5:
  //   Renderer: engine.graphics_quality ("Mocktail default (level 3)",
  //     "Roblox in-game slider", "Custom level…" -> spin 1..21; only applies
  //     while the performance preset is active, performance_policy.cc).
  //   Frame rate: graphics.frame_rate_limit ("display" writes the monitor's
  //     integer refresh rate), graphics.vsync (direct Vulkan only).
  AddGroup(page, _("Coming soon"),
           _("These settings arrive in a later step. Until then they can be "
             "changed in config.yaml."));
  return page;
}

}  // namespace mocktail::launcher_ui
