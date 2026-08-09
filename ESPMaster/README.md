# ESPMaster Scott Version - Modifications

This is a modified version of the ESPMaster code with improvements for better web server responsiveness and debugging capabilities.

## Major Changes from the Base v2.3.0 Release

- Added LED-driven setup/debug indicators that map each initialization stage to a blink pattern for quick diagnosis in the field.
- Replaced blocking calls (notably `waitForSync()` and `delay()`) with non-blocking loops that `yield()` so the HTTP server stays responsive.
- Introduced per-phase timeouts (WiFi, NTP sync, flap updates, filesystem waits) to prevent indefinite hangs.
- Expanded debug output, including a status string that traces the current setup phase over serial and mirroring of those messages to the web UI.
- Added a WiFi strength indicator (RSSI + qualitative label) on the main interface so you can spot reception issues without attaching a serial cable.
- Hardened `ServiceFlapFunctions` with cooperative timing so lengthy display updates do not starve the rest of the system.

## Key Changes

### 1. LED Debug Codes
Added LED blink codes on GPIO 2 (configurable) to indicate system status:
- **1 blink** = WiFi connecting
- **2 blinks** = WiFi connected
- **3 blinks** = NTP syncing
- **4 blinks** = NTP sync success
- **5 blinks** = NTP sync failed
- **6 blinks** = File system init
- **7 blinks** = Web server starting
- **8 blinks** = Web server ready
- **Continuous blink** = Error/hang detected

**ESP-01 vs ESP-01S notes**
- `ESP-01` (original): the onboard blue LED is tied to GPIO 1 (TX), so leave `LED_DEBUG_PIN` on GPIO 2 and drive an external LED when using that module.
- `ESP-01S`: the blue LED is factory-wired to GPIO 2 (not TX/RX), is active LOW, and works great for the blink codes out of the box—no extra wiring needed. Keeping the default `LED_DEBUG_PIN 2` setting drives that LED while the UART pins stay dedicated to the flap-chain traffic.

### 2. Non-Blocking NTP Sync
- Replaced blocking `waitForSync()` with a non-blocking timeout-based approach
- 30-second timeout prevents indefinite hanging
- Calls `yield()` during sync to allow web server to process requests
- System continues even if NTP sync fails

### 3. Web Server Responsiveness Improvements
- Added `yield()` calls in all blocking `while` loops in `ServiceFlapFunctions.ino`
- Replaced blocking `delay()` calls with non-blocking timing using `millis()`
- Added 30-second timeouts to prevent infinite hangs when waiting for display
- Web server can now process requests even while display is updating

### 4. Enhanced Debugging
- Added `DEBUG:` status messages throughout setup
- Debug status string tracks current initialization phase
- More verbose logging for troubleshooting
- **Debug Mode** (`DEBUG_ENABLE`): When enabled, provides comprehensive debugging features:
  - **Startup Debug Page**: Shows initialization log during boot (accessible before main UI loads)
  - **Error Status Panel**: Displays JavaScript errors, network errors, and page load issues at the top of the main page
  - **Page Load Debug Log**: Shows debug messages during page initialization (collapsible viewer)
  - **Serial Debug Log**: Real-time serial output at the bottom of the main page with hide/show toggle
- All debug features can be enabled/disabled via `/enable-debug-mode` and `/exit-debug-mode` endpoints

### 5. WiFi Strength Indicator
- `getCurrentSettingValues()` now samples RSSI multiple times and returns both the raw dBm value and connection state.
- The web UI renders this as a color-coded "Excellent/Very Good/…/Very Weak" status line next to the WiFi details.
- Use this to diagnose poor placement or antenna issues; values worse than about -80 dBm usually cause missed OTA packets and unreliable message pushes.

## Hardware Requirements

- **LED Debug Pin**: GPIO 2 (default, can be changed in code)
- Connect LED between GPIO 2 and GND (with appropriate current-limiting resistor if needed)
- Note: GPIO 2 is also used for I2C on some ESP01 modules - adjust if there's a conflict

## Configuration

To change the LED pin, modify these lines in `ESPMaster.ino`:
```cpp
#define LED_DEBUG_PIN 2
#define LED_ON LOW   // ESP01 LED is typically active LOW
#define LED_OFF HIGH
```

## Version

- **Version**: 2.3.0-Scott
- **Based on**: split-flap-scientress ESPMaster v2.3.0

## Files Modified

1. **ESPMaster.ino**
   - Added LED control functions
   - Non-blocking NTP sync with timeout
   - LED blink codes throughout setup
   - Enhanced debug logging

2. **ServiceFlapFunctions.ino**
   - Added `yield()` calls in all blocking loops
   - Non-blocking delays with `yield()`
   - Timeout protection for display wait loops

## Benefits

- **Web interface remains responsive** even when display is updating
- **Visual debugging** via LED blink codes
- **No more indefinite hangs** from NTP sync or display waits
- **Better error detection** through LED patterns and debug messages

## Troubleshooting

If the web page still doesn't load:
1. Check LED blink pattern to see where initialization stops
2. Enable `SERIAL_ENABLE` to see debug messages
3. Check if NTP sync is failing (5 blinks = failed)
4. Verify WiFi connection (2 blinks = connected)

If LED doesn't work:
- Check GPIO 2 pin assignment
- Verify LED polarity (LED_ON = LOW for ESP01)
- Ensure LED has proper current-limiting resistor

