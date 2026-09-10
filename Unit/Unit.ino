/*********
  Split Flap Arduino Nano Unit
*********/

//#define SERIAL_ENABLE // uncomment for serial debug communication
//#define TEST_ENABLE   // uncomment for Test mode. Rotates through a few character to make sure unit is working. These characters should be displayed in the correct order: " ", "Z", "A", "U", "N", "?", "0", "1", "2", "9"

#include <Arduino.h>
#include <Wire.h>
#include <Stepper.h>
#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <util/atomic.h>

//sentinel value sent over I2C (in place of a letter) to request a reboot into the twiboot
//bootloader for a firmware update - see Unit/BOOTLOADER_SETUP.md. Out of range of valid
//letter indices (0-44), so it can never collide with a normal display request.
#define CMD_ENTER_BOOTLOADER 255

//Address jumper block (J4 "AddressJumpers" on the fae-pcb schematic), 6 bits = 64 addresses. J4
//has 8 pin pairs; D5/D6 (topmost) stay unused - I2C addresses are 7 bits wide and twiboot adds a
//fixed 0x29 base (Unit/bootloader/twiboot/main.c, must stay in sync), so 6 bits (0-63, bootloader
//addresses 0x29-0x68) is the max safe extension.
//
//J4 top-to-bottom (verified against the PCB netlist): D6 (pin 1, nearest ISP header) - D5 - D4 -
//D3 - A3 - A2 - A1 - A0 (pin 15, bottom). Bit order: bottom-most = bit 0 - A0=bit0, A1=bit1,
//A2=bit2, A3=bit3, D3=bit4, D4=bit5. No jumper = address 0 (INPUT_PULLUP, getaddress() inverts each bit).
#define ADDRESS_PIN_COUNT 6
const uint8_t addressPins[ADDRESS_PIN_COUNT] = {A0, A1, A2, A3, 3, 4}; //bit 0..5, bottom-to-top on J4

//constants stepper
#define STEPPERPIN1 11
#define STEPPERPIN2 10
#define STEPPERPIN3 9
#define STEPPERPIN4 8
#define STEPS 2038 //28BYJ-48 stepper, number of steps
#define HALLPIN 7 //Pin of hall sensor
#define AMOUNTFLAPS 45

//Deceleration ramp tuning for stepLetters() below - slows the last letter-position(s) instead of
//hitting the target at full speed every time. 0 letters disables the ramp entirely.
#define RAMP_DOWN_LETTERS 1
#define RAMP_DOWN_MIN_FACTOR 0.7 //speed fraction (of stepperSpeed) reached on the final letter-position

//constants others
#define BAUDRATE 115200
//Valid stepperSpeed range - must stay in sync with ESPMaster.ino's MIN_SPEED/MAX_SPEED
//(convertSpeed() there clamps to this same range before ever sending). Used defensively here too
//(see receiveLetter()'s comment) - a garbled-but-checksum-valid speed byte shouldn't reach the
//motor unclamped, same reasoning as ESPMaster.ino's own comment on convertSpeed().
#define MIN_SPEED 1
#define MAX_SPEED 12
#define ROTATIONDIRECTION -1 //-1 for reverse direction
#define OVERHEATINGTIMEOUT 2 //timeout in seconds to avoid overheating of stepper. After starting rotation, the counter will start. Stepper won't move again until timeout is passed
unsigned long lastRotation = 0;

#ifdef SERIAL_ENABLE
//Serial input stuff
const byte numChars = 32;
char receivedChars[numChars]; // an array to store the received data
boolean newData = false;
#endif

//globals
//Wire's onReceive()/onRequest() callbacks (receiveLetter()/requestEvent() below) run directly
//inside the AVR TWI interrupt handler, with global interrupts disabled for their own duration -
//so a variable written or read by BOTH one of those callbacks and "normal" code (setup()/loop()/
//everything they call) needs `volatile` (found 2026-09-09, CODE_REVIEW.md #30): without it, the
//compiler is free to cache a copy of the variable in a register across normal-code statements,
//since it can't see the ISR-side write - normal code could then keep observing a stale value
//indefinitely, an optimization-dependent bug that wouldn't show up in every build. `volatile`
//alone doesn't make a multi-byte (int/uint16_t) access atomic though - normal code CAN be
//interrupted mid-access by the ISR, unlike the ISR interrupting itself - see the ATOMIC_BLOCK
//uses below at the specific multi-byte cross-context accesses where a torn value would actually
//matter physically (how far the motor turns, whether a rotation is issued at all). The frequent,
//self-correcting-next-poll ones (currentPhaseProgress, currentlyrotating, currentPhase - a stuck-
//detection heartbeat and busy flags, not motor-movement or rotation-dispatch decisions) are left
//as plain volatile without extra wrapping, deliberately - see BOOTLOADER_SETUP.md-style reasoning:
//the risk here is real but narrow and non-destructive, not worth the added per-step overhead/risk
//of wrapping the hot stepLetters()/calibrate() progress-counter loops too.
volatile int displayedLetter = 0; //currently shown letter
//Tracks whether displayedLetter is an actually-confirmed position (a calibrate() call reached the
//marker) rather than just calibrate()'s own "give up, reset to 0" fallback on failure - found
//2026-09-09, CODE_REVIEW_V2.md #5: previously a failed calibrate(false) inside rotateToLetter()
//correctly aborted the CURRENT call (CODE_REVIEW.md #18's fix), but left displayedLetter at 0 with
//nothing distinguishing "genuinely homed to 0" from "unknown, calibration never confirmed it" - the
//NEXT call then trusted that 0 as real and could take the direct-move branch from a completely
//wrong actual position. Set by calibrate() itself (true on success, false on its failure path) so
//every call site is covered without needing to touch each one's own handling of the return value.
volatile bool positionKnown = false; //starts false: no calibration has run yet
int desiredLetter = 0; //letter to be shown
const char letters[] = {' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'Ä', 'Ö', 'Ü', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', '-', '?', '!'};
Stepper stepper(STEPS, STEPPERPIN1, STEPPERPIN3, STEPPERPIN2, STEPPERPIN4); //stepper setup
bool lastInd1 = false; //store last status of phase
bool lastInd2 = false; //store last status of phase
bool lastInd3 = false; //store last status of phase
bool lastInd4 = false; //store last status of phase
float missedSteps = 0; //cummulate steps <1, to compensate via additional step when reaching >1
volatile int currentlyrotating = 0; // 1 = drum is currently rotating, 0 = drum is standing still

//Extra I2C status byte so ESPMaster can tell a slow calibration search from an actually stuck unit.
#define PHASE_IDLE        0
#define PHASE_CALIBRATING 1
#define PHASE_ROTATING    2
volatile uint8_t currentPhase = PHASE_IDLE;
volatile uint16_t currentPhaseProgress = 0; //steps done so far in the current phase - lets a watcher tell "still making progress" from "stuck"
volatile int stepperSpeed = 10; //current speed of stepper, value only for first homing
int eeAddress = 0;   //EEPROM address for calibration offset
volatile uint16_t calOffset;       //Offset for calibration in steps, stored in EEPROM, gets read in setup
volatile int receivedNumber = 0;
int i2cAddress;
volatile bool forceRecalibrate = false;   //new offset received via I2C, recalibrate immediately in loop()
volatile bool pendingEepromWrite = false; //new offset received via I2C, EEPROM.put() must happen in loop(), not the ISR
volatile bool enterBootloaderRequested = false; //CMD_ENTER_BOOTLOADER received, triggers a watchdog reset in loop()

//sleep globals
const unsigned long WAIT_TIME = 2000;    //wait time before sleep routine gets executed again in milliseconds
unsigned long previousMillis = 0;       //stores last time sleep was interrupted

//test calibration settings
#if defined(SERIAL_ENABLE) || defined(TEST_ENABLE)
int calLetters[10] = {0, 26, 1, 21, 14, 43, 30, 31, 32, 39};
//int calLetters[10] = {0, 6, 1, 5, 0};

void run_test() {
  stepperSpeed = 12;
  for (int i = 0; i < 10; i++) {
    int currentCalLetter = calLetters[i];
    rotateToLetter(currentCalLetter);
    delay(2000);
  }
}
#endif

//setup
void setup() {
  // i2c adress switch
  for (int i = 0; i < ADDRESS_PIN_COUNT; i++) {
    pinMode(addressPins[i], INPUT_PULLUP);
  }

  //Hall sensor: INPUT_PULLUP rather than plain INPUT - if this is an open-collector/open-drain
  //sensor without its own pull-up, a floating pin gives noisy readings and calibrate() can miss
  //the marker indefinitely. Revert if this makes homing worse (would suggest a push-pull sensor).
  pinMode(HALLPIN, INPUT_PULLUP);

  i2cAddress = getaddress(); //get I2C Address and save in variable

#ifdef SERIAL_ENABLE
  //initialize serial
  Serial.begin(BAUDRATE);
  Serial.println("starting unit");
  Serial.print("I2CAddress: ");
  Serial.println(i2cAddress);
#endif

  //I2C function assignment
  Wire.begin(i2cAddress); //i2c address of this unit
  Wire.onReceive(receiveLetter);//call-function for transfered letter via i2c
  Wire.onRequest(requestEvent); //call-funtion if master requests unit state

  getOffset();     //get calibration offset from EEPROM
  calibrate(true); //home stepper after startup

#ifdef TEST_ENABLE
  // Run test on startup
  run_test();
#endif
}

void loop() {
  //if a firmware update was requested via I2C, cleanly stop the motor and reboot into
  //the twiboot bootloader (see Unit/BOOTLOADER_SETUP.md). twiboot itself disables the
  //watchdog again immediately on entry, so this can't trap the unit in a reset loop.
  if (enterBootloaderRequested) {
    enterBootloaderRequested = false;
    stopMotor();
    wdt_enable(WDTO_15MS);
    while (1) {} //wait for the watchdog to fire
  }

  //if a new calibration offset arrived via I2C, write it to EEPROM here (not in receiveLetter()/
  //the ISR - see the comment there) before recalibrating.
  if (pendingEepromWrite) {
    pendingEepromWrite = false;
    //calOffset is 2 bytes - snapshot it atomically so a TWI interrupt firing mid-read (updating
    //calOffset again from a second, near-simultaneous I2C write) can't hand writeToEEPROM() a
    //torn, half-old/half-new value (see the volatile-block comment near calOffset's declaration).
    uint16_t offsetSnapshot;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      offsetSnapshot = calOffset;
    }
    writeToEEPROM(offsetSnapshot);
  }

  //if a new calibration offset arrived via I2C, recalibrate immediately so the change is visible right away
  if (forceRecalibrate) {
    forceRecalibrate = false;
    calibrate(true);
  }

  unsigned long currentMillis = millis();
  //Sleep block disabled (if (false && ...)) - suspected contributor to the intermittent I2C-read-
  //hang bug: an ESP read landing as the unit enters sleep can badly delay/miss the ACK. Re-enable
  //only once that's ruled out - exists for power savings, not correctness.
  if (false && currentMillis - previousMillis >= WAIT_TIME) {
    byte old_ADCSRA = ADCSRA;
    // disable ADC
    ADCSRA = 0;
#ifdef SERIAL_ENABLE
    set_sleep_mode (SLEEP_MODE_IDLE);
#else
    set_sleep_mode (SLEEP_MODE_PWR_DOWN);
#endif
    sleep_enable();
#ifndef SERIAL_ENABLE
    digitalWrite (LED_BUILTIN, LOW); // shuts off LED when starting to sleep, for debugging
#endif
    sleep_cpu ();
#ifndef SERIAL_ENABLE
    digitalWrite (LED_BUILTIN, HIGH); // turns on LED when waking up, for debugging
#endif
    sleep_disable();
    previousMillis = currentMillis; //reset sleep counter
    ADCSRA = old_ADCSRA;

    // release TWI bus
    TWCR = bit(TWEN) | bit(TWIE) | bit(TWEA) | bit(TWINT);

    // turn it back on again
    Wire.begin (i2cAddress);
  }  // end of time to sleep

  //check if new letter was received through i2c
  //Both are 2-byte ints, each independently written from the other context (receivedNumber only
  //by the ISR, displayedLetter only by rotateToLetter() below) - snapshot both atomically so this
  //comparison (and the value handed to rotateToLetter()) can't see one of them mid-update, torn
  //between its old and new byte (see the volatile-block comment near their declarations).
  int receivedNumberSnapshot;
  int displayedLetterSnapshot;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    receivedNumberSnapshot = receivedNumber;
    displayedLetterSnapshot = displayedLetter;
  }
  //Also re-attempt whenever positionKnown is false, even if the numbers already coincide - found
  //2026-09-10, CODE_REVIEW_V3.md #4: this comparison alone decided whether to call
  //rotateToLetter() at all, completely independent of positionKnown (see that variable's own
  //declaration comment) - if a failed calibration happened to leave displayedLetter equal to
  //whatever receivedNumber already was (e.g. both 0/space), this dispatch never called
  //rotateToLetter() again at all, so positionKnown staying false had no effect: the gate it
  //controls lives INSIDE a function that was never even invoked. rotateToLetter() itself already
  //handles this correctly once actually called - positionKnown==false forces its full-rotation/
  //recalibration branch regardless of the numeric relationship between the two positions.
  if (displayedLetterSnapshot != receivedNumberSnapshot || !positionKnown)
  {
    /*
      #ifdef SERIAL_ENABLE
      Serial.print("Value over serial received: ");
      Serial.print(receivedNumber);
      Serial.print(" Letter: ");
      Serial.print(letters[receivedNumber]);
      Serial.println();
      #endif
    */
    //rotate to new letter
    rotateToLetter(receivedNumberSnapshot);
  }

#ifdef SERIAL_ENABLE
  // check if we got calibration data via serial
  recvWithEndMarker();
#endif
}

int translateLettertoInt(char letterchar) {
  // automatically convert lower case to upper case
  if ('a' <= letterchar && letterchar <= 'z') {
    letterchar -= 32;
  }
  for (int flapIndex = 0; flapIndex < sizeof(letters); flapIndex++) {
    if (letterchar == letters[flapIndex]) {
      return flapIndex;
    }
  }

  return -1;
}

//Moves the drum forward by `letterSteps` letter-positions, decelerating over the final
//RAMP_DOWN_LETTERS positions instead of hitting the target at full stepperSpeed. Shared by both
//rotateToLetter() branches (direct move, and the post-calibration full-rotation move).
void stepLetters(int letterSteps) {
  stepper.setSpeed(stepperSpeed);

  for (int i = 0; i < letterSteps; i++) {
    currentPhaseProgress = (uint16_t)i;

    int stepsRemaining = letterSteps - i;
    if (RAMP_DOWN_LETTERS > 0 && stepsRemaining <= RAMP_DOWN_LETTERS) {
      float rampFraction = (float)stepsRemaining / (float)(RAMP_DOWN_LETTERS + 1);
      float speedFactor = RAMP_DOWN_MIN_FACTOR + (1.0 - RAMP_DOWN_MIN_FACTOR) * rampFraction;
      int rampedSpeed = (int)(stepperSpeed * speedFactor);
      stepper.setSpeed(rampedSpeed > 0 ? rampedSpeed : 1); //never 0, Stepper::step() would spin forever
    }

    float preciseStep = (float)STEPS / (float)AMOUNTFLAPS;
    int roundedStep = (int)preciseStep;
    missedSteps = missedSteps + ((float)preciseStep - (float)roundedStep);
    if (missedSteps > 1) {
      roundedStep = roundedStep + 1;
      missedSteps--;
    }
    stepper.step(ROTATIONDIRECTION * roundedStep);
  }
}

//rotate to letter
void rotateToLetter(int toLetter) {
  if (lastRotation == 0 || (millis() - lastRotation > OVERHEATINGTIMEOUT * 1000)) {
    lastRotation = millis();
    //get letter position
    int posLetter = -1;
    posLetter = toLetter;
    int posCurrentLetter = -1;
    posCurrentLetter = displayedLetter;
    //int amountLetters = sizeof(letters) / sizeof(String);
#ifdef SERIAL_ENABLE
    Serial.print(F("go to letter: "));
    Serial.println(letters[toLetter]);
#endif
    //go to letter, but only if available (>-1)
    if (posLetter > -1) { //check if letter exists
      //check if letter is on higher index, then no full rotaion is needed - but only if
      //posCurrentLetter is actually a confirmed position (see positionKnown's comment above): an
      //unconfirmed one (last calibration failed) must always force the full-rotation/calibrate()
      //branch below, regardless of the index comparison, since a "direct" move counts steps
      //relative to a position that's known to already be wrong.
      if (positionKnown && posLetter >= posCurrentLetter) {
#ifdef SERIAL_ENABLE
        Serial.println("direct");
#endif
        //go directly to next letter, get steps from current letter to target letter
        int diffPosition = posLetter - posCurrentLetter;
        startMotor();
        stepLetters(diffPosition);
      }
      else {
        //full rotation is needed, good time for a calibration
#ifdef SERIAL_ENABLE
        Serial.println(F("full rotation incl. calibration"));
#endif
        //calibrate()'s return value was previously discarded entirely (found 2026-09-09,
        //CODE_REVIEW.md #18): on failure (-1, Hall sensor/marker never found within 3*STEPS) it
        //already stops the motor and resets displayedLetter to 0 as an honest "actual position
        //unknown" - but this function then went on to step further FROM that unverified position
        //and unconditionally claimed displayedLetter=toLetter anyway, as if the rotation had
        //genuinely succeeded. Abort instead: leave displayedLetter at calibrate()'s own reset
        //value (0) rather than lying about having reached toLetter. loop()'s existing
        //`displayedLetter != receivedNumber` retry naturally tries again on the next call (gated
        //by the same OVERHEATINGTIMEOUT cooldown as every other rotation attempt, so this can't
        //spin tightly) - a lightweight, self-retrying "error state" rather than a new persistent
        //lockout that would need its own explicit clear/reporting mechanism.
        if (calibrate(false) == -1) {
          return;
        }
        //startMotor();
        currentPhase = PHASE_ROTATING; //calibrate() left it at PHASE_CALIBRATING, we're rotating to the target letter now
        stepLetters(posLetter);
      }
      //store new position
      displayedLetter = toLetter;
      //rotation is done, stop the motor
      delay(100); //important to stop rotation before shutting of the motor to avoid rotation after switching off current
      stopMotor();
    }
    else {
#ifdef SERIAL_ENABLE
      Serial.println("letter unknown, go to space");
#endif
      desiredLetter = 0;
    }
  }
}

//Every message ends with an XOR-checksum byte (mirrors i2cChecksum() in ESPMaster). A mismatch
//(likely I2C bus noise) is ignored entirely.
void receiveLetter(int numBytes) {
  int receiveArray[5]; //array for received bytes - grew from 4 to 5 for the checksum byte
  uint16_t newCalOffset = 0;

  int actualBytes = numBytes;
  if (actualBytes > 5) {
    actualBytes = 5; //shouldn't happen (ESPMaster never sends more than 5 bytes), but never overflow
  }

  for (int i = 0; i < actualBytes; i++) {
    receiveArray[i] = Wire.read();
  }
  //Drain anything beyond our buffer so it doesn't linger in the TWI hardware's own queue
  for (int i = actualBytes; i < numBytes; i++) {
    Wire.read();
  }

  //Every real message from ESPMaster carries at least 2 payload bytes (letter + speed) plus the
  //checksum. Previously only actualBytes<2 (payloadLen<1) was rejected here, which let a bare
  //{letter, checksum} 2-byte packet through with payloadLen==1 - receiveArray[1] (the checksum
  //byte itself) then got silently read as the speed a few lines below (found 2026-09-09,
  //CODE_REVIEW.md #20, reproduced with such a packet).
  if (actualBytes < 3) {
    return; //too short to contain a real letter+speed payload + checksum - ignore
  }

  int payloadLen = actualBytes - 1; //last received byte is always the checksum
  uint8_t computedChecksum = 0;
  for (int i = 0; i < payloadLen; i++) {
    computedChecksum ^= (uint8_t)receiveArray[i];
  }
  if (computedChecksum != (uint8_t)receiveArray[payloadLen]) {
    return; //checksum mismatch - see the comment above this function
  }

  //firmware update request - don't treat as a letter, just raise the flag and bail out
  if (receiveArray[0] == CMD_ENTER_BOOTLOADER) {
    enterBootloaderRequested = true;
    return;
  }

  //A matching checksum only proves the bytes weren't corrupted in transit, not that they're a
  //*plausible* letter position - a garbled-but-checksum-valid value outside the 45 real positions
  //(1-in-256 chance per corrupted byte) was previously accepted and driven to as-is (found
  //2026-09-09, CODE_REVIEW.md #20, reproduced with e.g. letter=100). Reject it the same way
  //ESPMaster.ino's own translateLettertoInt() already only ever returns 0..44 (or -1).
  if (receiveArray[0] < 0 || receiveArray[0] >= AMOUNTFLAPS) {
    return;
  }

  //Write received bytes to correct variables
  receivedNumber = receiveArray[0];
  //Defensive floor AND ceiling: stepperSpeed=0 makes Stepper::setSpeed(0) divide by zero, spinning
  //Stepper::step() forever waiting for a delay that never elapses (needs a power cycle to
  //recover); an implausibly high speed risks missed steps/motor strain, the same concern
  //ESPMaster.ino's own convertSpeed() comment already raises - constrain() to the same
  //MIN_SPEED..MAX_SPEED range used there, independent of what the master actually sends.
  stepperSpeed = constrain(receiveArray[1], MIN_SPEED, MAX_SPEED);
  if (payloadLen >= 4) {
    newCalOffset = receiveArray[2] + (receiveArray[3] << 8);
    //Same range ESPMaster.ino's own /api/units?action=offset already validates against
    //(0..STEPS-1) - without this, a garbled-but-checksum-valid offset could set calOffset far
    //outside a real rotation's worth of steps, the same failure mode fixed in getOffset() above.
    if (newCalOffset < STEPS && newCalOffset != calOffset) {
      calOffset = newCalOffset;
      //Defer the actual EEPROM.put() to loop() - receiveLetter() runs inside the AVR's TWI
      //interrupt handler with global interrupts disabled, and EEPROM.put() blocks for several ms.
      pendingEepromWrite = true;
      forceRecalibrate = true; //apply the new offset right away instead of waiting for the next full rotation
    }
  }
}

void requestEvent() {
  //send unit status and current calibration offset to master
  //Report busy if EITHER actually rotating right now (currentlyrotating), OR a newly-received
  //letter is still waiting to be applied - e.g. during the OVERHEATINGTIMEOUT cooldown in
  //rotateToLetter(), which returns immediately without moving anything or updating
  //displayedLetter whenever it's called again too soon after the last rotation. Previously only
  //currentlyrotating was checked, which stays 0 for the whole cooldown wait - the master's "is
  //this unit ready" poll (pollDisplayJobWaitAllUnitsReady() in ESPMaster.ino) considered the unit
  //already done before it had even started moving toward the newly-received letter, so a job
  //could complete (and a subsequent write overwrite receivedNumber) before the pending rotation
  //ever actually happened. Found 2026-09-09, CODE_REVIEW.md #16.
  //Also busy while positionKnown is false - found 2026-09-10, CODE_REVIEW_V3.md #4: this report
  //ignored positionKnown entirely, same gap as the loop() dispatch condition above it. Without
  //this, a unit whose last calibration failed but whose displayedLetter (reset to 0 by that
  //failure) happened to already equal receivedNumber would report itself READY despite its actual
  //position being unconfirmed - letting the master consider the display job successfully done.
  uint8_t effectivelyBusy = (currentlyrotating != 0 || displayedLetter != receivedNumber || !positionKnown) ? 1 : 0;
  Wire.write(effectivelyBusy);
  Wire.write(lowByte(calOffset));
  Wire.write(highByte(calOffset));
  Wire.write(currentPhase);
  Wire.write(lowByte(currentPhaseProgress));
  Wire.write(highByte(currentPhaseProgress));
  /*
    #ifdef SERIAL_ENABLE
    Serial.print("Status ");
    Serial.print(currentlyrotating);
    Serial.print(" sent to master");
    Serial.println();
    #endif
  */
}

//returns the address of the unit as int from 0-63 (see the comment on addressPins near the top
//of this file for the pin-to-bit mapping and why 6 bits, not the full 8 pins J4 has)
int getaddress() {
  int address = 0;
  for (int i = 0; i < ADDRESS_PIN_COUNT; i++) {
    if (!digitalRead(addressPins[i])) {
      address += (1 << i);
    }
  }
  return address;
}

//gets magnet sensor offset from EEPROM in steps
void getOffset() {
  EEPROM.get(eeAddress, calOffset);
  //calOffset is uint16_t - "< 0" can never be true, this check never did anything (found
  //2026-09-09, CODE_REVIEW.md #17). A blank/erased EEPROM (0xFFFF = 65535, e.g. after a fresh ISP
  //chip-erase) needs an upper-bound check instead: without one, calibrate()'s
  //stepper.step(ROTATIONDIRECTION * calOffset) runs with a bogus value far larger than one
  //rotation's worth of steps - live-reproduced as an unintended ~3-minute spin. Same valid range
  //ESPMaster.ino's own /api/units?action=offset already validates against (0..STEPS-1).
  if (calOffset >= STEPS) {
    calOffset = 0;
  }
#ifdef SERIAL_ENABLE
  Serial.print(F("CalOffset from EEPROM: "));
  Serial.print(calOffset);
  Serial.println();
#endif
}

//doing a calibration of the revolver using the hall sensor
int calibrate(bool initialCalibration) {
#ifdef SERIAL_ENABLE
  Serial.println(F("calibrate revolver"));
#endif
  currentlyrotating = 1; //set active state to active
  currentPhase = PHASE_CALIBRATING; //see the comment near currentPhase's declaration
  bool reachedMarker = false;
  stepper.setSpeed(stepperSpeed);
  int i = 0;
  //Two-phase edge search (found 2026-09-09, CODE_REVIEW_V2.md #6, following on from
  //CODE_REVIEW.md #19's i==0 special case below): that earlier fix only handled the drum already
  //sitting inside the marker zone at the very START of calibration - it still accepted ANY LOW
  //reading as "found" once past that one special case, without ever requiring a genuine
  //HIGH-to-LOW entry edge. A permanently-LOW/stuck sensor, or a marker zone wider than the blind
  //50-step jump below, was therefore accepted as a successful calibration (reproduced: sensor
  //stuck LOW, offset 0 -> reports success after just 50 steps, having never actually seen HIGH).
  //leavingZone tracks "we're still inside the zone we started in (or moved into) and haven't yet
  //observed a genuine HIGH reading" - only once HIGH has actually been seen does the final `else`
  //below get to declare the NEXT LOW a real, repeatable edge. If the sensor never goes HIGH at
  //all, leavingZone simply never clears and the existing i > 3*STEPS cap below times this out as
  //a failure, exactly like it already does for a sensor stuck HIGH.
  bool leavingZone = (digitalRead(HALLPIN) == 0);
  while (!reachedMarker) {
    currentPhaseProgress = (uint16_t)i;
    int currentHallValue = digitalRead(HALLPIN);
    if (leavingZone) {
      if (currentHallValue == 1) {
        leavingZone = false; //genuinely left the zone - now search for the real entry edge below
      } else {
        //not reached yet
        stepper.step(ROTATIONDIRECTION * 1);
      }
    }
    else if (currentHallValue == 1) {
      //not reached yet
      stepper.step(ROTATIONDIRECTION * 1);
    }
    else {
      //reached marker (a confirmed HIGH-to-LOW edge, not just any LOW reading), go to calibrated
      //offset position
      reachedMarker = true;
      //Snapshot atomically before use - a torn calOffset here would turn the motor by a wrong,
      //momentarily-garbled step count instead of just reporting a stale status value (see the
      //volatile-block comment near calOffset's declaration).
      uint16_t offsetSnapshot;
      ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        offsetSnapshot = calOffset;
      }
      stepper.step(ROTATIONDIRECTION * offsetSnapshot);
      displayedLetter = 0;
      positionKnown = true; //see positionKnown's declaration comment - this 0 is now confirmed real
      missedSteps = 0;
#ifdef SERIAL_ENABLE
      Serial.println(F("revolver calibrated"));
#endif
      //Only stop motor for initial calibration
      if (initialCalibration) {
        stopMotor();
      }
      return i;
    }
    if (i > 3 * STEPS) {
      //seems that there is a problem with the marker or the sensor. turn of the motor to avoid overheating.
      displayedLetter = 0;
      positionKnown = false; //see positionKnown's declaration comment - this 0 is NOT confirmed
      desiredLetter = 0;
      reachedMarker = true;
#ifdef SERIAL_ENABLE
      Serial.println(F("calibration revolver failed"));
#endif
      stopMotor();
      return -1;
    }
    i++;
  }
  return i;
}

//switching off the motor driver
void stopMotor() {
  lastInd1 = digitalRead(STEPPERPIN1);
  lastInd2 = digitalRead(STEPPERPIN2);
  lastInd3 = digitalRead(STEPPERPIN3);
  lastInd4 = digitalRead(STEPPERPIN4);

  digitalWrite(STEPPERPIN1, LOW);
  digitalWrite(STEPPERPIN2, LOW);
  digitalWrite(STEPPERPIN3, LOW);
  digitalWrite(STEPPERPIN4, LOW);
#ifdef SERIAL_ENABLE
  Serial.println(F("Motor Stop"));
#endif
  currentlyrotating = 0; //set active state to not active
  currentPhase = PHASE_IDLE; //see the comment near currentPhase's declaration
  currentPhaseProgress = 0;
  delay(100);
}

void startMotor() {
  currentPhase = PHASE_ROTATING; //see the comment near currentPhase's declaration
#ifdef SERIAL_ENABLE
  Serial.println(F("Motor Start"));
#endif
  currentlyrotating = 1; //set active state to active
  digitalWrite(STEPPERPIN1, lastInd1);
  digitalWrite(STEPPERPIN2, lastInd2);
  digitalWrite(STEPPERPIN3, lastInd3);
  digitalWrite(STEPPERPIN4, lastInd4);
}


void writeToEEPROM(uint16_t offsetValue) {
  //One simple call, with the address first and the object second.
  EEPROM.put(eeAddress, offsetValue);

#ifdef SERIAL_ENABLE
  Serial.print(F("Value written to EEPROM: "));
  Serial.print(calOffset);
  Serial.println();
#endif
}

#ifdef SERIAL_ENABLE
void recvWithEndMarker() {
  static byte ndx = 0;
  char endMarker = '\n';
  char rc;

  while (Serial.available() > 0 && newData == false) {
    rc = Serial.read();

    if (rc != endMarker) {
      receivedChars[ndx] = rc;
      ndx++;
      if (ndx >= numChars) {
        ndx = numChars - 1;
      }
    }
    else {
      receivedChars[ndx] = '\0'; // terminate the string
      ndx = 0;
      newData = true;
    }
  }

  if (newData == true) {
    newData = false;
    if (receivedChars[0] != 0x00 && receivedChars[1] == 0x00) {
      auto new_letter = translateLettertoInt(receivedChars[0]);
      if (new_letter == -1) {
        Serial.println(F("Letter not found in index"));
      } else {
        stepperSpeed = 12;
        receivedNumber = new_letter;
      }
    } else if (strncmp(receivedChars, "test", numChars) == 0) {
      Serial.println(F("Starting calibration test"));
      calibrate(true);
      run_test();
    } else {
      // convert to int
      calOffset = String(receivedChars).toInt();
      // check if the integer conversion failed
      if (calOffset == 0 && receivedChars[0] != '0') {
        Serial.println(F("Invalid Command"));
      } else {
        // save to eeprom
        writeToEEPROM(calOffset);
        calibrate(true);
      }
    }
  }
}
#endif
