#include "aot_app.h"

REXCVAR_DEFINE_BOOL(aot_nop_audio, false, "AOT",
                    "Use ReXGlue's NOP audio backend for first-boot triage");
REXCVAR_DEFINE_BOOL(aot_mount_shaderdumpxe, true, "AOT",
                    "Mount a writable ShaderDumpxe: VFS device for startup shader debug probes");
REXCVAR_DEFINE_BOOL(aot_seed_shader_compare_backend, true, "AOT",
                    "Seed a zero-byte ShaderDumpxe:\\CompareBackEnds file for startup probes");
REXCVAR_DEFINE_STRING(aot_graphics_backend, "auto", "AOT",
                      "Graphics backend override: auto, d3d12, vulkan")
    .allowed({"auto", "d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
