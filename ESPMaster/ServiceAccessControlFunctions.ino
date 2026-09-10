//Access control (CODE_REVIEW.md #29): a single password that can be globally disabled (empty
//string), plus per-category toggles deciding which state-changing endpoints stay reachable
//without it even while it's set. See the globals' own comments in ESPMaster.ino for the exact
//default/safety reasoning.

//True if this specific request should be allowed to proceed WITHOUT checking the password -
//either because no password is configured at all (global off-switch), or because the admin
//explicitly opted this specific category into the public allowlist. Pass `false` for
//categoryPublicToggle to mean "never toggleable, always protected while a password is set" -
//used by firmware/flash endpoints and the access-control settings themselves.
bool isRequestAllowedWithoutPassword(bool categoryPublicToggle) {
  return String(otaPassword) == "" || categoryPublicToggle;
}

//Call at the top of a protected handler. Sends 401 and returns true (meaning "stop, already
//responded") if the request should be rejected; returns false if it may proceed.
bool rejectIfPasswordRequired(AsyncWebServerRequest* request, bool categoryPublicToggle) {
  if (isRequestAllowedWithoutPassword(categoryPublicToggle)) {
    return false;
  }
  String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
  if (constantTimeEquals(providedPassword, String(otaPassword))) {
    return false;
  }
  request->send(401, "text/plain", "Password required - set it via the Administration page, or send it as the X-OTA-Password header");
  return true;
}
