// Libre Army of Two - ReXGlue Recompiled Project
//
// Keep this intentionally small until Army of Two has a measured compatibility
// need. Prefer official ReXGlue behavior over project-specific hooks.

#pragma once

#include "crash_dump.h"
#include "aot_bink_shim.h"
#include "aot_xam_coop.h"
#include "generated/default/aot_init.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <rex/audio/nop/nop_audio_system.h>
#include <rex/cvar.h>
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

REXCVAR_DECLARE(bool, aot_nop_audio);
REXCVAR_DECLARE(bool, aot_mount_shaderdumpxe);
REXCVAR_DECLARE(bool, aot_seed_shader_compare_backend);
REXCVAR_DECLARE(std::string, aot_graphics_backend);
REXCVAR_DECLARE(std::string, aot_hdr_readback);

class AotApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AotApp>(new AotApp(ctx, "aot", PPCImageConfig));
  }

  void OnPostInitLogging() override {
    AotInstallCrashDumpHandler();
    REXLOG_INFO("Libre Army of Two: official ReXGlue app initialized");
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    const auto& graphics_backend = REXCVAR_GET(aot_graphics_backend);
    if (graphics_backend == "d3d12") {
#if REX_HAS_D3D12
      config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
      REXLOG_INFO("Libre Army of Two: forcing Direct3D 12 graphics backend");
#else
      REXLOG_WARN("Libre Army of Two: Direct3D 12 backend was requested but is unavailable");
#endif
    } else if (graphics_backend == "vulkan") {
#if REX_HAS_VULKAN
      config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
      REXLOG_INFO("Libre Army of Two: forcing Vulkan graphics backend");
#else
      REXLOG_WARN("Libre Army of Two: Vulkan backend was requested but is unavailable");
#endif
    }

    if (REXCVAR_GET(aot_nop_audio)) {
      config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);
      REXLOG_INFO("Libre Army of Two: using NOP audio backend for triage");
    }

    ApplyHdrReadbackMode();

    AotInstallLocalCoopHooks(config);
    AotInstallBinkHooks(config);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    AotCreateMovieOverlay(drawer, immediate_drawer());
  }

  void OnPostSetup() override {
    if (!REXCVAR_GET(aot_mount_shaderdumpxe) || !runtime() || !runtime()->file_system()) {
      return;
    }

    auto shader_dump_root = cache_root() / "shaderdumpxe";
    if (REXCVAR_GET(aot_seed_shader_compare_backend)) {
      std::error_code ec;
      std::filesystem::create_directories(shader_dump_root, ec);
      if (ec) {
        REXLOG_WARN("Libre Army of Two: failed to create ShaderDumpxe cache {}: {}",
                    shader_dump_root.string(), ec.message());
      } else {
        auto compare_backends = shader_dump_root / "CompareBackEnds";
        if (!std::filesystem::exists(compare_backends, ec)) {
          std::ofstream(compare_backends, std::ios::binary).close();
        }
        if (ec) {
          REXLOG_WARN("Libre Army of Two: failed to probe ShaderDumpxe stub {}: {}",
                      compare_backends.string(), ec.message());
        }
      }
    }

    auto device = std::make_unique<rex::filesystem::HostPathDevice>(
        "\\Device\\ShaderDumpxe", shader_dump_root, false);
    if (!device->Initialize()) {
      REXLOG_WARN("Libre Army of Two: failed to initialize ShaderDumpxe device at {}",
                  shader_dump_root.string());
      return;
    }

    auto* file_system = runtime()->file_system();
    file_system->RegisterDevice(std::move(device));
    file_system->RegisterSymbolicLink("ShaderDumpxe:", "\\Device\\ShaderDumpxe");
    REXLOG_INFO("Libre Army of Two: mounted {} as ShaderDumpxe:", shader_dump_root.string());
  }

 private:
  // Army of Two (UE3) drives auto-exposure/bloom from HDR eye-adaptation, which needs GPU->CPU
  // resolve readback. ReXGlue (like stock Xenia) ships readback-resolve disabled by default, so
  // exposure blows out (over-bright/washed-out). Mirror Xenia's known-good fix by enabling the
  // kFast readback path. Driven by the aot_hdr_readback enum cvar (instead of a bare
  // HasNonDefaultValue guard) so the "off" case is detectable despite the cvar system having no
  // source tracking - see docs/cli-limitation.md. Uses the shared readback_resolve string so it
  // applies to both D3D12 and Vulkan. Runs in OnPreSetup, before the runtime/GPU consume it.
  void ApplyHdrReadbackMode() {
    const std::string& mode = REXCVAR_GET(aot_hdr_readback);
    if (mode == "fast" || mode == "full" || mode == "off") {
      rex::cvar::SetFlagByName("readback_resolve", mode == "off" ? "none" : mode);
      REXLOG_INFO("Libre Army of Two: HDR readback-resolve = {} (aot_hdr_readback)", mode);
      return;
    }
    // auto: enable kFast unless the user already chose a readback setting via any source.
    if (rex::cvar::HasNonDefaultValue("readback_resolve") ||
        rex::cvar::HasNonDefaultValue("d3d12_readback_resolve") ||
        rex::cvar::HasNonDefaultValue("vulkan_readback_resolve")) {
      REXLOG_INFO("Libre Army of Two: HDR readback-resolve left at user/config setting");
      return;
    }
    rex::cvar::SetFlagByName("readback_resolve", "fast");
    REXLOG_INFO("Libre Army of Two: HDR readback-resolve enabled (kFast, auto default); "
                "override with --aot_hdr_readback=off|full");
  }
};
