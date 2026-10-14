//Non-blocking replacement for ezTime's network calls (Timezone::setLocation() and the NTP sync of
//events()/updateNTP()): those do a blocking DNS + UDP round-trip that crashed the device via the
//software watchdog under network stress. Uses lwIP's callback-based dns_gethostbyname() and
//non-blocking WiFiUDP::parsePacket() instead. ezTime's own NTP sync is off (setInterval(0) in
//setup()); checkNtpSync() and checkTimezoneLookup() take over.
//
//Don't apply NTP time with a plain Timezone::setTime(utc) - see ntpSyncApplyResult().

#include <lwip/dns.h>

//===== Shared non-blocking DNS lookup, wrapping lwIP's own callback-based resolver =====

enum AsyncDnsState { ASYNC_DNS_IDLE, ASYNC_DNS_PENDING, ASYNC_DNS_DONE };

struct AsyncDnsLookup {
  AsyncDnsState state = ASYNC_DNS_IDLE;
  //Written by dnsFoundCallback() from lwIP's own call stack - volatile avoids stale cached reads.
  volatile bool callbackFired = false;
  volatile bool found = false;
  IPAddress resultIp;

  //Only call when idle (or after isDone()) - a pending lookup's callback would write into this struct.
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
//tzLookupResult/tzLookupDone are declared in ESPMaster.ino (see the comment there).

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
      //Only accept a reply from the IP/port the request went to (not authentication, just filters
      //unrelated traffic on this port); ignore anything else and keep waiting.
      if (tzLookupUdp.remoteIP() != tzDnsLookup.resultIp || tzLookupUdp.remotePort() != TIMEZONED_PORT) {
        SerialPrintln(F("Timezone lookup: ignoring UDP packet from an unexpected sender"));
        while (tzLookupUdp.available()) tzLookupUdp.read(); //discard it, keep the socket open for the real reply
        return;
      }

      String recv;
      recv.reserve(60);
      while (tzLookupUdp.available()) recv += (char)tzLookupUdp.read();
      tzLookupUdp.stop();

      //Same "OK <olson> <posix>"/"ERROR <msg>" protocol as ezTime's setLocation(); setPosix()
      //applies the result without another round-trip.
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

//Applies a raw NTP UTC timestamp so ezTime ends up in the same state as after its own updateNTP().
//Timezone::setTime(t) treats t as local time and stores t + getOffset(t)*60, with ezTime's
//POSIX-inverted offset sign - so setTime(rawUtc) would be off by the zone offset. Instead,
//pre-compensate with offsetGuess = getOffset(rawUtc, UTC_TIME) and call setTime(rawUtc -
//offsetGuess*60); setTime() recomputes the same offset and stores rawUtc. Checked below via
//getRealUtcNow().
void ntpSyncApplyResult(time_t rawUtc) {
  int16_t offsetGuess = timezone.getOffset(rawUtc, UTC_TIME);
  timezone.setTime(rawUtc - (long)offsetGuess * 60);

  long checkUtc = getRealUtcNow();
  long driftSeconds = checkUtc - (long)rawUtc;
  if (driftSeconds < -5 || driftSeconds > 5) {
    //Log loudly instead of silently trusting a possibly wrong clock.
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

      //Same sender check as the timezone lookup - the reply becomes the system time.
      if (ntpSyncUdp.remoteIP() != ntpDnsLookup.resultIp || ntpSyncUdp.remotePort() != 123) {
        SerialPrintln(F("NTP sync: ignoring UDP packet from an unexpected sender"));
        while (ntpSyncUdp.available()) ntpSyncUdp.read(); //discard it, keep the socket open for the real reply
        return;
      }

      byte buffer[NTP_SYNC_PACKET_SIZE]; //deliberately NOT pre-zeroed - see the length check below
      int bytesRead = ntpSyncUdp.read(buffer, NTP_SYNC_PACKET_SIZE);
      //Drop short datagrams: the fixed-offset reads below would otherwise parse uninitialized stack
      //memory, which could pass the plausibility checks.
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
      //Keep the optional DS3231 drift-corrected and its Oscillator Stop Flag cleared.
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
