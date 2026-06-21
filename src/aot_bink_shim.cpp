#include "aot_bink_shim.h"

#include "generated/default/aot_init.h"

#include <algorithm>
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

// HBINK of the active movie, captured on the movie thread in the sub_8308E368
// override and consumed by the sub_824B5EA8 override (same thread).
uint32_t g_hbink = 0;

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
  const uint32_t cw = GuestLoadU32(base, fb + 0x0C);  // cRcBBufferWidth
  const uint32_t chh = GuestLoadU32(base, fb + 0x10);  // cRcBBufferHeight
  uint32_t cur = GuestLoadU32(base, fb + 0x14);        // index Bink is decoding into
  if (cur >= total) {
    cur = 0;
  }

  const uint32_t frame = fb + 0x18 + cur * 0x30;
  const uint32_t y_buf = GuestLoadU32(base, frame + 0x04);
  const uint32_t y_pitch = GuestLoadU32(base, frame + 0x08);
  const uint32_t cr_buf = GuestLoadU32(base, frame + 0x10);  // Cr (V)
  const uint32_t cr_pitch = GuestLoadU32(base, frame + 0x14);
  const uint32_t cb_buf = GuestLoadU32(base, frame + 0x1C);  // Cb (U)
  const uint32_t cb_pitch = GuestLoadU32(base, frame + 0x20);
  if (!y_buf || !cr_buf || !cb_buf || !y_pitch || !cr_pitch || !cb_pitch || !cw || !chh) {
    return;
  }

  const uint8_t* Y = base + y_buf;
  const uint8_t* U = base + cb_buf;  // Cb
  const uint8_t* V = base + cr_buf;  // Cr

  // One Bink decode worker's chroma comes out near-zero under ReXGlue over a
  // fixed region (a green band/strip; luma is fine). A chroma pixel is "dead" when
  // U and V are both well below neutral (which renders green). Repair into fixed
  // half-res chroma planes: first fill dead pixels horizontally from the nearest
  // good pixel in the row; then fill mostly-dead rows vertically from the nearest
  // good row (so a fully-dead top/bottom strip gets chroma from a good row). If the
  // whole frame is dead (a torn/transition frame), skip and keep the previous.
  static std::vector<uint8_t> uf, vf;  // fixed chroma planes, cw*chh
  static std::vector<uint32_t> row_dead;
  uf.resize(static_cast<size_t>(cw) * chh);
  vf.resize(static_cast<size_t>(cw) * chh);
  row_dead.resize(chh);
  auto dead_at = [&](const uint8_t* us, const uint8_t* vs, uint32_t x) {
    return us[x] < 64 && vs[x] < 64;  // both far below neutral(128) => artifact green
  };
  uint64_t dead_px = 0;
  for (uint32_t r = 0; r < chh; ++r) {
    const uint8_t* us = U + static_cast<size_t>(r) * cb_pitch;
    const uint8_t* vs = V + static_cast<size_t>(r) * cr_pitch;
    uint8_t* ud = uf.data() + static_cast<size_t>(r) * cw;
    uint8_t* vd = vf.data() + static_cast<size_t>(r) * cw;
    uint32_t rdead = 0;
    int first_good = -1;
    for (uint32_t x = 0; x < cw; ++x) {
      if (!dead_at(us, vs, x)) {
        first_good = static_cast<int>(x);
        break;
      }
    }
    if (first_good < 0) {  // whole row dead -> mark; fixed by the vertical pass
      std::memset(ud, 128, cw);
      std::memset(vd, 128, cw);
      row_dead[r] = cw;
      dead_px += cw;
      continue;
    }
    for (int x = 0; x < first_good; ++x) {  // leading dead run
      ud[x] = us[first_good];
      vd[x] = vs[first_good];
      ++rdead;
    }
    int last = first_good;
    for (uint32_t x = static_cast<uint32_t>(first_good); x < cw; ++x) {
      if (!dead_at(us, vs, x)) {
        ud[x] = us[x];
        vd[x] = vs[x];
        last = static_cast<int>(x);
      } else {
        ud[x] = ud[last];
        vd[x] = vd[last];
        ++rdead;
      }
    }
    row_dead[r] = rdead;
    dead_px += rdead;
  }
  // Vertical fill: replace mostly-dead rows with the nearest good row's chroma.
  {
    const uint32_t good_thresh = cw / 4;
    static std::vector<int> above, below;
    above.resize(chh);
    below.resize(chh);
    int g = -1;
    for (uint32_t r = 0; r < chh; ++r) {
      if (row_dead[r] <= good_thresh) g = static_cast<int>(r);
      above[r] = g;
    }
    g = -1;
    for (uint32_t r = chh; r-- > 0;) {
      if (row_dead[r] <= good_thresh) g = static_cast<int>(r);
      below[r] = g;
    }
    for (uint32_t r = 0; r < chh; ++r) {
      if (row_dead[r] <= cw / 2) {
        continue;  // row is mostly good (horizontal fill already handled it)
      }
      int src = -1;
      if (above[r] < 0) {
        src = below[r];
      } else if (below[r] < 0) {
        src = above[r];
      } else {
        src = (r - static_cast<uint32_t>(above[r]) <= static_cast<uint32_t>(below[r]) - r)
                  ? above[r]
                  : below[r];
      }
      if (src >= 0) {
        std::memcpy(uf.data() + static_cast<size_t>(r) * cw,
                    uf.data() + static_cast<size_t>(src) * cw, cw);
        std::memcpy(vf.data() + static_cast<size_t>(r) * cw,
                    vf.data() + static_cast<size_t>(src) * cw, cw);
      }
    }
  }
  if (dead_px > (static_cast<uint64_t>(cw) * chh) / 2) {
    return;  // mostly-dead frame => keep the previous good frame
  }

  // Under ReXGlue the decoded picture comes out wrapped horizontally: each plane
  // row is cyclically rotated to the right, so the right edge appears at the left
  // (verified on the EA / Army-of-Two logos against an ffmpeg reference decode of
  // the same .bik). The luma rotation equals twice the chroma-plane pitch padding
  // (cr_pitch - cw); chroma is rotated by that padding directly. Undo the wrap by
  // sampling each source column at (x + x_wrap) mod w. (cr_pitch - cw == 128 here,
  // so the luma picture is shifted right by 256 px in a 1280-wide frame.)
  const uint32_t x_wrap = ((cr_pitch > cw ? (cr_pitch - cw) : 0u) * 2u) % (w ? w : 1u);

  // Convert BT.601 limited-range YUV420 -> RGBA using the repaired chroma planes,
  // un-wrapping the horizontal shift as we sample each source column.
  static std::vector<uint8_t> scratch;
  scratch.resize(static_cast<size_t>(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* yr = Y + static_cast<size_t>(y) * y_pitch;
    const uint8_t* ur = uf.data() + static_cast<size_t>(y >> 1) * cw;
    const uint8_t* vr = vf.data() + static_cast<size_t>(y >> 1) * cw;
    uint8_t* out = scratch.data() + static_cast<size_t>(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t sx = (x + x_wrap) % w;  // un-wrap the horizontal rotation
      const uint32_t cc = sx >> 1;
      const int c = static_cast<int>(yr[sx]) - 16;
      const int d = static_cast<int>(ur[cc]) - 128;
      const int e = static_cast<int>(vr[cc]) - 128;
      int R = ClampU8((298 * c + 409 * e + 128) >> 8);
      int G = ClampU8((298 * c - 100 * d - 208 * e + 128) >> 8);
      int B = ClampU8((298 * c + 516 * d + 128) >> 8);
      // Final guard: the decode-artifact colour is a strong green (chroma ~0 ->
      // G dominates R and B). Detect green dominance in the output and render
      // neutral grey (luma) instead, so no green survives at any brightness.
      if (G > R + 40 && G > B + 40 && R < 110 && B < 110) {
        R = G = B = ClampU8((298 * c + 128) >> 8);
      }
      out[0] = static_cast<uint8_t>(R);
      out[1] = static_cast<uint8_t>(G);
      out[2] = static_cast<uint8_t>(B);
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
    if (live && texture_ && tw_ > 0 && th_ > 0) {
      // Draw aspect-correct (letterboxed), centered in the window, instead of
      // stretching to the full surface (which distorts non-16:9 windows).
      const float dw = io.DisplaySize.x;
      const float dh = io.DisplaySize.y;
      const float scale =
          std::min(dw / static_cast<float>(tw_), dh / static_cast<float>(th_));
      const float fw = static_cast<float>(tw_) * scale;
      const float fh = static_cast<float>(th_) * scale;
      const float x0 = (dw - fw) * 0.5f;
      const float y0 = (dh - fh) * 0.5f;
      ImGui::GetBackgroundDrawList()->AddImage(reinterpret_cast<ImTextureID>(texture_.get()),
                                               ImVec2(x0, y0), ImVec2(x0 + fw, y0 + fh));
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

// --- Bink internal-function overrides (weak-alias strong definitions) ---------
// The recompiler emits each internal function `sub_XXXX` as a weak alias to its
// real body `__imp__sub_XXXX`; defining `sub_XXXX` strongly here intercepts all
// direct calls. We call the original, then do our work.
//
// `sub_8308E368` is Bink's per-frame service call (HBINK in r3) but it runs
// *inside* the multithreaded decode/wait loop, so the planes are not yet complete
// there (reading them caused the green chroma band). We use it only to capture the
// HBINK pointer. `sub_824B5EA8` is the outer UCodecMovieBink::service whose loop
// returns only once decode is complete — we publish the finished frame there.
extern "C" {
void __imp__sub_8308E368(PPCContext& ctx, uint8_t* base);
void __imp__sub_824B5EA8(PPCContext& ctx, uint8_t* base);
}

void sub_8308E368(PPCContext& ctx, uint8_t* base) {
  g_hbink = ctx.r3.u32;  // HBINK; captured for the post-loop publish below
  __imp__sub_8308E368(ctx, base);
}

void sub_824B5EA8(PPCContext& ctx, uint8_t* base) {
  __imp__sub_824B5EA8(ctx, base);  // runs the full per-frame decode + wait loop
  // The decode loop has completed, so the frame buffers are now stable.
  if (g_hbink && REXCVAR_GET(aot_host_movies)) {
    PublishMovieFrame(base, g_hbink);
  }
}
