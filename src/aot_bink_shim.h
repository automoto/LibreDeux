// Libre Army of Two - host-side Bink movie presentation.
//
// The recompiled Bink decoder works (it writes correct YUV frames into the plane
// buffers), but ReXGlue's GPU render path does not draw the movie quad, leaving a
// black screen. Since decode is fine, we read the decoded planes each frame,
// convert YUV->RGBA on the host, and draw them fullscreen as an ImGui overlay,
// bypassing the broken guest render. See docs/fix-movies.md.

#pragma once

#include <rex/cvar.h>
#include <rex/runtime.h>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

REXCVAR_DECLARE(bool, aot_host_movies);
REXCVAR_DECLARE(bool, aot_trace_movies);

// Installs movie-related hooks (currently the optional .bik open trace). Call
// from AotApp::OnPreSetup.
void AotInstallBinkHooks(rex::RuntimeConfig& config);

// Creates the fullscreen host movie overlay. Call from AotApp::OnCreateDialogs.
void AotCreateMovieOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate);
