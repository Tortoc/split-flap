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

//This project's own semver for Unit.ino - bump on every functionally meaningful change (not pure
//comment/formatting cleanup). Reported to ESPMaster via requestEvent()'s 3 trailing bytes, added
//below - see ESPMaster.ino's ANSWER_SIZE_VERSION/readUnitInfo() comments for why a unit running
//older firmware without these bytes reads back as 255/255/255 ("unknown"), not a crash.
#define UNIT_FW_VERSION_MAJOR 2
#define UNIT_FW_VERSION_MINOR 2
#define UNIT_FW_VERSION_PATCH 3

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

//Acceleration ramp tuning for stepLetters() below - mirrors RAMP_DOWN_* above but for the START of
//a move instead of the end. A slower start needs less initial current/torque than jumping straight
//to full stepperSpeed - plausible (not proven) as one contributor to the still-open "Unit dreht
//langsam" investigation (voltage-sag hypothesis, see unit-slow-rotation-and-remote-reset memory).
//0.6 is a slightly more conservative starting factor than RAMP_DOWN_MIN_FACTOR's 0.7 - starting
//from a dead stop needs more torque than coasting to one.
#define RAMP_UP_LETTERS 1
#define RAMP_UP_MIN_FACTOR 0.6 //speed fraction (of stepperSpeed) used on the first letter-position

//Diagnostic-only threshold (found successful, but took unusually long) - see
//unexpectedHomeTriggerCount/slowHomeSearchCount's own comments below for the full story. From any
//starting position the marker should normally be found within about one revolution; this is a
//deliberately generous multiple of that, well under the existing 3*STEPS hard-failure cap in
//calibrate(), meant to catch "still working, but struggling" before it becomes an outright failure.
//Starting value, not a measured one - see the plan's own "Unsicherheiten" section.
#define EXPECTED_HOME_SEARCH_MAX_STEPS (STEPS + STEPS / 2)

//Chunk size for calibrate()'s post-marker offset move - see that call site's own comment. 20 steps
//keeps every chunk well under a second even at the slowest allowed speed (1 RPM, ~29ms/step ->
//~0.6s/chunk), so currentPhaseProgress updates far more often than the master's 15s
//stuck-detection window, while still being coarse enough not to add meaningful per-chunk call
//overhead over the previous single stepper.step() call.
#define CALIBRATION_OFFSET_STEP_CHUNK 20

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
//everything they call) needs `volatile`: without it, the compiler is free to cache a copy of the
//variable in a register across normal-code statements, since it can't see the ISR-side write.
//`volatile` alone doesn't make a multi-byte (int/uint16_t) access atomic though - normal code CAN
//be interrupted mid-access by the ISR - see the ATOMIC_BLOCK uses below at the specific multi-byte
//cross-context accesses where a torn value would actually matter physically (how far the motor
//turns, whether a rotation is issued at all). The frequent, self-correcting-next-poll ones
//(currentPhaseProgress, currentlyrotating, currentPhase - a stuck-detection heartbeat and busy
//flags, not motor-movement or rotation-dispatch decisions) are left as plain volatile without
//extra wrapping - the risk here is real but narrow and non-destructive, not worth the added
//per-step overhead of wrapping the hot stepLetters()/calibrate() progress-counter loops too.
volatile int displayedLetter = 0; //currently shown letter
//Tracks whether displayedLetter is an actually-confirmed position (a calibrate() call reached the
//marker) rather than just calibrate()'s own "give up, reset to 0" fallback on failure - without
//this, a failed calibrate(false) inside rotateToLetter() leaves displayedLetter at 0 with nothing
//distinguishing "genuinely homed to 0" from "unknown, calibration never confirmed it", so the NEXT
//call could trust that 0 and take the direct-move branch from a completely wrong actual position.
//Set by calibrate() itself (true on success, false on failure) so every call site is covered.
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

//Diagnostic counters. Both accumulate since the last power-cycle/reset only (like currentlyrotating
//and friends above) - no reset command or EEPROM persistence, deliberately kept simple. Neither is
//a hard error by itself; both are a coarse "has this unit been struggling lately" signal, surfaced
//to ESPMaster via requestEvent() below and GET /api/units?detail=1.
//
//Incremented in stepLetters() whenever the hall sensor reads LOW (marker state) at any point
//during a DIRECT move - a direct move (by construction) never expects to cross the home marker, so
//any such reading is anomalous (bumped drum, slipped steps, sensor noise). stepLetters() also
//clears positionKnown when this happens - the existing !positionKnown dispatch in loop() (see its
//own comment) then forces a full recalibration on the very next iteration.
volatile uint8_t unexpectedHomeTriggerCount = 0;

//Incremented in calibrate()'s own success path when it took unusually many steps (more than
//EXPECTED_HOME_SEARCH_MAX_STEPS, but still under the existing 3*STEPS hard-failure cap) to find
//the marker - "found it, but struggled" rather than an outright failure. Doesn't change the actual
//calibration result (still succeeds, positionKnown still becomes true) - purely an early-warning
//diagnostic that would otherwise be completely invisible (calibrate()'s own step count isn't
//looked at by anything today beyond the hard failure cap).
volatile uint8_t slowHomeSearchCount = 0;

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
  //Also re-attempt whenever positionKnown is false, even if the numbers already coincide - without
  //this, a failed calibration that happened to leave displayedLetter equal to receivedNumber
  //(e.g. both 0/space) would never call rotateToLetter() again, so positionKnown staying false
  //would have no effect (the gate it controls lives INSIDE a function never invoked).
  //rotateToLetter() itself already handles this correctly once actually called - positionKnown==
  //false forces its full-rotation/recalibration branch regardless of the numeric relationship
  //between the two positions.
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

//Approximate letter-space position of the Hall marker, given the current calibration offset.
//calibrate() defines letter 0 as calOffset STEPS past the marker (see its own
//stepper.step(offsetSnapshot) call), so the marker itself sits calOffset steps BEHIND letter 0 -
//converting that to letter-space and wrapping into [0, AMOUNTFLAPS) gives its approximate letter
//index. Deliberately as coarse as stepLetters()'s own plausibility check below (per
//letter-position, not per microstep) - it only needs to be right to within about one
//letter-position's worth of steps.
int markerLetterIndex() {
  float stepsPerLetter = (float)STEPS / (float)AMOUNTFLAPS;
  uint16_t offsetSnapshot;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    offsetSnapshot = calOffset;
  }
  int offsetLetters = ((int)round((float)offsetSnapshot / stepsPerLetter)) % AMOUNTFLAPS;
  return (AMOUNTFLAPS - offsetLetters) % AMOUNTFLAPS;
}

//Moves the drum forward by `letterSteps` letter-positions, ramping speed at both ends instead of
//jumping straight to full stepperSpeed / braking abruptly: RAMP_UP_LETTERS at the very start,
//RAMP_DOWN_LETTERS at the very end (see both constants' own comments) - whichever ramp is more
//conservative at a given position wins, which also handles short moves where both windows overlap
//without any special-casing. Shared by both rotateToLetter() branches (direct move, and the
//post-calibration full-rotation move) - both only ever call this with letterSteps < AMOUNTFLAPS
//(diffPosition/posLetter are always 0..44), so a single call here never covers a full revolution.
//
//`startLetter` is the absolute letter index this specific move starts FROM - always 0 for the
//post-calibration full-rotation call, since calibrate() itself already left the drum exactly at
//letter 0 (see its own comment). calOffset can place the marker's own letter-space position (see
//markerLetterIndex() above) ANYWHERE within the 0..AMOUNTFLAPS-1 range, not necessarily exactly at
//the 0/AMOUNTFLAPS-1 boundary, so a move that happens to span across it - post-calibration or a
//"direct" move (rotateToLetter()'s other branch) alike - is a perfectly legitimate crossing, not a
//bumped-drum/slipped-step anomaly - see the hall-sensor check below (unexpectedHomeTriggerCount's
//own comment) for how this is taken into account.
void stepLetters(int letterSteps, int startLetter) {
  //Precompute the one loop iteration `i` (if any) within THIS move where the marker is
  //legitimately expected, so the check below can tell "the marker was crossed exactly where
  //calOffset says it should be" apart from "the marker showed up somewhere it has no business
  //being". Iteration i steps the drum from absolute letter (startLetter+i) to (startLetter+i+1)
  //and checks HALLPIN right after - so the marker (at markerLetterIndex()) is expected to be seen
  //during iteration i = markerLetterIndex()-startLetter-1, INCLUSIVE of the last iteration
  //(letterSteps-1, i.e. the marker sitting exactly at the target letter itself is a valid, in-range
  //crossing, not one-past-the-end).
  int expectedCrossingStep = -1;
  if (startLetter >= 0) {
    int crossingIndex = markerLetterIndex() - startLetter - 1;
    if (crossingIndex >= 0 && crossingIndex < letterSteps) {
      expectedCrossingStep = crossingIndex;
    }
  }

  stepper.setSpeed(stepperSpeed);

  for (int i = 0; i < letterSteps; i++) {
    currentPhaseProgress = (uint16_t)i;

    float speedFactor = 1.0;

    if (RAMP_UP_LETTERS > 0 && i < RAMP_UP_LETTERS) {
      float rampFraction = (float)(i + 1) / (float)(RAMP_UP_LETTERS + 1);
      float upFactor = RAMP_UP_MIN_FACTOR + (1.0 - RAMP_UP_MIN_FACTOR) * rampFraction;
      if (upFactor < speedFactor) {
        speedFactor = upFactor;
      }
    }

    int stepsRemaining = letterSteps - i;
    if (RAMP_DOWN_LETTERS > 0 && stepsRemaining <= RAMP_DOWN_LETTERS) {
      float rampFraction = (float)stepsRemaining / (float)(RAMP_DOWN_LETTERS + 1);
      float downFactor = RAMP_DOWN_MIN_FACTOR + (1.0 - RAMP_DOWN_MIN_FACTOR) * rampFraction;
      if (downFactor < speedFactor) {
        speedFactor = downFactor;
      }
    }

    if (speedFactor < 1.0) {
      int rampedSpeed = (int)(stepperSpeed * speedFactor);
      stepper.setSpeed(rampedSpeed > 0 ? rampedSpeed : 1); //never 0, Stepper::step() would spin forever
    } else {
      stepper.setSpeed(stepperSpeed);
    }

    float preciseStep = (float)STEPS / (float)AMOUNTFLAPS;
    int roundedStep = (int)preciseStep;
    missedSteps = missedSteps + ((float)preciseStep - (float)roundedStep);
    if (missedSteps > 1) {
      roundedStep = roundedStep + 1;
      missedSteps--;
    }
    stepper.step(ROTATIONDIRECTION * roundedStep);

    //Continuous plausibility check (see unexpectedHomeTriggerCount's own comment) - as coarse an
    //approximation of "continuous" as the blocking Stepper library allows: only after each whole
    //letter-position's worth of microsteps (~STEPS/AMOUNTFLAPS ≈ 45), not truly during it. A very
    //brief sensor blip entirely inside one step() call could still be missed - an accepted
    //limitation, not a claim of full per-microstep coverage.
    if (digitalRead(HALLPIN) == 0) {
      //Allow one letter-position's worth of slack around the precomputed expected crossing point -
      //this check only samples once per letter-position to begin with (see this function's own
      //header comment), so it can't pin the crossing down to single-microstep precision, and the
      //ramp-adjusted step size right at RAMP_UP_LETTERS/RAMP_DOWN_LETTERS boundaries isn't
      //perfectly uniform either.
      bool expectedHere = (expectedCrossingStep >= 0) && (abs(i - expectedCrossingStep) <= 1);
      if (!expectedHere) {
        //Marker seen where none should be, mid-move (see this function's own header comment for
        //why a single call never legitimately covers a full revolution, and why a crossing AT the
        //calibration offset's own expected position is not itself anomalous). Don't abort THIS
        //move - just flag it and let the existing loop() dispatch self-heal via a full
        //recalibration on the next iteration, instead of adding a new abort path to this tight
        //timing loop.
        unexpectedHomeTriggerCount++;
        positionKnown = false;
      }
    }
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
        stepLetters(diffPosition, posCurrentLetter);
      }
      else {
        //full rotation is needed, good time for a calibration
#ifdef SERIAL_ENABLE
        Serial.println(F("full rotation incl. calibration"));
#endif
        //On failure (-1, Hall sensor/marker never found within 3*STEPS), calibrate() already stops
        //the motor and resets displayedLetter to 0 as an honest "actual position unknown" - abort
        //here too, rather than stepping further from that unverified position and claiming
        //displayedLetter=toLetter as if the rotation had genuinely succeeded. loop()'s existing
        //`displayedLetter != receivedNumber` retry naturally tries again on the next call.
        if (calibrate(false) == -1) {
          //Re-stamp lastRotation HERE, not just at this function's own entry above: the
          //OVERHEATINGTIMEOUT cooldown is measured from lastRotation, set at the START of this
          //attempt - but a failed calibrate() search can itself run for up to 3*STEPS steps (well
          //over OVERHEATINGTIMEOUT's 2s worth of stepping), so the cooldown window can already be
          //fully expired again by the time this function returns. Without this, the very next
          //loop() iteration would immediately start another full failed search with no actual rest
          //for the motor in between. Re-stamping restarts the cooldown from the point of FAILURE
          //instead, guaranteeing at least one genuine OVERHEATINGTIMEOUT-long gap before the next attempt.
          lastRotation = millis();
          return;
        }
        //startMotor();
        currentPhase = PHASE_ROTATING; //calibrate() left it at PHASE_CALIBRATING, we're rotating to the target letter now
        stepLetters(posLetter, 0); //fresh from letter 0, right after calibrate() - see stepLetters()'s own comment
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
  //checksum. A bare {letter, checksum} 2-byte packet must be rejected here too - otherwise
  //receiveArray[1] (the checksum byte itself) would get silently read as the speed a few lines below.
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
  //(1-in-256 chance per corrupted byte) must still be rejected. Reject it the same way
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
    //outside a real rotation's worth of steps, the same failure mode getOffset() below guards against.
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
  //displayedLetter whenever it's called again too soon after the last rotation. currentlyrotating
  //alone stays 0 for the whole cooldown wait, which would let the master's "is this unit ready"
  //poll (pollDisplayJobWaitAllUnitsReady() in ESPMaster.ino) consider the unit already done before
  //it had even started moving toward the newly-received letter.
  //Also busy while positionKnown is false - without this, a unit whose last calibration failed but
  //whose displayedLetter (reset to 0 by that failure) happened to already equal receivedNumber
  //would report itself READY despite its actual position being unconfirmed.
  uint8_t effectivelyBusy = (currentlyrotating != 0 || displayedLetter != receivedNumber || !positionKnown) ? 1 : 0;
  Wire.write(effectivelyBusy);
  Wire.write(lowByte(calOffset));
  Wire.write(highByte(calOffset));
  Wire.write(currentPhase);
  Wire.write(lowByte(currentPhaseProgress));
  Wire.write(highByte(currentPhaseProgress));
  //Diagnostic counters (see their own comments above) - appended, not inserted, so this stays
  //backwards compatible: a master still requesting only ANSWER_SIZE_LIGHT (3) or the previous
  //ANSWER_SIZE (6) bytes via Wire.requestFrom() simply reads fewer of the bytes written here, same
  //as how the light 3-byte read and the previous 6-byte read already coexisted with this same
  //handler always writing 6. Only a master that now asks for all 8 (ESPMaster.ino's
  //ANSWER_SIZE_DIAG) sees these two.
  Wire.write(unexpectedHomeTriggerCount);
  Wire.write(slowHomeSearchCount);
  //Firmware version - same backwards-compatible append-only extension as the two diagnostic
  //counters above. A master still requesting only ANSWER_SIZE/ANSWER_SIZE_LIGHT/ANSWER_SIZE_DIAG
  //bytes simply never reads these three.
  Wire.write(UNIT_FW_VERSION_MAJOR);
  Wire.write(UNIT_FW_VERSION_MINOR);
  Wire.write(UNIT_FW_VERSION_PATCH);
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
  //calOffset is uint16_t, so an upper-bound check is what matters: a blank/erased EEPROM (0xFFFF =
  //65535, e.g. after a fresh ISP chip-erase) would otherwise leave calibrate()'s
  //stepper.step(ROTATIONDIRECTION * calOffset) running with a bogus value far larger than one
  //rotation's worth of steps (an unintended multi-minute spin). Same valid range ESPMaster.ino's
  //own /api/units?action=offset already validates against (0..STEPS-1).
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
  //Two-phase edge search: accepting ANY LOW reading as "found" (without requiring a genuine
  //HIGH-to-LOW entry edge) would let a permanently-LOW/stuck sensor, or a marker zone wider than
  //expected, be accepted as a successful calibration. leavingZone tracks "we're still inside the
  //zone we started in (or moved into) and haven't yet observed a genuine HIGH reading" - only once
  //HIGH has actually been seen does the final `else` below get to declare the NEXT LOW a real,
  //repeatable edge. If the sensor never goes HIGH at all, leavingZone simply never clears and the
  //existing i > 3*STEPS cap below times this out as a failure, exactly like it already does for a
  //sensor stuck HIGH.
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
      //Early-warning diagnostic (see slowHomeSearchCount's own comment) - found the marker, so
      //this is still a genuine success below, just an unusually long search for one. Doesn't
      //change the outcome, only adds visibility that would otherwise not exist.
      if (i > EXPECTED_HOME_SEARCH_MAX_STEPS) {
        slowHomeSearchCount++;
      }
      //Snapshot atomically before use - a torn calOffset here would turn the motor by a wrong,
      //momentarily-garbled step count instead of just reporting a stale status value (see the
      //volatile-block comment near calOffset's declaration).
      uint16_t offsetSnapshot;
      ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        offsetSnapshot = calOffset;
      }
      //Chunk the offset move instead of one single blocking stepper.step(offsetSnapshot) call: at
      //the slowest allowed speed (1 RPM) and the largest allowed offset (STEPS-1), that single call
      //would block for up to ~60s with currentPhaseProgress never updating in between - long enough
      //for ESPMaster's own stuck-detection (pollDisplayJobWaitAllUnitsReady(), 15s without a
      //progress change) to force-reboot a unit that's actually still correctly, if slowly, homing.
      //Same per-chunk progress update idea as stepLetters()'s own per-letter-position tracking.
      uint16_t offsetStepsRemaining = offsetSnapshot;
      while (offsetStepsRemaining > 0) {
        uint16_t thisChunk = offsetStepsRemaining < CALIBRATION_OFFSET_STEP_CHUNK ? offsetStepsRemaining : CALIBRATION_OFFSET_STEP_CHUNK;
        stepper.step(ROTATIONDIRECTION * thisChunk);
        offsetStepsRemaining -= thisChunk;
        currentPhaseProgress = (uint16_t)(offsetSnapshot - offsetStepsRemaining); //steps done so far - keeps the master's stuck-detection seeing real, live progress
      }
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
