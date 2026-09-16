//A single password, globally disable-able (empty string), plus per-category toggles deciding
//which endpoints stay reachable without it even while set.

//True if no password is required for this category - either the global off-switch, or the admin
//opted this category into the public allowlist. Pass `false` for categoryPublicToggle to mean
//"always protected while a password is set" (firmware/flash endpoints, access-control itself).
bool isRequestAllowedWithoutPassword(bool categoryPublicToggle) {
  return String(otaPassword) == "" || categoryPublicToggle;
}

//Call at the top of a protected handler. Sends 401 and returns true if rejected.
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
