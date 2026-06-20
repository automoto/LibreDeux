#include "aot_xam_coop.h"

#include "generated/default/aot_init.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/string/util.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xio.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(aot_coop_local, false, "AOT",
                    "Expose a second offline local profile for split-screen co-op")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(aot_trace_xam, false, "AOT",
                    "Trace AoT local co-op XAM user/input/sign-in calls")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::kernel::xam {

// SDK entry points. These are implemented by rexruntime; forward declarations
// let the project wrappers delegate exact stock behavior when coop is disabled.
i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr);
u32 XamUserGetSigninState_entry(u32 user_index);
u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len);
u32 XamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len);
u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                     mapped_u64 xuids, u32 setting_count,
                                     mapped_u32 setting_ids, mapped_u32 buffer_size_ptr,
                                     mapped_void buffer_ptr,
                                     ppc_ptr_t<system::XAM_OVERLAPPED> overlapped);
u32 XamUserWriteProfileSettings_entry(u32 title_id, u32 user_index, u32 setting_count,
                                      ppc_ptr_t<system::xam::X_USER_PROFILE_SETTING> settings,
                                      ppc_ptr_t<system::XAM_OVERLAPPED> overlapped);
u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value);
u32 XamUserGetMembershipTier_entry(u32 user_index);
u32 XamUserAreUsersFriends_entry(u32 user_index, u32 unk1, u32 unk2, mapped_u32 out_value,
                                 u32 overlapped_ptr);
u32 XamShowSigninUI_entry(u32 unk, u32 unk_mask);

u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags,
                                  ppc_ptr_t<input::X_INPUT_CAPABILITIES> caps);
u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    ppc_ptr_t<input::X_INPUT_CAPABILITIES> caps);
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<input::X_INPUT_STATE> input_state);
u32 XamInputSetState_entry(u32 user_index, u32 unk,
                           ppc_ptr_t<input::X_INPUT_VIBRATION> vibration);
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags,
                               ppc_ptr_t<input::X_INPUT_KEYSTROKE> keystroke);
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<input::X_INPUT_KEYSTROKE> keystroke);
i32 XamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr);

}  // namespace rex::kernel::xam

namespace {

using rex::i32;
using rex::u32;
using rex::u64;
using rex::X_HRESULT;
using rex::X_RESULT;
using ::mapped_string;
using ::mapped_u32;
using ::mapped_u64;
using ::mapped_void;
using ::mapped_wstring;
using ::ppc_ptr_t;

constexpr u32 kSigninStateNotSignedIn = 0;
constexpr u32 kSigninStateSignedInLocally = 1;
constexpr u32 kFakeProfileTypeMask = 1 | 2;
constexpr u32 kXinputFlagAnyUser = 1u << 30;
constexpr u32 kXinputFlagGamepad = 0x01;
constexpr u64 kPlayer1Xuid = 0xB13EBABEBABEBABEull;
constexpr u64 kPlayer2Xuid = 0xB13EBABEBABEBABFull;
constexpr const char* kPlayer1Name = "User";
constexpr const char* kPlayer2Name = "Player2";

struct X_USER_SIGNIN_INFO {
  rex::be<u64> xuid;
  rex::be<u32> unk08;
  rex::be<u32> signin_state;
  rex::be<u32> unk10;
  rex::be<u32> unk14;
  char name[16];
};
static_assert_size(X_USER_SIGNIN_INFO, 40);

struct X_USER_READ_PROFILE_SETTINGS {
  rex::be<u32> setting_count;
  rex::be<u32> settings_ptr;
};
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

#define AOT_XAM_TRACE(...)                 \
  do {                                     \
    if (REXCVAR_GET(aot_trace_xam)) {      \
      REXKRNL_INFO("[AOT_COOP] " __VA_ARGS__); \
    }                                      \
  } while (false)

bool HooksActive() {
  return REXCVAR_GET(aot_coop_local) || REXCVAR_GET(aot_trace_xam);
}

rex::input::InputSystem* InputSystem() {
  auto* kernel_state = rex::system::kernel_state();
  if (!kernel_state || !kernel_state->emulator() || !kernel_state->emulator()->input_system()) {
    return nullptr;
  }
  return static_cast<rex::input::InputSystem*>(kernel_state->emulator()->input_system());
}

bool ControllerConnected(u32 user_index) {
  if (user_index >= 4) {
    return false;
  }
  auto* input = InputSystem();
  if (!input) {
    return false;
  }
  rex::input::X_INPUT_CAPABILITIES caps = {};
  return input->GetCapabilities(user_index, kXinputFlagGamepad, &caps) == X_ERROR_SUCCESS;
}

bool CoopUserSignedIn(u32 user_index) {
  if (user_index == 0) {
    return true;
  }
  if (user_index == 1) {
    // AoT checks signed-in profiles before it has a useful chance to bind P2
    // input. Keep profile presence separate from physical controller routing.
    return true;
  }
  return false;
}

u32 CoopSigninState(u32 user_index) {
  return CoopUserSignedIn(user_index) ? kSigninStateSignedInLocally : kSigninStateNotSignedIn;
}

u64 CoopXuid(u32 user_index) {
  return user_index == 1 ? kPlayer2Xuid : kPlayer1Xuid;
}

const char* CoopName(u32 user_index) {
  return user_index == 1 ? kPlayer2Name : kPlayer1Name;
}

u32 CoopSignedInUserMask() {
  u32 mask = 0;
  for (u32 i = 0; i < 4; ++i) {
    if (CoopUserSignedIn(i)) {
      mask |= 1u << i;
    }
  }
  return mask;
}

u32 NormalizeInputUser(u32 user_index, u32 flags) {
  if ((user_index & 0xFF) == 0xFF || (flags & kXinputFlagAnyUser)) {
    return 0;
  }
  return user_index;
}

void BroadcastSigninChanged() {
  if (auto* kernel_state = rex::system::kernel_state()) {
    kernel_state->BroadcastNotification(0x0000000A, 1);
  }
}

void BroadcastUiClosed() {
  if (auto* kernel_state = rex::system::kernel_state()) {
    kernel_state->BroadcastNotification(0x00000009, 0);
  }
}

void CompleteOverlappedNow(u32 overlapped_ptr, X_RESULT result) {
  if (overlapped_ptr) {
    if (auto* kernel_state = rex::system::kernel_state()) {
      kernel_state->CompleteOverlappedImmediate(overlapped_ptr, result);
    }
  }
}

u32 RemapP2ToP1(u32 user_index) {
  return user_index == 1 ? 0 : user_index;
}

void FixProfileSettingsUserIndex(mapped_void buffer_ptr, u32 fallback_setting_count) {
  if (!buffer_ptr) {
    return;
  }

  auto* header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer_ptr.host_address());
  auto setting_count = static_cast<u32>(header->setting_count);
  if (!setting_count) {
    setting_count = fallback_setting_count;
  }

  auto settings_ptr = static_cast<u32>(header->settings_ptr);
  if (!settings_ptr) {
    return;
  }

  auto* memory = rex::system::kernel_state()->memory();
  auto* setting = memory->TranslateVirtual<rex::system::xam::X_USER_PROFILE_SETTING*>(settings_ptr);
  for (u32 i = 0; i < setting_count; ++i) {
    setting[i].user_index = 1;
  }
}

i32 AotXamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  if (!REXCVAR_GET(aot_coop_local)) {
    auto result = rex::kernel::xam::XamUserGetXUID_entry(user_index, type_mask, xuid_ptr);
    AOT_XAM_TRACE("XamUserGetXUID(user={}, type_mask={:#x}) -> {:#x}", user_index, type_mask,
                  static_cast<u32>(result));
    return result;
  }

  i32 result = X_E_NO_SUCH_USER;
  u64 xuid = 0;
  if (!xuid_ptr) {
    result = X_E_INVALIDARG;
  } else if (user_index >= 4) {
    result = X_E_INVALIDARG;
  } else if (CoopUserSignedIn(user_index) && (type_mask & kFakeProfileTypeMask)) {
    xuid = CoopXuid(user_index);
    result = X_E_SUCCESS;
  }
  if (xuid_ptr) {
    *xuid_ptr = xuid;
  }
  AOT_XAM_TRACE("XamUserGetXUID(user={}, type_mask={:#x}) -> {:#x}, xuid={:#x}", user_index,
                type_mask, static_cast<u32>(result), xuid);
  return result;
}

u32 AotXamUserGetSigninState_entry(u32 user_index) {
  u32 result = REXCVAR_GET(aot_coop_local)
                   ? (user_index < 4 ? CoopSigninState(user_index) : kSigninStateNotSignedIn)
                   : rex::kernel::xam::XamUserGetSigninState_entry(user_index);
  AOT_XAM_TRACE("XamUserGetSigninState(user={}) -> {}", user_index, result);
  return result;
}

i32 AotXamUserGetSigninInfo_entry(u32 user_index, u32 flags,
                                  ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    AOT_XAM_TRACE("XamUserGetSigninInfo(user={}, flags={:#x}) -> invalidarg", user_index, flags);
    return X_E_INVALIDARG;
  }

  std::memset(info.host_address(), 0, sizeof(X_USER_SIGNIN_INFO));
  bool signed_in = REXCVAR_GET(aot_coop_local) ? CoopUserSignedIn(user_index) : user_index == 0;
  if (user_index >= 4 || !signed_in) {
    AOT_XAM_TRACE("XamUserGetSigninInfo(user={}, flags={:#x}) -> no user", user_index, flags);
    return X_E_NO_SUCH_USER;
  }

  info->xuid = REXCVAR_GET(aot_coop_local) ? CoopXuid(user_index) : kPlayer1Xuid;
  info->signin_state = REXCVAR_GET(aot_coop_local) ? CoopSigninState(user_index)
                                                   : kSigninStateSignedInLocally;
  const char* name = REXCVAR_GET(aot_coop_local) ? CoopName(user_index) : kPlayer1Name;
  rex::string::util_copy_truncating(info->name, name, rex::countof(info->name));
  AOT_XAM_TRACE("XamUserGetSigninInfo(user={}, flags={:#x}) -> success, name={}", user_index,
                flags, name);
  return X_E_SUCCESS;
}

u32 AotXamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (!REXCVAR_GET(aot_coop_local) || user_index == 0) {
    auto result = rex::kernel::xam::XamUserGetName_entry(user_index, buffer, buffer_len);
    AOT_XAM_TRACE("XamUserGetName(user={}, len={}) -> {:#x}", user_index, buffer_len, result);
    return result;
  }
  if (user_index >= 4) {
    AOT_XAM_TRACE("XamUserGetName(user={}, len={}) -> invalid", user_index, buffer_len);
    return X_E_INVALIDARG;
  }
  if (!CoopUserSignedIn(user_index)) {
    AOT_XAM_TRACE("XamUserGetName(user={}, len={}) -> no user", user_index, buffer_len);
    return X_E_NO_SUCH_USER;
  }
  rex::string::util_copy_truncating(buffer, CoopName(user_index), std::min(buffer_len, u32(16)));
  AOT_XAM_TRACE("XamUserGetName(user={}, len={}) -> success, name={}", user_index, buffer_len,
                CoopName(user_index));
  return X_E_SUCCESS;
}

u32 AotXamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len) {
  if (!REXCVAR_GET(aot_coop_local) || user_index == 0) {
    auto result = rex::kernel::xam::XamUserGetGamerTag_entry(user_index, buffer, buffer_len);
    AOT_XAM_TRACE("XamUserGetGamerTag(user={}, len={}) -> {:#x}", user_index, buffer_len, result);
    return result;
  }
  if (user_index >= 4 || !buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }
  if (!CoopUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }
  auto user_name = rex::string::to_utf16(CoopName(user_index));
  rex::string::util_copy_and_swap_truncating(buffer, user_name, std::min(buffer_len, u32(16)));
  AOT_XAM_TRACE("XamUserGetGamerTag(user={}, len={}) -> success, name={}", user_index, buffer_len,
                CoopName(user_index));
  return X_E_SUCCESS;
}

u32 AotXamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                        mapped_u64 xuids, u32 setting_count,
                                        mapped_u32 setting_ids, mapped_u32 buffer_size_ptr,
                                        mapped_void buffer_ptr,
                                        ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped) {
  if (!REXCVAR_GET(aot_coop_local) || user_index != 1) {
    auto result = rex::kernel::xam::XamUserReadProfileSettings_entry(
        title_id, user_index, xuid_count, xuids, setting_count, setting_ids, buffer_size_ptr,
        buffer_ptr, overlapped);
    AOT_XAM_TRACE("XamUserReadProfileSettings(title={:#x}, user={}, xuid_count={}, settings={}) "
                  "-> {:#x}",
                  title_id, user_index, xuid_count, setting_count, result);
    return result;
  }
  if (!CoopUserSignedIn(1)) {
    if (overlapped) {
      CompleteOverlappedNow(overlapped.guest_address(), X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }
  if (xuid_count != 0) {
    AOT_XAM_TRACE("XamUserReadProfileSettings P2 xuid path unsupported: xuid_count={}",
                  xuid_count);
    if (overlapped) {
      CompleteOverlappedNow(overlapped.guest_address(), X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  auto result = rex::kernel::xam::XamUserReadProfileSettings_entry(
      title_id, 0, xuid_count, xuids, setting_count, setting_ids, buffer_size_ptr, buffer_ptr,
      overlapped);
  if (result == X_ERROR_SUCCESS && buffer_ptr) {
    FixProfileSettingsUserIndex(buffer_ptr, setting_count);
  }
  AOT_XAM_TRACE("XamUserReadProfileSettings(title={:#x}, user=1->0, settings={}) -> {:#x}",
                title_id, setting_count, result);
  return result;
}

u32 AotXamUserWriteProfileSettings_entry(
    u32 title_id, u32 user_index, u32 setting_count,
    ppc_ptr_t<rex::system::xam::X_USER_PROFILE_SETTING> settings,
    ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped) {
  u32 actual_user_index = user_index;
  if (REXCVAR_GET(aot_coop_local) && user_index == 1) {
    if (!CoopUserSignedIn(1)) {
      if (overlapped) {
        CompleteOverlappedNow(overlapped.guest_address(), X_ERROR_NO_SUCH_USER);
        return X_ERROR_IO_PENDING;
      }
      return X_ERROR_NO_SUCH_USER;
    }
    actual_user_index = 0;
  }

  auto result = rex::kernel::xam::XamUserWriteProfileSettings_entry(
      title_id, actual_user_index, setting_count, settings, overlapped);
  AOT_XAM_TRACE("XamUserWriteProfileSettings(title={:#x}, user={}=>{}, settings={}) -> {:#x}",
                title_id, user_index, actual_user_index, setting_count, result);
  return result;
}

u32 AotXamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  if (!REXCVAR_GET(aot_coop_local) || (user_index != 1 && user_index != 0xFF)) {
    auto result = rex::kernel::xam::XamUserCheckPrivilege_entry(user_index, mask, out_value);
    AOT_XAM_TRACE("XamUserCheckPrivilege(user={}, mask={:#x}) -> {:#x}", user_index, mask, result);
    return result;
  }
  if (user_index >= 4 && user_index != 0xFF) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (out_value) {
    *out_value = 0;
  }
  AOT_XAM_TRACE("XamUserCheckPrivilege(user={}, mask={:#x}) -> success, allowed=0", user_index,
                mask);
  return X_ERROR_SUCCESS;
}

u32 AotXamUserGetMembershipTier_entry(u32 user_index) {
  if (!REXCVAR_GET(aot_coop_local) || user_index == 0) {
    auto result = rex::kernel::xam::XamUserGetMembershipTier_entry(user_index);
    AOT_XAM_TRACE("XamUserGetMembershipTier(user={}) -> {:#x}", user_index, result);
    return result;
  }
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!CoopUserSignedIn(user_index)) {
    return X_ERROR_NO_SUCH_USER;
  }
  AOT_XAM_TRACE("XamUserGetMembershipTier(user={}) -> 0", user_index);
  return 0;
}

u32 AotXamUserAreUsersFriends_entry(u32 user_index, u32 unk1, u32 unk2, mapped_u32 out_value,
                                    u32 overlapped_ptr) {
  if (!REXCVAR_GET(aot_coop_local) || user_index == 0) {
    auto result =
        rex::kernel::xam::XamUserAreUsersFriends_entry(user_index, unk1, unk2, out_value,
                                                       overlapped_ptr);
    AOT_XAM_TRACE("XamUserAreUsersFriends(user={}, unk1={:#x}, unk2={:#x}) -> {:#x}",
                  user_index, unk1, unk2, result);
    return result;
  }

  X_RESULT result;
  u32 are_friends = 0;
  if (user_index >= 4) {
    result = X_ERROR_INVALID_PARAMETER;
  } else if (!CoopUserSignedIn(user_index)) {
    result = X_ERROR_NO_SUCH_USER;
  } else {
    result = X_ERROR_SUCCESS;
  }

  if (out_value) {
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
    AOT_XAM_TRACE("XamUserAreUsersFriends(user={}) -> {:#x}, friends={}", user_index, result,
                  are_friends);
    return result;
  }
  if (overlapped_ptr) {
    auto complete_result =
        result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
    if (auto* kernel_state = rex::system::kernel_state()) {
      kernel_state->CompleteOverlappedImmediateEx(
          overlapped_ptr, complete_result, X_HRESULT_FROM_WIN32(result),
          result == X_ERROR_SUCCESS ? are_friends : 0);
    }
    AOT_XAM_TRACE("XamUserAreUsersFriends(user={}, overlapped={:#x}) -> pending/{:#x}",
                  user_index, overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_INVALID_PARAMETER;
}

u32 AotXamShowSigninUI_entry(u32 users, u32 flags) {
  if (!REXCVAR_GET(aot_coop_local)) {
    auto result = rex::kernel::xam::XamShowSigninUI_entry(users, flags);
    AOT_XAM_TRACE("XamShowSigninUI(users={}, flags={:#x}) -> {:#x}", users, flags, result);
    return result;
  }
  BroadcastSigninChanged();
  BroadcastUiClosed();
  AOT_XAM_TRACE("XamShowSigninUI(users={}, flags={:#x}) -> success, mask={:#x}", users, flags,
                CoopSignedInUserMask());
  return X_ERROR_SUCCESS;
}

u32 AotXamInputGetCapabilities_entry(u32 user_index, u32 flags,
                                     ppc_ptr_t<rex::input::X_INPUT_CAPABILITIES> caps) {
  if (!REXCVAR_GET(aot_coop_local)) {
    auto result = rex::kernel::xam::XamInputGetCapabilities_entry(user_index, flags, caps);
    AOT_XAM_TRACE("XamInputGetCapabilities(user={}, flags={:#x}) -> {:#x}", user_index, flags,
                  result);
    return result;
  }
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if ((flags & 0xFF) && (flags & kXinputFlagGamepad) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto actual_user_index = NormalizeInputUser(user_index, flags);
  auto* input = InputSystem();
  auto result = input ? input->GetCapabilities(actual_user_index, flags, caps)
                      : X_ERROR_DEVICE_NOT_CONNECTED;
  AOT_XAM_TRACE("XamInputGetCapabilities(user={}=>{}, flags={:#x}) -> {:#x}", user_index,
                actual_user_index, flags, result);
  return result;
}

u32 AotXamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                       ppc_ptr_t<rex::input::X_INPUT_CAPABILITIES> caps) {
  (void)unk;
  return AotXamInputGetCapabilities_entry(user_index, flags, caps);
}

u32 AotXamInputGetState_entry(u32 user_index, u32 flags,
                              ppc_ptr_t<rex::input::X_INPUT_STATE> input_state) {
  if (!REXCVAR_GET(aot_coop_local)) {
    auto result = rex::kernel::xam::XamInputGetState_entry(user_index, flags, input_state);
    AOT_XAM_TRACE("XamInputGetState(user={}, flags={:#x}) -> {:#x}", user_index, flags, result);
    return result;
  }
  if ((flags & 0xFF) && (flags & kXinputFlagGamepad) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto actual_user_index = NormalizeInputUser(user_index, flags);
  auto* input = InputSystem();
  auto result =
      input ? input->GetState(actual_user_index, input_state) : X_ERROR_DEVICE_NOT_CONNECTED;
  AOT_XAM_TRACE("XamInputGetState(user={}=>{}, flags={:#x}) -> {:#x}", user_index,
                actual_user_index, flags, result);
  return result;
}

u32 AotXamInputSetState_entry(u32 user_index, u32 unk,
                              ppc_ptr_t<rex::input::X_INPUT_VIBRATION> vibration) {
  if (!REXCVAR_GET(aot_coop_local)) {
    auto result = rex::kernel::xam::XamInputSetState_entry(user_index, unk, vibration);
    AOT_XAM_TRACE("XamInputSetState(user={}, unk={:#x}) -> {:#x}", user_index, unk, result);
    return result;
  }
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  auto actual_user_index = ((user_index & 0xFF) == 0xFF) ? 0 : user_index;
  auto* input = InputSystem();
  auto result =
      input ? input->SetState(actual_user_index, vibration) : X_ERROR_DEVICE_NOT_CONNECTED;
  AOT_XAM_TRACE("XamInputSetState(user={}=>{}, unk={:#x}) -> {:#x}", user_index,
                actual_user_index, unk, result);
  return result;
}

u32 AotXamInputGetKeystroke_entry(u32 user_index, u32 flags,
                                  ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke) {
  if (!REXCVAR_GET(aot_coop_local)) {
    auto result = rex::kernel::xam::XamInputGetKeystroke_entry(user_index, flags, keystroke);
    AOT_XAM_TRACE("XamInputGetKeystroke(user={}, flags={:#x}) -> {:#x}", user_index, flags,
                  result);
    return result;
  }
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if ((flags & 0xFF) && (flags & kXinputFlagGamepad) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto actual_user_index = NormalizeInputUser(user_index, flags);
  auto* input = InputSystem();
  auto result = input ? input->GetKeystroke(actual_user_index, flags, keystroke)
                      : X_ERROR_DEVICE_NOT_CONNECTED;
  AOT_XAM_TRACE("XamInputGetKeystroke(user={}=>{}, flags={:#x}) -> {:#x}", user_index,
                actual_user_index, flags, result);
  return result;
}

u32 AotXamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                    ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke) {
  if (!REXCVAR_GET(aot_coop_local)) {
    auto result = rex::kernel::xam::XamInputGetKeystrokeEx_entry(user_index_ptr, flags, keystroke);
    AOT_XAM_TRACE("XamInputGetKeystrokeEx(flags={:#x}) -> {:#x}", flags, result);
    return result;
  }
  if (!user_index_ptr || !keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  u32 user_index = *user_index_ptr;
  auto result = AotXamInputGetKeystroke_entry(user_index, flags, keystroke);
  if (result == X_ERROR_SUCCESS) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}

i32 AotXamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr) {
  if (!REXCVAR_GET(aot_coop_local)) {
    auto result = rex::kernel::xam::XamUserGetDeviceContext_entry(user_index, unk, out_ptr);
    AOT_XAM_TRACE("XamUserGetDeviceContext(user={}, unk={:#x}) -> {:#x}", user_index, unk,
                  static_cast<u32>(result));
    return result;
  }
  if (out_ptr) {
    *out_ptr = 0;
  }
  bool any_user = (user_index & 0xFF) == 0xFF;
  bool signed_in = any_user ? CoopUserSignedIn(0) : CoopUserSignedIn(user_index);
  auto result = signed_in ? X_E_SUCCESS : X_E_DEVICE_NOT_CONNECTED;
  AOT_XAM_TRACE("XamUserGetDeviceContext(user={}, unk={:#x}) -> {:#x}", user_index, unk,
                static_cast<u32>(result));
  return result;
}

u32 AotXamUserGetUserIndexMask_entry() {
  u32 mask = REXCVAR_GET(aot_coop_local) ? CoopSignedInUserMask() : 1;
  AOT_XAM_TRACE("XamUserGetUserIndexMask() -> {:#x}", mask);
  return mask;
}

u32 AotXamUserIsGuest_entry(u32 user_index) {
  AOT_XAM_TRACE("XamUserIsGuest(user={}) -> 0", user_index);
  return 0;
}

#undef AOT_XAM_TRACE

}  // namespace

REX_EXPORT(__imp__AotXamUserGetXUID, AotXamUserGetXUID_entry)
REX_EXPORT(__imp__AotXamUserGetSigninState, AotXamUserGetSigninState_entry)
REX_EXPORT(__imp__AotXamUserGetSigninInfo, AotXamUserGetSigninInfo_entry)
REX_EXPORT(__imp__AotXamUserGetName, AotXamUserGetName_entry)
REX_EXPORT(__imp__AotXamUserGetGamerTag, AotXamUserGetGamerTag_entry)
REX_EXPORT(__imp__AotXamUserReadProfileSettings, AotXamUserReadProfileSettings_entry)
REX_EXPORT(__imp__AotXamUserWriteProfileSettings, AotXamUserWriteProfileSettings_entry)
REX_EXPORT(__imp__AotXamUserCheckPrivilege, AotXamUserCheckPrivilege_entry)
REX_EXPORT(__imp__AotXamUserGetMembershipTier, AotXamUserGetMembershipTier_entry)
REX_EXPORT(__imp__AotXamUserAreUsersFriends, AotXamUserAreUsersFriends_entry)
REX_EXPORT(__imp__AotXamShowSigninUI, AotXamShowSigninUI_entry)
REX_EXPORT(__imp__AotXamInputGetCapabilities, AotXamInputGetCapabilities_entry)
REX_EXPORT(__imp__AotXamInputGetCapabilitiesEx, AotXamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__AotXamInputGetState, AotXamInputGetState_entry)
REX_EXPORT(__imp__AotXamInputSetState, AotXamInputSetState_entry)
REX_EXPORT(__imp__AotXamInputGetKeystroke, AotXamInputGetKeystroke_entry)
REX_EXPORT(__imp__AotXamInputGetKeystrokeEx, AotXamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__AotXamUserGetDeviceContext, AotXamUserGetDeviceContext_entry)
REX_EXPORT(__imp__AotXamUserGetUserIndexMask, AotXamUserGetUserIndexMask_entry)
REX_EXPORT(__imp__AotXamUserGetRequestedUserIndexMask, AotXamUserGetUserIndexMask_entry)
REX_EXPORT(__imp__AotXamUserIsGuest, AotXamUserIsGuest_entry)

namespace {

struct StaticImportHook {
  size_t guest_address;
  PPCFunc* hook;
  const char* name;
};

void InstallStaticImportHooks() {
  static constexpr std::array<StaticImportHook, 12> kHooks = {{
      {0x8308AAA4, __imp__AotXamUserGetName, "XamUserGetName"},
      {0x8308AAB4, __imp__AotXamUserGetSigninState, "XamUserGetSigninState"},
      {0x8308AAC4, __imp__AotXamUserAreUsersFriends, "XamUserAreUsersFriends"},
      {0x8308AAD4, __imp__AotXamUserCheckPrivilege, "XamUserCheckPrivilege"},
      {0x8308AB14, __imp__AotXamUserGetXUID, "XamUserGetXUID"},
      {0x8308AB34, __imp__AotXamInputGetCapabilities, "XamInputGetCapabilities"},
      {0x8308AB44, __imp__AotXamInputGetState, "XamInputGetState"},
      {0x8308AB54, __imp__AotXamInputSetState, "XamInputSetState"},
      {0x8308AB64, __imp__AotXamUserGetSigninInfo, "XamUserGetSigninInfo"},
      {0x8308AB74, __imp__AotXamShowSigninUI, "XamShowSigninUI"},
      {0x8308B6A4, __imp__AotXamUserReadProfileSettings, "XamUserReadProfileSettings"},
      {0x8308B6B4, __imp__AotXamUserWriteProfileSettings, "XamUserWriteProfileSettings"},
  }};

  for (const auto& hook : kHooks) {
    bool replaced = false;
    for (auto* mapping = PPCFuncMappings; mapping->guest != 0; ++mapping) {
      if (mapping->guest == hook.guest_address) {
        mapping->host = hook.hook;
        replaced = true;
        REXLOG_INFO("Libre Army of Two: local co-op hook {} at {:08X}", hook.name,
                    static_cast<u32>(hook.guest_address));
        break;
      }
    }
    if (!replaced) {
      REXLOG_WARN("Libre Army of Two: local co-op hook target missing for {} at {:08X}", hook.name,
                  static_cast<u32>(hook.guest_address));
    }
  }
}

rex::runtime::Export* AotXamExport(rex::u16 ordinal, const char* name) {
  auto* export_entry = new rex::runtime::Export(ordinal, rex::runtime::Export::Type::kFunction,
                                                name, rex::runtime::ExportTag::kImplemented);
  return rex::kernel::xam::RegisterExport_xam(export_entry);
}

void RegisterDynamicXamHooks() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;

  AotXamExport(0x0190, "AotXamInputGetCapabilities");
  AotXamExport(0x0191, "AotXamInputGetState");
  AotXamExport(0x0192, "AotXamInputSetState");
  AotXamExport(0x0193, "AotXamInputGetKeystroke");
  AotXamExport(0x0198, "AotXamInputGetKeystrokeEx");
  AotXamExport(0x0208, "AotXamUserGetDeviceContext");
  AotXamExport(0x020A, "AotXamUserGetXUID");
  AotXamExport(0x020C, "AotXamUserGetGamerTag");
  AotXamExport(0x020D, "AotXamUserGetUserIndexMask");
  AotXamExport(0x020E, "AotXamUserGetName");
  AotXamExport(0x0210, "AotXamUserGetSigninState");
  AotXamExport(0x0212, "AotXamUserCheckPrivilege");
  AotXamExport(0x0213, "AotXamUserAreUsersFriends");
  AotXamExport(0x0219, "AotXamUserReadProfileSettings");
  AotXamExport(0x021A, "AotXamUserWriteProfileSettings");
  AotXamExport(0x021B, "AotXamUserGetMembershipTier");
  AotXamExport(0x021D, "AotXamUserGetRequestedUserIndexMask");
  AotXamExport(0x021E, "AotXamUserIsGuest");
  AotXamExport(0x0227, "AotXamUserGetSigninInfo");
  AotXamExport(0x02AD, "AotXamInputGetCapabilitiesEx");
  AotXamExport(0x02BC, "AotXamShowSigninUI");
}

}  // namespace

void AotInstallLocalCoopHooks(rex::RuntimeConfig& config) {
  if (!HooksActive()) {
    return;
  }

  InstallStaticImportHooks();

  auto previous_kernel_init = std::move(config.kernel_init);
  config.kernel_init = [previous_kernel_init = std::move(previous_kernel_init)](
                           rex::Runtime* runtime, rex::system::KernelState* kernel_state) mutable {
    RegisterDynamicXamHooks();
    if (previous_kernel_init) {
      previous_kernel_init(runtime, kernel_state);
    } else {
      rex::kernel::InitializeKernel(runtime, kernel_state);
    }
  };

  REXLOG_INFO("Libre Army of Two: local co-op XAM hooks installed (coop={}, trace={})",
              REXCVAR_GET(aot_coop_local), REXCVAR_GET(aot_trace_xam));
}
