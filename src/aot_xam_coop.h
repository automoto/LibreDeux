#pragma once

#include <rex/cvar.h>
#include <rex/runtime.h>

REXCVAR_DECLARE(bool, aot_coop_local);
REXCVAR_DECLARE(bool, aot_trace_xam);

void AotInstallLocalCoopHooks(rex::RuntimeConfig& config);
