param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot ".."))
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path $Root).Path
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Replace-OnceOrThrow([string]$InputText, [string]$Needle, [string]$Replacement, [string]$Description) {
    $index = $InputText.IndexOf($Needle, [System.StringComparison]::Ordinal)
    if ($index -lt 0) {
        throw "Unable to apply generated workaround segment: $Description"
    }

    return $InputText.Substring(0, $index) + $Replacement + $InputText.Substring($index + $Needle.Length)
}

function Apply-GeneratedWorkaround([string]$Target, [string[]]$Labels, [object[]]$Segments) {
    if (-not (Test-Path -LiteralPath $Target)) {
        throw "Generated file not found: $Target"
    }

    $text = [System.IO.File]::ReadAllText($Target).Replace("`r`n", "`n")
    $presentLabels = @($Labels | Where-Object { $text.Contains($_) })
    $missingLabels = @($Labels | Where-Object { -not $text.Contains($_) })

    if ($presentLabels.Count -gt 0) {
        if ($missingLabels.Count -eq 0) {
            Write-Output "generated_workaround=already_applied file=$Target"
            return
        }

        throw "Generated file has a partial workaround: $Target missing=$($missingLabels -join ',')"
    }

    $patchedText = $text
    foreach ($segment in $Segments) {
        $patchedText = Replace-OnceOrThrow $patchedText $segment["Needle"] $segment["Replacement"] $segment["Description"]
    }

    [System.IO.File]::WriteAllText($Target, $patchedText, $utf8NoBom)
    Write-Output "generated_workaround=applied file=$Target"
}

$shaderBackendSegments = @(
    @{
        Description = "first shader/backend factory guard"
        Needle = @'
	// bl 0x82a780a0
	ctx.lr = 0x829CA1C4;
	sub_82A780A0(ctx, base);
	// lwz r3,96(r1)
'@
        Replacement = @'
	// bl 0x82a780a0
	ctx.lr = 0x829CA1C4;
	sub_82A780A0(ctx, base);
	if (ctx.r3.s32 < 0 || REX_LOAD_U32(ctx.r1.u32 + 96) < 0x1000) {
		REXLOG_WARN("AOT workaround: first shader backend factory failed rc=0x{:08X} out=0x{:08X}; skipping dereference",
		            ctx.r3.u32, REX_LOAD_U32(ctx.r1.u32 + 96));
		goto loc_aot_skip_first_shader_backend;
	}
	// lwz r3,96(r1)
'@
    }
    @{
        Description = "first shader/backend skip label"
        Needle = @'
	// lwz r3,27520(r28)
	ctx.r3.u64 = REX_LOAD_U32(ctx.r28.u32 + 27520);
'@
        Replacement = @'
	// lwz r3,27520(r28)
loc_aot_skip_first_shader_backend:
	ctx.r3.u64 = REX_LOAD_U32(ctx.r28.u32 + 27520);
'@
    }
    @{
        Description = "second shader/backend factory guard"
        Needle = @'
	// bl 0x82a780a0
	ctx.lr = 0x829CA244;
	sub_82A780A0(ctx, base);
	// lwz r3,96(r1)
'@
        Replacement = @'
	// bl 0x82a780a0
	ctx.lr = 0x829CA244;
	sub_82A780A0(ctx, base);
	if (ctx.r3.s32 < 0 || REX_LOAD_U32(ctx.r1.u32 + 96) < 0x1000) {
		REXLOG_WARN("AOT workaround: second shader backend factory failed rc=0x{:08X} out=0x{:08X}; skipping dereference",
		            ctx.r3.u32, REX_LOAD_U32(ctx.r1.u32 + 96));
		goto loc_aot_skip_second_shader_backend;
	}
	// lwz r3,96(r1)
'@
    }
    @{
        Description = "second shader/backend skip label"
        Needle = @'
	// bctrl 
	ctx.lr = 0x829CA274;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// addi r1,r1,192
	ctx.r1.s64 = ctx.r1.s64 + 192;
'@
        Replacement = @'
	// bctrl 
	ctx.lr = 0x829CA274;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// addi r1,r1,192
loc_aot_skip_second_shader_backend:
	ctx.r1.s64 = ctx.r1.s64 + 192;
'@
    }
)

$shellShaderBackendSegments = @(
    @{
        Description = "first shell shader/backend factory guard"
        Needle = @'
	// bl 0x82a780a0
	ctx.lr = 0x829D6254;
	sub_82A780A0(ctx, base);
	// lwz r3,96(r1)
'@
        Replacement = @'
	// bl 0x82a780a0
	ctx.lr = 0x829D6254;
	sub_82A780A0(ctx, base);
	if (ctx.r3.s32 < 0 || REX_LOAD_U32(ctx.r1.u32 + 96) < 0x1000) {
		REXLOG_WARN("AOT workaround: first shell shader backend factory failed rc=0x{:08X} out=0x{:08X}; skipping dereference",
		            ctx.r3.u32, REX_LOAD_U32(ctx.r1.u32 + 96));
		goto loc_aot_skip_first_shell_shader_backend;
	}
	// lwz r3,96(r1)
'@
    }
    @{
        Description = "first shell shader/backend skip label"
        Needle = @'
	// bctrl 
	ctx.lr = 0x829D6284;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// addi r11,r1,384
	ctx.r11.s64 = ctx.r1.s64 + 384;
'@
        Replacement = @'
	// bctrl 
	ctx.lr = 0x829D6284;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// addi r11,r1,384
loc_aot_skip_first_shell_shader_backend:
	ctx.r11.s64 = ctx.r1.s64 + 384;
'@
    }
    @{
        Description = "second shell shader/backend factory guard"
        Needle = @'
	// bl 0x82a780a0
	ctx.lr = 0x829D62D0;
	sub_82A780A0(ctx, base);
	// lwz r3,96(r1)
'@
        Replacement = @'
	// bl 0x82a780a0
	ctx.lr = 0x829D62D0;
	sub_82A780A0(ctx, base);
	if (ctx.r3.s32 < 0 || REX_LOAD_U32(ctx.r1.u32 + 96) < 0x1000) {
		REXLOG_WARN("AOT workaround: second shell shader backend factory failed rc=0x{:08X} out=0x{:08X}; skipping dereference",
		            ctx.r3.u32, REX_LOAD_U32(ctx.r1.u32 + 96));
		goto loc_aot_skip_second_shell_shader_backend;
	}
	// lwz r3,96(r1)
'@
    }
    @{
        Description = "second shell shader/backend skip label"
        Needle = @'
	// bctrl 
	ctx.lr = 0x829D6300;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// li r11,1
	ctx.r11.s64 = 1;
	// stw r11,-29096(r29)
	REX_STORE_U32(ctx.r29.u32 + -29096, ctx.r11.u32);
loc_829D6308:
'@
        Replacement = @'
	// bctrl 
	ctx.lr = 0x829D6300;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// li r11,1
	ctx.r11.s64 = 1;
	// stw r11,-29096(r29)
	REX_STORE_U32(ctx.r29.u32 + -29096, ctx.r11.u32);
loc_aot_skip_second_shell_shader_backend:
loc_829D6308:
'@
    }
)

$localCoopImportRerouteSegments = @(
    @{
        Description = "local co-op direct import declarations and reroutes"
        Needle = @'
DECLARE_REX_FUNC(__imp__XamUserReadProfileSettings);
DECLARE_REX_FUNC(__imp__XamUserWriteProfileSettings);
'@
        Replacement = @'
DECLARE_REX_FUNC(__imp__XamUserReadProfileSettings);
DECLARE_REX_FUNC(__imp__XamUserWriteProfileSettings);

// AOT local co-op direct import reroute. Generated C++ calls imported XAM
// thunks directly, so PPCFuncMappings replacement alone does not intercept
// these call sites.
REX_EXTERN(__imp__AotXamUserGetName);
REX_EXTERN(__imp__AotXamUserGetSigninState);
REX_EXTERN(__imp__AotXamUserAreUsersFriends);
REX_EXTERN(__imp__AotXamUserCheckPrivilege);
REX_EXTERN(__imp__AotXamUserGetXUID);
REX_EXTERN(__imp__AotXamInputGetCapabilities);
REX_EXTERN(__imp__AotXamInputGetState);
REX_EXTERN(__imp__AotXamInputSetState);
REX_EXTERN(__imp__AotXamUserGetSigninInfo);
REX_EXTERN(__imp__AotXamShowSigninUI);
REX_EXTERN(__imp__AotXamUserReadProfileSettings);
REX_EXTERN(__imp__AotXamUserWriteProfileSettings);

#define __imp__XamUserGetName __imp__AotXamUserGetName
#define __imp__XamUserGetSigninState __imp__AotXamUserGetSigninState
#define __imp__XamUserAreUsersFriends __imp__AotXamUserAreUsersFriends
#define __imp__XamUserCheckPrivilege __imp__AotXamUserCheckPrivilege
#define __imp__XamUserGetXUID __imp__AotXamUserGetXUID
#define __imp__XamInputGetCapabilities __imp__AotXamInputGetCapabilities
#define __imp__XamInputGetState __imp__AotXamInputGetState
#define __imp__XamInputSetState __imp__AotXamInputSetState
#define __imp__XamUserGetSigninInfo __imp__AotXamUserGetSigninInfo
#define __imp__XamShowSigninUI __imp__AotXamShowSigninUI
#define __imp__XamUserReadProfileSettings __imp__AotXamUserReadProfileSettings
#define __imp__XamUserWriteProfileSettings __imp__AotXamUserWriteProfileSettings
'@
    }
)

Apply-GeneratedWorkaround `
    (Join-Path $repoRoot "generated\default\aot_recomp.62.cpp") `
    @("loc_aot_skip_first_shader_backend", "loc_aot_skip_second_shader_backend") `
    $shaderBackendSegments

Apply-GeneratedWorkaround `
    (Join-Path $repoRoot "generated\default\aot_recomp.63.cpp") `
    @("loc_aot_skip_first_shell_shader_backend", "loc_aot_skip_second_shell_shader_backend") `
    $shellShaderBackendSegments

Apply-GeneratedWorkaround `
    (Join-Path $repoRoot "generated\default\aot_init.h") `
    @("AOT local co-op direct import reroute", "__imp__AotXamUserGetSigninState") `
    $localCoopImportRerouteSegments
