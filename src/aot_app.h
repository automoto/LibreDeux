// Libre Army of Two - ReXGlue Recompiled Project
//
// Keep this intentionally small until Army of Two has a measured compatibility
// need. Prefer official ReXGlue behavior over project-specific hooks.

#pragma once

#include "generated/default/aot_init.h"

#include <memory>

#include <rex/audio/nop/nop_audio_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

REXCVAR_DECLARE(bool, aot_nop_audio);

class AotApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AotApp>(new AotApp(ctx, "aot", PPCImageConfig));
  }

  void OnPostInitLogging() override {
    REXLOG_INFO("Libre Army of Two: official ReXGlue app initialized");
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!REXCVAR_GET(aot_nop_audio)) {
      return;
    }

    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);
    REXLOG_INFO("Libre Army of Two: using NOP audio backend for triage");
  }
};
