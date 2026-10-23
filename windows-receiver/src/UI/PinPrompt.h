#pragma once
// =============================================================================
// PinPrompt.h — Modal verification-code dialog for first-time pairing
// =============================================================================
// Shown on the UI thread, owned by the (possibly fullscreen) display window so
// it always appears above it. The network thread never blocks on user input.
// =============================================================================

#include <Windows.h>
#include <functional>
#include <string>

/// Show a modal dialog asking for the 6-digit verification code the Mac app
/// shows (protocol v2: both sides derive it from the handshake).
/// `stillValid` is polled while the dialog is open; when it returns false
/// (connection dropped, a newer request arrived) the dialog closes itself.
/// Returns true and fills outPIN when the user confirmed a 6-digit code.
bool PromptForPIN(HWND owner, std::string& outPIN,
                  const std::function<bool()>& stillValid);
