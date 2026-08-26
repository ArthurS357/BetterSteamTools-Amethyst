#pragma once

#include <string>

// TokeerBridge — the "redeem a code" bridge, driven by the amethysttool:// URI scheme.
//
// A public website opens amethysttool://redeem/<code>; Windows launches the OST DLL as the
// registered handler (rundll32 -> TokeerUri export), which calls into here. All work is
// standalone (registry credential store + HTTP), so it does NOT require the
// Steam-injected instance.
namespace TokeerBridge {

    // Parse and dispatch a "amethysttool://<action>/<arg>" URL. Unknown actions are ignored.
    void HandleUri(const std::string& url);

    // redeem: POST the code to the code server, then write the returned AppTicket +
    // ETicket to the credential store and show a confirmation popup.
    void Redeem(const std::string& code);

    // Register the amethysttool:// URI scheme under HKCU (no admin) pointing at this DLL via
    // rundll32. Safe to call every startup; only rewrites when the target changed.
    void RegisterUriScheme(const std::string& dllPath);

} // namespace TokeerBridge
