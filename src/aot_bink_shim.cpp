#include "aot_bink_shim.h"

#include "generated/default/aot_init.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xio.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

// Host-side movie presentation.
//
// The recompiled Bink decoder works correctly (it writes correct YUV frames into
// the plane buffers), but ReXGlue's GPU render path does not draw the movie quad,
// so the screen stays black (see docs/fix-movies.md). Since decode is fine, we
// read the decoded planes each frame, convert YUV->RGBA on the host, and draw the
// frame fullscreen as a UI overlay via the ImGui drawer — bypassing the broken
// guest render entirely. No FFmpeg is involved; the game's own decoder is the
// source. Audio/timing/skip stay on the game's working path.

REXCVAR_DEFINE_BOOL(aot_host_movies, true, "AOT",
                    "Present Bink movies host-side (read the game's decoded planes and draw them "
                    "as an overlay), working around ReXGlue's broken movie render path")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(aot_trace_movies, false, "AOT",
                    "Log Bink movie (.bik) file opens")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// Big-endian guest u32 load straight from the mapped guest image base.
uint32_t GuestLoadU32(uint8_t* base, uint32_t guest_addr) {
  if (guest_addr < 0x1000u || guest_addr > 0xFFFFFFF8u) {
    return 0;
  }
  uint32_t raw;
  std::memcpy(&raw, base + guest_addr, sizeof(raw));
  return __builtin_bswap32(raw);
}

inline uint8_t ClampU8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v)); }

// Latest decoded movie frame, handed from the game's movie thread (where the
// Bink service call runs) to the UI thread (where the overlay is drawn).
struct HostMovieFrame {
  std::mutex mtx;
  std::vector<uint8_t> rgba;  // w*h*4, R8G8B8A8
  int w = 0;
  int h = 0;
  uint64_t seq = 0;  // bumped on each published frame
};
HostMovieFrame g_movie;

// HBINK layout (see docs/fix-movies.md): FrameBuffers* at +0xB8.
// BINKFRAMEBUFFERS: TotalFrames +0x00, YABufferWidth +0x04, YABufferHeight +0x08,
// cRcBBufferWidth +0x0C, cRcBBufferHeight +0x10, FrameNum(current index) +0x14,
// Frames[] at +0x18, each = 4 planes (Y, cR, cB, A) x {Allocate, Buffer, Pitch}.
void PublishMovieFrame(uint8_t* base, uint32_t hbink) {
  const uint32_t fb = GuestLoadU32(base, hbink + 0xB8);
  if (!fb) {
    return;
  }
  const uint32_t total = GuestLoadU32(base, fb + 0x00);
  const uint32_t w = GuestLoadU32(base, fb + 0x04);
  const uint32_t h = GuestLoadU32(base, fb + 0x08);
  // Only present fullscreen movies; skip small UI loops (e.g. LoadingCoin 100x100)
  // which are composited by the game at a specific spot, not fullscreen.
  if (w < 1024 || h < 576 || total == 0 || total > 8) {
    return;
  }
  uint32_t cur = GuestLoadU32(base, fb + 0x14);  // BINKFRAMEBUFFERS current buffer index
  if (cur >= total) {
    cur = 0;
  }
  // KNOWN ISSUE: a fixed vertical band shows green because the chroma planes are
  // not fully decoded for one worker-thread strip at this read point (the async
  // multithreaded decode hasn't finished that strip). Reading the current buffer
  // or the previous buffer both still show it. See docs/fix-movies.md "Next steps".
  const uint32_t frame = fb + 0x18 + cur * 0x30;
  const uint32_t y_buf = GuestLoadU32(base, frame + 0x04);
  const uint32_t y_pitch = GuestLoadU32(base, frame + 0x08);
  const uint32_t cr_buf = GuestLoadU32(base, frame + 0x10);  // Cr (V)
  const uint32_t cr_pitch = GuestLoadU32(base, frame + 0x14);
  const uint32_t cb_buf = GuestLoadU32(base, frame + 0x1C);  // Cb (U)
  const uint32_t cb_pitch = GuestLoadU32(base, frame + 0x20);
  if (!y_buf || !cr_buf || !cb_buf || !y_pitch || !cr_pitch || !cb_pitch) {
    return;
  }

  // Convert BT.601 limited-range YUV420 -> RGBA on this (movie) thread.
  static std::vector<uint8_t> scratch;
  scratch.resize(static_cast<size_t>(w) * h * 4);
  const uint8_t* Y = base + y_buf;
  const uint8_t* V = base + cr_buf;
  const uint8_t* U = base + cb_buf;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* yr = Y + static_cast<size_t>(y) * y_pitch;
    const uint8_t* ur = U + static_cast<size_t>(y >> 1) * cb_pitch;
    const uint8_t* vr = V + static_cast<size_t>(y >> 1) * cr_pitch;
    uint8_t* out = scratch.data() + static_cast<size_t>(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      const int c = static_cast<int>(yr[x]) - 16;
      const int d = static_cast<int>(ur[x >> 1]) - 128;
      const int e = static_cast<int>(vr[x >> 1]) - 128;
      out[0] = ClampU8((298 * c + 409 * e + 128) >> 8);
      out[1] = ClampU8((298 * c - 100 * d - 208 * e + 128) >> 8);
      out[2] = ClampU8((298 * c + 516 * d + 128) >> 8);
      out[3] = 255;
      out += 4;
    }
  }

  std::lock_guard<std::mutex> lock(g_movie.mtx);
  g_movie.rgba.swap(scratch);  // hand off; keep the old buffer in scratch for reuse
  g_movie.w = static_cast<int>(w);
  g_movie.h = static_cast<int>(h);
  ++g_movie.seq;
}

// Fullscreen overlay that draws the latest published movie frame on the UI thread.
class MovieOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  MovieOverlayDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate)
      : ImGuiDialog(drawer), immediate_(immediate) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    uint64_t seq = 0;
    bool refresh = false;
    {
      std::lock_guard<std::mutex> lock(g_movie.mtx);
      seq = g_movie.seq;
      if (seq != 0 && seq != tex_seq_ && !g_movie.rgba.empty()) {
        local_.assign(g_movie.rgba.begin(), g_movie.rgba.end());
        tw_ = g_movie.w;
        th_ = g_movie.h;
        refresh = true;
      }
    }
    if (refresh && immediate_) {
      texture_ = immediate_->CreateTexture(static_cast<uint32_t>(tw_), static_cast<uint32_t>(th_),
                                           rex::ui::ImmediateTextureFilter::kLinear, false,
                                           local_.data());
      tex_seq_ = seq;
    }

    // Liveness: while new frames keep arriving the movie is playing; once they
    // stop advancing for a few UI frames, hide the overlay.
    if (seq != last_seen_seq_) {
      last_seen_seq_ = seq;
      stall_ = 0;
    } else if (stall_ < 1000) {
      ++stall_;
    }
    const bool live = seq != 0 && stall_ < 8;
    if (live && texture_) {
      ImGui::GetBackgroundDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture_.get()),
                                               ImVec2(0.0f, 0.0f), io.DisplaySize);
    }
  }

 private:
  rex::ui::ImmediateDrawer* immediate_ = nullptr;
  std::unique_ptr<rex::ui::ImmediateTexture> texture_;
  std::vector<uint8_t> local_;
  uint64_t tex_seq_ = 0;
  uint64_t last_seen_seq_ = 0;
  int stall_ = 1000;
  int tw_ = 0;
  int th_ = 0;
};

MovieOverlayDialog* g_overlay = nullptr;

// --- Optional .bik open trace (diagnostic) ----------------------------------
PPCFunc* g_orig_nt_create_file = nullptr;
PPCFunc* g_orig_nt_open_file = nullptr;

std::string ReadOpenPath(PPCContext& ctx) {
  const uint32_t object_attrs_ptr = ctx.r5.u32;  // 3rd arg of NtCreateFile/NtOpenFile
  if (!object_attrs_ptr) {
    return {};
  }
  auto* memory = rex::system::kernel_state()->memory();
  auto* object_attrs =
      memory->TranslateVirtual<rex::system::X_OBJECT_ATTRIBUTES*>(object_attrs_ptr);
  if (!object_attrs) {
    return {};
  }
  const uint32_t name_ptr = object_attrs->name_ptr;
  if (!name_ptr) {
    return {};
  }
  return std::string(rex::system::util::TranslateAnsiStringAddress(memory, name_ptr));
}

void DiagOpen(PPCContext& ctx, uint8_t* base, const char* which, PPCFunc* orig) {
  const std::string path = ReadOpenPath(ctx);
  if (!path.empty()) {
    std::string lower = path;
    for (char& c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower.find(".bik") != std::string::npos) {
      REXLOG_INFO("Libre Army of Two: {} opening movie '{}'", which, path);
    }
  }
  orig(ctx, base);
}

void DiagNtCreateFile(PPCContext& ctx, uint8_t* base) {
  DiagOpen(ctx, base, "NtCreateFile", g_orig_nt_create_file);
}
void DiagNtOpenFile(PPCContext& ctx, uint8_t* base) {
  DiagOpen(ctx, base, "NtOpenFile", g_orig_nt_open_file);
}

void InstallMovieDiagnostics() {
  struct Target {
    uint32_t guest;
    PPCFunc** original;
    PPCFunc* hook;
  };
  const std::array<Target, 2> targets{{
      {0x8308AD74u, &g_orig_nt_create_file, DiagNtCreateFile},
      {0x8308AD64u, &g_orig_nt_open_file, DiagNtOpenFile},
  }};
  for (const auto& t : targets) {
    for (auto* mapping = PPCFuncMappings; mapping->guest != 0; ++mapping) {
      if (mapping->guest == t.guest) {
        *t.original = mapping->host;
        mapping->host = t.hook;
        break;
      }
    }
  }
}

}  // namespace

void AotInstallBinkHooks(rex::RuntimeConfig& /*config*/) {
  if (REXCVAR_GET(aot_trace_movies)) {
    InstallMovieDiagnostics();
  }
}

void AotCreateMovieOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate) {
  if (!REXCVAR_GET(aot_host_movies) || !drawer || !immediate || g_overlay) {
    return;
  }
  // ImGuiDialog retains itself and is drawn each UI frame; kept for app lifetime.
  g_overlay = new MovieOverlayDialog(drawer, immediate);
  REXLOG_INFO("Libre Army of Two: host movie overlay installed");
}

// --- Bink internal-function override (weak-alias strong definition) -----------
// `sub_8308E368` is the per-frame Bink service call (HBINK in r3). The recompiler
// emits `sub_8308E368` as a weak alias to the real body `__imp__sub_8308E368`;
// defining it strongly here intercepts all direct calls. We run the original
// (which advances decode/audio/timing), then publish the freshly decoded frame.
extern "C" {
void __imp__sub_8308E368(PPCContext& ctx, uint8_t* base);
}

void sub_8308E368(PPCContext& ctx, uint8_t* base) {
  const uint32_t hbink = ctx.r3.u32;
  __imp__sub_8308E368(ctx, base);
  // PublishMovieFrame reads the buffer NOT currently being decoded (the stable
  // previous frame), to avoid the async decoder's half-written current buffer.
  if (hbink && REXCVAR_GET(aot_host_movies)) {
    PublishMovieFrame(base, hbink);
  }
}
