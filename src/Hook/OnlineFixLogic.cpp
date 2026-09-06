#include "Hook/OnlineFixLogic.h"

namespace OnlineFixLogic {

bool ShouldSuppressAppIdFlip(std::string_view cmdLine) noexcept {
    return cmdLine.find("-realappid") != std::string_view::npos;
}

bool ShouldReportOnlineFixAppId(bool suppressFlip, bool hasRealAppId, bool networkingActive) noexcept {
    if (suppressFlip) return false;
    return hasRealAppId && networkingActive;
}

} // namespace OnlineFixLogic
