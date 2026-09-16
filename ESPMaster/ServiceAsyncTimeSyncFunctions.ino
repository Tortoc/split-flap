//Non-blocking replacement for ezTime's own network operations (Timezone::setLocation() and the
//NTP sync normally driven by events()/updateNTP()) - see CODE_REVIEW.md #30: the original,
//still-vendored ezTime calls do a blocking DNS-then-UDP round-trip that can exceed the software
//watchdog's patience under real network stress - live-reproduced as a genuine device crash.
//
//This file avoids the blocking calls entirely instead of working around them, using lwIP's own
//callback-based dns_gethostbyname() and WiFiUDP::parsePacket()'s already-non-blocking mode. Neither
//state machine below ever blocks loop() for longer than a cheap, immediate check.
//
//ezTime's own NTP auto-sync is fully disabled (setInterval(0) in setup()) - checkNtpSync() below
//takes over that job instead. Timezone changes go through checkTimezoneLookup().
//
//IMPORTANT - NOT just calling the obvious-looking public setTime(t): ezTime's own
//now()/getRealUtcNow() have a well-documented sign-inversion quirk - naively calling
//Timezone::setTime() with a raw UTC value reproduces the exact double-offset-conversion bug class
//already fixed once in this project. See ntpSyncApplyResult()'s comment below.

#include <lwip/dns.h>

//===== Shared non-blocking DNS lookup, wrapping lwIP's own callback-based resolver =====

enum AsyncDnsState { ASYNC_DNS_IDLE, ASYNC_DNS_PENDING, ASYNC_DNS_DONE };

struct AsyncDnsLookup {
  AsyncDnsState state = ASYNC_DNS_IDLE;
  //lwIP invokes dnsFoundCallback() from its own timer/event processing, a different call stack
  //than this struct's own isDone()/tick() callers - volatile keeps the compiler from caching a
  //stale value across that boundary.
  volatile bool callbackFired = false;
  volatile bool found = false;
  IPAddress resultIp;

  //Only call this when state == ASYNC_DNS_IDLE (or after isDone() returned true) - starting a new
  //lookup while one is still pending would leave the old callback writing into this same struct.
  void start(const char* hostname) {
    state = ASYNC_DNS_PENDING;
    callbackFired = false;
    found = false;
    ip_addr_t addr;
    err_t err = dns_gethostbyname(hostname, &addr, &AsyncDnsLookup::dnsFoundCallback, this);
    if (err == ERR_OK) {
      //Already cached by lwIP - resolved synchronously right here, no callback will fire.
      resultIp = IPAddress(&addr);
      found = true;
      callbackFired = true;
      state = ASYNC_DNS_DONE;
    } else if (err != ERR_INPROGRESS) {
      //Immediate failure (bad hostname, out of memory, ...) - no callback will fire either.
      found = false;
      callbackFired = true;
      state = ASYNC_DNS_DONE;
    }
    //else ERR_INPROGRESS: genuinely pending - dnsFoundCallback() will fire later, polled via isDone().
  }

  bool isDone() {
    if (callbackFired && state == ASYNC_DNS_PENDING) {
      state = ASYNC_DNS_DONE;
    }
    return state == ASYNC_DNS_DONE;
  }

  static void dnsFoundCallback(const char* name, const ip_addr_t* ipaddr, void* arg) {
    AsyncDnsLookup* self = (AsyncDnsLookup*)arg;
    if (ipaddr != nullptr) {
      self->resultIp = IPAddress(ipaddr);
      self->found = true;
    } else {
      self->found = false;
    }
    self->callbackFired = true;
  }
};

//===== Timezone lookup (replaces Timezone::setLocation()'s network part) =====

enum TzLookupState { TZ_LOOKUP_IDLE, TZ_LOOKUP_RESOLVING, TZ_LOOKUP_WAITING };
TzLookupState tzLookupState = TZ_LOOKUP_IDLE;
AsyncDnsLookup tzDnsLookup;
WiFiUDP tzLookupUdp;
unsigned long tzLookupPhaseStartedAt = 0;
String tzLookupLocation;
//tzLookupResult/tzLookupDone (valid once tzLookupDone == true, polled by ESPMaster.ino's
//pendingTimezoneSlot handling) are declared in ESPMaster.ino instead of here - see the comment
//there.

#define TZ_LOOKUP_STEP_TIMEOUT_MS 2000UL //matches ezTime's own TIMEZONED_TIMEOUT
#define TIMEZONED_HOST "timezoned.rop.nl" //matches ezTime.h's TIMEZONED_REMOTE_HOST
#define TIMEZONED_PORT 2342               //matches ezTime.h's TIMEZONED_REMOTE_PORT/_LOCAL_PORT

bool isTimezoneLookupBusy() {
  return tzLookupState != TZ_LOOKUP_IDLE;
}

void startTimezoneLookup(const String &location) {
  tzLookupLocation = location;
  tzLookupDone = false;
  tzLookupResult = false;
  tzLookupPhaseStartedAt = millis();
  tzDnsLookup.start(TIMEZONED_HOST);
  tzLookupState = TZ_LOOKUP_RESOLVING;
}

void checkTimezoneLookup() {
  switch (tzLookupState) {
    case TZ_LOOKUP_IDLE:
      return;

    case TZ_LOOKUP_RESOLVING:
      if (!tzDnsLookup.isDone()) {
        if (millisReached(tzLookupPhaseStartedAt + TZ_LOOKUP_STEP_TIMEOUT_MS)) {
          SerialPrintln(F("Timezone lookup: DNS resolution timed out"));
          tzLookupResult = false;
          tzLookupDone = true;
          tzLookupState = TZ_LOOKUP_IDLE;
        }
        return;
      }
      if (!tzDnsLookup.found) {
        SerialPrintln(F("Timezone lookup: DNS resolution failed"));
        tzLookupResult = false;
        tzLookupDone = true;
        tzLookupState = TZ_LOOKUP_IDLE;
        return;
      }
      tzLookupUdp.flush();
      tzLookupUdp.begin(TIMEZONED_PORT);
      tzLookupUdp.beginPacket(tzDnsLookup.resultIp, TIMEZONED_PORT);
      tzLookupUdp.write((const uint8_t*)tzLookupLocation.c_str(), tzLookupLocation.length());
      tzLookupUdp.endPacket();
      tzLookupPhaseStartedAt = millis();
      tzLookupState = TZ_LOOKUP_WAITING;
      return;

    case TZ_LOOKUP_WAITING: {
      int packetSize = tzLookupUdp.parsePacket(); //non-blocking - 0 immediately if nothing yet
      if (packetSize == 0) {
        if (millisReached(tzLookupPhaseStartedAt + TZ_LOOKUP_STEP_TIMEOUT_MS)) {
          SerialPrintln(F("Timezone lookup: response timed out"));
          tzLookupUdp.stop();
          tzLookupResult = false;
          tzLookupDone = true;
          tzLookupState = TZ_LOOKUP_IDLE;
        }
        return;
      }
      //Only trust a reply from the exact IP/port this request was actually sent to - not
      //cryptographic authentication, just rules out random/unrelated traffic on this fixed local
      //port. Ignore and keep waiting rather than treating it as a hard failure.
      if (tzLookupUdp.remoteIP() != tzDnsLookup.resultIp || tzLookupUdp.remotePort() != TIMEZONED_PORT) {
        SerialPrintln(F("Timezone lookup: ignoring UDP packet from an unexpected sender"));
        while (tzLookupUdp.available()) tzLookupUdp.read(); //discard it, keep the socket open for the real reply
        return;
      }

      String recv;
      recv.reserve(60);
      while (tzLookupUdp.available()) recv += (char)tzLookupUdp.read();
      tzLookupUdp.stop();

      //Same "OK <olson> <posix>"/"ERROR <msg>" protocol as ezTime's own setLocation() - see its
      //comment. setPosix() (public API) applies the result without a second network round-trip.
      if (recv.substring(0, 3) == "OK ") {
        String posix = recv.substring(recv.indexOf(" ", 4) + 1);
        tzLookupResult = timezone.setPosix(posix);
        //Cache the resolved POSIX rule for next boot - only touches flash when it actually changed.
        if (tzLookupResult && posix != cachedTimezonePosix) {
          cachedTimezonePosix = posix;
          writeFile(LittleFS, timezonePosixCachePath, cachedTimezonePosix.c_str());
          SerialPrintln(F("Timezone lookup: cached new POSIX rule for next boot: ") + posix);
        }
      } else {
        SerialPrintln(F("Timezone lookup: server rejected '") + tzLookupLocation + F("': ") + recv);
        tzLookupResult = false;
      }
      tzLookupDone = true;
      tzLookupState = TZ_LOOKUP_IDLE;
      return;
    }
  }
}

//===== NTP sync (replaces ezTime's own events()-driven updateNTP()/queryNTP()) =====

enum NtpSyncState { NTP_SYNC_IDLE, NTP_SYNC_RESOLVING, NTP_SYNC_WAITING };
NtpSyncState ntpSyncState = NTP_SYNC_IDLE;
AsyncDnsLookup ntpDnsLookup;
WiFiUDP ntpSyncUdp;
unsigned long ntpSyncPhaseStartedAt = 0;
unsigned long ntpSyncNextDueAt = 0; //millis()-based, overflow-safe via millisReached()
bool ntpSyncEverSucceeded = false;

#define NTP_SYNC_STEP_TIMEOUT_MS 2000UL      //per-phase (DNS or UDP-wait) timeout
#define NTP_SYNC_INTERVAL_MS (21600UL * 1000UL) //6h - matches the previous setInterval(21600)
#define NTP_SYNC_RETRY_MS (20UL * 1000UL)       //20s - matches ezTime's own hardcoded NTP_RETRY
#define NTP_SYNC_SERVER "pool.ntp.org"           //matches ezTime.h's NTP_SERVER default
#define NTP_SYNC_LOCAL_PORT 4243 //ezTime's own NTP_LOCAL_PORT (4242) stays free for ezTime itself
#define NTP_SYNC_PACKET_SIZE 48  //matches ezTime.h's NTP_PACKET_SIZE

void startNtpSync() {
  if (WiFi.status() != WL_CONNECTED) {
    ntpSyncNextDueAt = millis() + NTP_SYNC_RETRY_MS;
    return; //matches ezTime's own NO_NETWORK short-circuit in queryNTP()
  }
  ntpSyncPhaseStartedAt = millis();
  ntpDnsLookup.start(NTP_SYNC_SERVER);
  ntpSyncState = NTP_SYNC_RESOLVING;
}

//Applies a raw NTP-derived UTC unix timestamp to ezTime's internal clock, producing the SAME
//_last_sync_time state ezTime's own updateNTP() would have - only the public
//Timezone::setPosix()/setTime()/getOffset() API is available here (ezTime is a shared system
//library, not vendored in-repo).
//
//Timezone::setTime(t, ms) computes: offset = getOffset(t) [treats t AS local]; _last_sync_time =
//t + offset*60. ezTime's offset is POSIX-sign-inverted (NEGATIVE for zones ahead of UTC), so
//calling setTime(rawUtc) directly would NOT reproduce _last_sync_time = rawUtc.
//
//Fix: pre-compensate by computing offsetGuess = getOffset(rawUtc, UTC_TIME) - the offset AS IF
//rawUtc were genuine UTC - then call setTime(rawUtc - offsetGuess*60). setTime()'s own internal
//getOffset(t) then recomputes the SAME offsetGuess, so _last_sync_time ends up = rawUtc, matching
//updateNTP(). Self-checked below via getRealUtcNow(), the mathematical inverse of this relationship.
void ntpSyncApplyResult(time_t rawUtc) {
  int16_t offsetGuess = timezone.getOffset(rawUtc, UTC_TIME);
  timezone.setTime(rawUtc - (long)offsetGuess * 60);

  long checkUtc = getRealUtcNow();
  long driftSeconds = checkUtc - (long)rawUtc;
  if (driftSeconds < -5 || driftSeconds > 5) {
    //Self-check failed by more than plausible round-trip time - logged loudly rather than
    //silently trusting a possibly-wrong clock, since every time-dependent feature depends on it.
    SerialPrintln(F("WARNING: NTP sync self-check drift ") + String(driftSeconds) + F("s - getRealUtcNow() doesn't match the raw NTP value as expected, double-check ntpSyncApplyResult()'s offset math"));
  }
}

void checkNtpSync() {
  switch (ntpSyncState) {
    case NTP_SYNC_IDLE:
      if (millisReached(ntpSyncNextDueAt)) {
        startNtpSync();
      }
      return;

    case NTP_SYNC_RESOLVING:
      if (!ntpDnsLookup.isDone()) {
        if (millisReached(ntpSyncPhaseStartedAt + NTP_SYNC_STEP_TIMEOUT_MS)) {
          SerialPrintln(F("NTP sync: DNS resolution timed out"));
          ntpSyncState = NTP_SYNC_IDLE;
          ntpSyncNextDueAt = millis() + NTP_SYNC_RETRY_MS;
        }
        return;
      }
      if (!ntpDnsLookup.found) {
        SerialPrintln(F("NTP sync: DNS resolution failed"));
        ntpSyncState = NTP_SYNC_IDLE;
        ntpSyncNextDueAt = millis() + NTP_SYNC_RETRY_MS;
        return;
      }
      {
        byte buffer[NTP_SYNC_PACKET_SIZE];
        memset(buffer, 0, NTP_SYNC_PACKET_SIZE);
        buffer[0] = 0b11100011; // LI, Version, Mode
        buffer[1] = 0;
        buffer[2] = 9;
        buffer[3] = 0xEC;
        buffer[12] = 'X'; buffer[13] = 'E'; buffer[14] = 'Z'; buffer[15] = 'T'; //"kiss code", RFC5905

        ntpSyncUdp.flush();
        ntpSyncUdp.begin(NTP_SYNC_LOCAL_PORT);
        ntpSyncUdp.beginPacket(ntpDnsLookup.resultIp, 123); //NTP requests go to port 123
        ntpSyncUdp.write(buffer, NTP_SYNC_PACKET_SIZE);
        ntpSyncUdp.endPacket();
      }
      ntpSyncPhaseStartedAt = millis();
      ntpSyncState = NTP_SYNC_WAITING;
      return;

    case NTP_SYNC_WAITING: {
      int packetSize = ntpSyncUdp.parsePacket(); //non-blocking - 0 immediately if nothing yet
      if (packetSize == 0) {
        if (millisReached(ntpSyncPhaseStartedAt + NTP_SYNC_STEP_TIMEOUT_MS)) {
          SerialPrintln(F("NTP sync: response timed out"));
          ntpSyncUdp.stop();
          ntpSyncState = NTP_SYNC_IDLE;
          ntpSyncNextDueAt = millis() + NTP_SYNC_RETRY_MS;
        }
        return;
      }

      //Same sender check as the timezone lookup's own UDP receive - an accepted NTP reply here is
      //applied directly as the system time, which every time-dependent feature then trusts.
      if (ntpSyncUdp.remoteIP() != ntpDnsLookup.resultIp || ntpSyncUdp.remotePort() != 123) {
        SerialPrintln(F("NTP sync: ignoring UDP packet from an unexpected sender"));
        while (ntpSyncUdp.available()) ntpSyncUdp.read(); //discard it, keep the socket open for the real reply
        return;
      }

      byte buffer[NTP_SYNC_PACKET_SIZE]; //deliberately NOT pre-zeroed - see the length check below
      int bytesRead = ntpSyncUdp.read(buffer, NTP_SYNC_PACKET_SIZE);
      //A short/malformed datagram must be dropped, not parsed - a nonzero-but-short packet would
      //otherwise leave uninitialized stack garbage read from fixed offsets below, which could
      //plausibly pass the plausibility checks further down and get accepted as a real time sync.
      if (packetSize < NTP_SYNC_PACKET_SIZE || bytesRead < NTP_SYNC_PACKET_SIZE) {
        SerialPrintln(F("NTP sync: ignoring short/malformed packet (") + String(packetSize) + F(" bytes)"));
        return;
      }
      ntpSyncUdp.stop();

      //Identical parsing/validation to ezTime's own queryNTP() (standard NTPv3/v4 packet layout).
      uint32_t highWord, lowWord;
      highWord = (buffer[16] << 8 | buffer[17]) & 0x0000FFFF;
      lowWord = (buffer[18] << 8 | buffer[19]) & 0x0000FFFF;
      uint32_t reftsSec = highWord << 16 | lowWord;

      highWord = (buffer[32] << 8 | buffer[33]) & 0x0000FFFF;
      lowWord = (buffer[34] << 8 | buffer[35]) & 0x0000FFFF;
      uint32_t rcvtsSec = highWord << 16 | lowWord;

      highWord = (buffer[40] << 8 | buffer[41]) & 0x0000FFFF;
      lowWord = (buffer[42] << 8 | buffer[43]) & 0x0000FFFF;
      uint32_t secsSince1900 = highWord << 16 | lowWord;

      if (buffer[1] < 1 || buffer[1] > 15 || reftsSec == 0 || rcvtsSec == 0 || rcvtsSec > secsSince1900) {
        SerialPrintln(F("NTP sync: invalid response data"));
        ntpSyncState = NTP_SYNC_IDLE;
        ntpSyncNextDueAt = millis() + NTP_SYNC_RETRY_MS;
        return;
      }

      time_t rawUtc = secsSince1900 - 2208988800UL; //subtract 70 years: NTP epoch -> Unix epoch
      ntpSyncApplyResult(rawUtc);
      //Keep the optional DS3231 RTC drift-corrected and its Oscillator Stop Flag cleared - a
      //no-op if none is present.
      if (rtcPresent) {
        writeRtcTime(rawUtc);
      }
      ntpSyncEverSucceeded = true;
      SerialPrintln(F("NTP sync successful"));
      ntpSyncState = NTP_SYNC_IDLE;
      ntpSyncNextDueAt = millis() + NTP_SYNC_INTERVAL_MS;
      return;
    }
  }
}
