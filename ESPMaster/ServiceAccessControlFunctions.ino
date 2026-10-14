//A single password, globally disable-able (empty string), plus per-category toggles deciding
//which endpoints stay reachable without it even while set.

//The password is stored only as a salted PBKDF2-HMAC-SHA256 hash, "<iterations>$<salt hex>$<hash
//hex>", so a flash dump doesn't reveal it; the iteration count is stored with it. Every wrong guess
//costs one full hash; checkAdminPassword()'s per-sender pause bounds that.
#define ADMIN_PASSWORD_HASH_ITERATIONS 1000
#define ADMIN_PASSWORD_SALT_BYTES 16

String bytesToHex(const uint8_t *bytes, size_t length) {
  static const char hexDigits[] = "0123456789abcdef";
  String hex;
  hex.reserve(length * 2);
  for (size_t i = 0; i < length; i++) {
    hex += hexDigits[bytes[i] >> 4];
    hex += hexDigits[bytes[i] & 0x0F];
  }
  return hex;
}

//False unless `hex` is exactly 2 * length lowercase/uppercase hex digits.
bool hexToBytes(const String &hex, uint8_t *bytes, size_t length) {
  if (hex.length() != length * 2) {
    return false;
  }
  for (size_t i = 0; i < length * 2; i++) {
    char c = hex[i];
    uint8_t nibble;
    if (c >= '0' && c <= '9') nibble = c - '0';
    else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
    else return false;
    bytes[i / 2] = (i % 2 == 0) ? (nibble << 4) : (bytes[i / 2] | nibble);
  }
  return true;
}

//HMAC-SHA256 of `message` - shared by the password cache below and the primary->extension link.
void hmacSha256(const uint8_t *key, size_t keyLength, const uint8_t *message, size_t messageLength, uint8_t out[32]) {
  br_hmac_key_context keyContext;
  br_hmac_key_init(&keyContext, &br_sha256_vtable, key, keyLength);
  br_hmac_context context;
  br_hmac_init(&context, &keyContext, 0);
  br_hmac_update(&context, message, messageLength);
  br_hmac_out(&context, out);
}

//PBKDF2-HMAC-SHA256, one 32-byte block (RFC 8018). The key context is set up once, so every
//iteration costs just two SHA-256 compressions.
void pbkdf2Sha256(const String &password, const uint8_t *salt, size_t saltLength, uint32_t iterations, uint8_t out[32]) {
  br_hmac_key_context keyContext;
  br_hmac_key_init(&keyContext, &br_sha256_vtable, password.c_str(), password.length());
  br_hmac_context context;
  uint8_t block[32];
  static const uint8_t blockIndex[4] = { 0, 0, 0, 1 };
  br_hmac_init(&context, &keyContext, 0);
  br_hmac_update(&context, salt, saltLength);
  br_hmac_update(&context, blockIndex, sizeof(blockIndex));
  br_hmac_out(&context, block);
  memcpy(out, block, 32);
  for (uint32_t i = 1; i < iterations; i++) {
    br_hmac_init(&context, &keyContext, 0);
    br_hmac_update(&context, block, sizeof(block));
    br_hmac_out(&context, block);
    for (int j = 0; j < 32; j++) {
      out[j] ^= block[j];
    }
  }
}

//"" for an empty password (= protection off), otherwise a fresh salt and its hash.
String hashAdminPassword(const String &password) {
  if (password == "") {
    return "";
  }
  uint8_t salt[ADMIN_PASSWORD_SALT_BYTES];
  ESP.random(salt, sizeof(salt));
  uint8_t hash[32];
  pbkdf2Sha256(password, salt, sizeof(salt), ADMIN_PASSWORD_HASH_ITERATIONS, hash);
  return String(ADMIN_PASSWORD_HASH_ITERATIONS) + '$' + bytesToHex(salt, sizeof(salt)) + '$' + bytesToHex(hash, sizeof(hash));
}

bool isAdminPasswordSet() {
  return adminPasswordHash != "";
}

//Scripts and uploads send the password as a header on every request - an OTA upload alone is
//~150 chunk requests. The last password that checked out is remembered as an HMAC under a key
//that only lives in RAM for this boot, so a repeat costs one HMAC instead of a full PBKDF2.
uint8_t verifiedPasswordCacheKey[32];
bool verifiedPasswordCacheKeyReady = false;
uint8_t verifiedPasswordDigest[32];
bool verifiedPasswordCached = false;

void verifiedPasswordCacheDigest(const String &password, uint8_t out[32]) {
  if (!verifiedPasswordCacheKeyReady) {
    ESP.random(verifiedPasswordCacheKey, sizeof(verifiedPasswordCacheKey));
    verifiedPasswordCacheKeyReady = true;
  }
  hmacSha256(verifiedPasswordCacheKey, sizeof(verifiedPasswordCacheKey), (const uint8_t*)password.c_str(), password.length(), out);
}

bool constantTimeEqualBytes(const uint8_t *a, const uint8_t *b, size_t length) {
  uint8_t diff = 0;
  for (size_t i = 0; i < length; i++) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}

//Compares against the stored hash. Without a password only "" matches - the password change
//accepts an empty currentPassword then, as it always has.
bool verifyAdminPassword(const String &provided) {
  if (!isAdminPasswordSet()) {
    return provided == "";
  }
  uint8_t digest[32];
  verifiedPasswordCacheDigest(provided, digest);
  if (verifiedPasswordCached && constantTimeEqualBytes(digest, verifiedPasswordDigest, sizeof(digest))) {
    return true;
  }
  int firstSeparator = adminPasswordHash.indexOf('$');
  int secondSeparator = adminPasswordHash.indexOf('$', firstSeparator + 1);
  if (firstSeparator <= 0 || secondSeparator <= firstSeparator) {
    return false;
  }
  long iterations = adminPasswordHash.substring(0, firstSeparator).toInt();
  uint8_t salt[ADMIN_PASSWORD_SALT_BYTES];
  uint8_t expected[32];
  //Bounded so a damaged file can't make every check take minutes.
  if (iterations < 1 || iterations > 100000 ||
      !hexToBytes(adminPasswordHash.substring(firstSeparator + 1, secondSeparator), salt, sizeof(salt)) ||
      !hexToBytes(adminPasswordHash.substring(secondSeparator + 1), expected, sizeof(expected))) {
    return false;
  }
  uint8_t actual[32];
  pbkdf2Sha256(provided, salt, sizeof(salt), (uint32_t)iterations, actual);
  if (!constantTimeEqualBytes(actual, expected, sizeof(actual))) {
    return false;
  }
  memcpy(verifiedPasswordDigest, digest, sizeof(digest));
  verifiedPasswordCached = true;
  return true;
}

//New admin password ("" = protection off): hashed, persisted, and the cache forgets the old one.
void setAdminPassword(const String &password) {
  adminPasswordHash = hashAdminPassword(password);
  verifiedPasswordCached = false;
  writeFile(LittleFS, adminPasswordHashPath, adminPasswordHash.c_str());
}

//True if no password is required for this category - either the global off-switch, or the admin
//opted this category into the public allowlist. Pass `false` for categoryPublicToggle to mean
//"always protected while a password is set" (firmware/flash endpoints, access-control itself).
bool isRequestAllowedWithoutPassword(bool categoryPublicToggle) {
  return !isAdminPasswordSet() || categoryPublicToggle;
}

//Web UI login: POST /api/login checks the password once and sets a random session token as an
//HttpOnly cookie; protected endpoints accept that cookie or the X-OTA-Password header (scripts keep
//working). A session lasts LOGIN_SESSION_DAYS after its last use (renewed at most daily, so flash
//isn't written per request), survives a reboot and ends with a password change.
//
//CSRF: the cookie only counts together with the X-Requested-With header the web UI sends - another
//site can't add it without a CORS preflight this device never approves. SameSite=Strict on top.
//LOGIN_SESSION_*, loginSessionsPath and failedLogins live in ESPMaster.ino (compiled first).
struct LoginSession {
  char token[33] = ""; //32 hex chars (128 random bits), "" = free slot
  uint32_t expiresAt = 0; //real UTC; 0 = not known yet (no valid clock when it was created)
};
LoginSession loginSessions[LOGIN_SESSIONS_MAX];
//Loaded on first use, not at boot - each file open costs ~800ms on this flash chip.
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
//Saves right away (logout, password change) instead of waiting for loop(). True once the file
//matches memory.
bool saveLoginSessionsNow() {
  saveLoginSessionsIfDirty();
  return !loginSessionsDirty;
}

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

//Every admin-password comparison goes through here (login, X-OTA-Password header, pairing,
//password change), so a wrong guess pauses all of them for 2 seconds - for that sender only. While
//paused nothing is compared, not even a correct password. If FAILED_LOGIN_SLOTS senders are paused,
//any further sender waits too, so more addresses don't buy more guesses. Sessions are unaffected.
int checkAdminPassword(AsyncWebServerRequest* request, const String &provided) {
  uint32_t ip = request->client() != nullptr ? (uint32_t)request->client()->remoteIP() : 0;
  unsigned long now = millis();
  int freeSlot = -1;
  for (int i = 0; i < FAILED_LOGIN_SLOTS; i++) {
    bool paused = failedLogins[i].at != 0 && now - failedLogins[i].at < FAILED_LOGIN_PAUSE_MS;
    if (paused && failedLogins[i].ip == ip) {
      return PASSWORD_THROTTLED;
    }
    if (!paused && freeSlot == -1) {
      freeSlot = i;
    }
  }
  if (freeSlot == -1) {
    return PASSWORD_THROTTLED;
  }
  if (verifyAdminPassword(provided)) {
    return PASSWORD_OK;
  }
  failedLogins[freeSlot].ip = ip;
  failedLogins[freeSlot].at = now == 0 ? 1 : now;
  return PASSWORD_WRONG;
}

//Password header, or a session cookie sent by the web UI itself (see the CSRF note above).
//Always true while no password is set.
bool requestIsAuthenticated(AsyncWebServerRequest* request) {
  if (!isAdminPasswordSet()) {
    return true;
  }
  String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
  if (providedPassword != "" && checkAdminPassword(request, providedPassword) == PASSWORD_OK) {
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
  return isAdminPasswordSet() && !(accessControlTextPublic && accessControlSettingsPublic &&
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
