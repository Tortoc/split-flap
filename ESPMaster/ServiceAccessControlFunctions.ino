//A single password, globally disable-able (empty string), plus per-category toggles deciding
//which endpoints stay reachable without it even while set.

//True if no password is required for this category - either the global off-switch, or the admin
//opted this category into the public allowlist. Pass `false` for categoryPublicToggle to mean
//"always protected while a password is set" (firmware/flash endpoints, access-control itself).
bool isRequestAllowedWithoutPassword(bool categoryPublicToggle) {
  return String(otaPassword) == "" || categoryPublicToggle;
}

//Login sessions for the web UI: POST /api/login checks the password once and hands the browser a
//random session token as an HttpOnly cookie; every protected endpoint then accepts either that
//cookie or the X-OTA-Password header as before (scripts/automations keep working unchanged).
//A session stays valid for LOGIN_SESSION_DAYS after its LAST use (renewed at most once a day, so
//regular use never logs out and flash isn't written on every request). Kept per device, persisted
//in their own small file so a reboot doesn't log everyone out. Changing the password ends all
//sessions.
//
//CSRF: browsers attach the cookie to cross-site requests too, so a cookie only counts together
//with the X-Requested-With header the web UI sends on every request - another site can't add that
//header without a CORS preflight this device never approves. SameSite=Strict on top.
//LOGIN_SESSION_* constants, loginSessionsPath and lastFailedLoginAt live in ESPMaster.ino (used by
//its /api/login handlers, and that file is compiled first).
struct LoginSession {
  char token[33] = ""; //32 hex chars (128 random bits), "" = free slot
  uint32_t expiresAt = 0; //real UTC; 0 = not known yet (no valid clock when it was created)
};
LoginSession loginSessions[LOGIN_SESSIONS_MAX];
//Loaded on the first check that needs it, not at boot - every file open costs ~800ms on this
//device's flash chip (see bootSnapshotPath's comment), and boot time matters more.
bool loginSessionsLoaded = false;
bool loginSessionsDirty = false; //saved from loop(), see saveLoginSessionsIfDirty()

//Before this, the clock hasn't been set yet (no RTC/NTP) - expiry can't be judged.
bool isRealClockKnown(long now) {
  return now > 1700000000L;
}

void restoreLoginSessionFromJson(JsonVariant value) {
  const char* token = value["t"] | "";
  for (int i = 0; i < LOGIN_SESSIONS_MAX; i++) {
    if (loginSessions[i].token[0] == '\0') {
      if (strlen(token) == 32) {
        strncpy(loginSessions[i].token, token, 32);
        loginSessions[i].token[32] = '\0';
        loginSessions[i].expiresAt = value["e"] | 0UL;
      }
      return;
    }
  }
}

void ensureLoginSessionsLoaded() {
  if (!loginSessionsLoaded) {
    loginSessionsLoaded = true;
    readJsonArrayFile(loginSessionsPath, restoreLoginSessionFromJson);
  }
}

void fillLoginSessionJson(int index, JsonDocument &element) {
  element["t"] = loginSessions[index].token;
  element["e"] = loginSessions[index].expiresAt;
}

//Ticked from loop() - flash writes stay out of the AsyncWebServer callbacks that change sessions.
//Packs the used slots to the front first, so the file only lists real sessions.
void saveLoginSessionsIfDirty() {
  if (!loginSessionsDirty) {
    return;
  }
  loginSessionsDirty = false;
  int used = 0;
  for (int i = 0; i < LOGIN_SESSIONS_MAX; i++) {
    if (loginSessions[i].token[0] != '\0') {
      if (i != used) {
        loginSessions[used] = loginSessions[i];
        loginSessions[i] = LoginSession();
      }
      used++;
    }
  }
  if (!writeJsonArrayFile(loginSessionsPath, used, fillLoginSessionJson)) {
    loginSessionsDirty = true; //try again on a later pass
  }
}

String requestSessionToken(AsyncWebServerRequest* request) {
  if (!request->hasHeader("Cookie")) {
    return "";
  }
  String cookies = request->getHeader("Cookie")->value();
  int start = cookies.indexOf(LOGIN_SESSION_COOKIE "=");
  if (start == -1) {
    return "";
  }
  start += strlen(LOGIN_SESSION_COOKIE "=");
  int end = cookies.indexOf(';', start);
  String token = end == -1 ? cookies.substring(start) : cookies.substring(start, end);
  token.trim();
  return token;
}

//Slot index of the request's valid session, or -1. Drops an expired one, renews a valid one.
int findValidLoginSession(AsyncWebServerRequest* request) {
  String token = requestSessionToken(request);
  if (token.length() != 32) {
    return -1;
  }
  ensureLoginSessionsLoaded();
  long now = getRealUtcNow();
  bool clockKnown = isRealClockKnown(now);
  for (int i = 0; i < LOGIN_SESSIONS_MAX; i++) {
    if (loginSessions[i].token[0] == '\0' || !constantTimeEquals(token, String(loginSessions[i].token))) {
      continue;
    }
    if (clockKnown) {
      if (loginSessions[i].expiresAt != 0 && (uint32_t)now > loginSessions[i].expiresAt) {
        loginSessions[i] = LoginSession();
        loginSessionsDirty = true;
        return -1;
      }
      //Sliding expiry - renewed at most once a day.
      if (loginSessions[i].expiresAt == 0 || loginSessions[i].expiresAt - (uint32_t)now < LOGIN_SESSION_SECONDS - 86400UL) {
        loginSessions[i].expiresAt = (uint32_t)now + LOGIN_SESSION_SECONDS;
        loginSessionsDirty = true;
      }
    }
    return i;
  }
  return -1;
}

//Password header, or a session cookie sent by the web UI itself (see the CSRF note above).
//Always true while no password is set.
bool requestIsAuthenticated(AsyncWebServerRequest* request) {
  if (String(otaPassword) == "") {
    return true;
  }
  String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
  if (providedPassword != "" && constantTimeEquals(providedPassword, String(otaPassword))) {
    return true;
  }
  bool fromWebUi = request->hasHeader("X-Requested-With") && request->getHeader("X-Requested-With")->value() == "splitflap";
  return fromWebUi && findValidLoginSession(request) != -1;
}

//Takes a free slot, or replaces the one closest to expiry. Returns the new token.
String createLoginSession() {
  ensureLoginSessionsLoaded();
  int slot = 0;
  for (int i = 0; i < LOGIN_SESSIONS_MAX; i++) {
    if (loginSessions[i].token[0] == '\0') {
      slot = i;
      break;
    }
    if (loginSessions[i].expiresAt < loginSessions[slot].expiresAt) {
      slot = i;
    }
  }
  uint8_t randomBytes[16];
  ESP.random(randomBytes, sizeof(randomBytes));
  for (int i = 0; i < 16; i++) {
    sprintf(&loginSessions[slot].token[i * 2], "%02x", randomBytes[i]);
  }
  long now = getRealUtcNow();
  loginSessions[slot].expiresAt = isRealClockKnown(now) ? (uint32_t)now + LOGIN_SESSION_SECONDS : 0;
  loginSessionsDirty = true;
  return String(loginSessions[slot].token);
}

void endLoginSession(AsyncWebServerRequest* request) {
  int slot = findValidLoginSession(request);
  if (slot != -1) {
    loginSessions[slot] = LoginSession();
    loginSessionsDirty = true;
  }
}

//On every password change - a session must never outlive the password it was created with.
void endAllLoginSessions() {
  loginSessionsLoaded = true; //nothing on flash needs reading, it's all being replaced
  for (int i = 0; i < LOGIN_SESSIONS_MAX; i++) {
    loginSessions[i] = LoginSession();
  }
  loginSessionsDirty = true;
}

//Whether the web UI should ask for the password right when it opens: one is set, and at least
//one category still needs it.
bool isLoginRequired() {
  return String(otaPassword) != "" && !(accessControlTextPublic && accessControlSettingsPublic &&
    accessControlUnitsPublic && accessControlWifiPublic && accessControlTimezonePublic && accessControlSystemPublic);
}

//Call at the top of a protected handler. Sends 401 and returns true if rejected.
bool rejectIfPasswordRequired(AsyncWebServerRequest* request, bool categoryPublicToggle) {
  if (isRequestAllowedWithoutPassword(categoryPublicToggle) || requestIsAuthenticated(request)) {
    return false;
  }
  request->send(401, "text/plain", "Password required - log in on the web page, or send it as the X-OTA-Password header");
  return true;
}
