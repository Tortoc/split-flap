/*********
  Split Flap Unit (ATmega328P on the fae-pcb)
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

//Sent over I2C in place of a letter to request a reboot into twiboot for a firmware update (see
//Unit/BOOTLOADER_SETUP.md). Outside the letter range (0-44), so it can't collide.
#define CMD_ENTER_BOOTLOADER 255

//Unit.ino version (semver), reported to ESPMaster via requestEvent()'s last 3 bytes; older units
//read back as 255/255/255 ("unknown").
#define UNIT_FW_VERSION_MAJOR 2
#define UNIT_FW_VERSION_MINOR 2
#define UNIT_FW_VERSION_PATCH 4

//Address jumpers J4 on the fae-pcb, 6 bits = addresses 0-63. J4 has 8 pin pairs; the top two (D6,
//D5) stay unused - twiboot adds a fixed 0x29 base (main.c, keep in sync), so 6 bits (bootloader
//0x29-0x68) is the safe maximum. Top to bottom: D6 (pin 1, next to the ISP header) - D5 - D4 - D3 -
//A3 - A2 - A1 - A0 (pin 15). Bit 0 is the bottom pair: A0..A3 = bits 0-3, D3 = bit 4, D4 = bit 5.
//No jumper = 0 (INPUT_PULLUP, getaddress() inverts each bit).
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

//Acceleration ramp, the counterpart of RAMP_DOWN_* for the start of a move: a slower start needs
//less current/torque (possibly relevant for slow rotation caused by voltage sag). 0.6 is slightly
//more conservative than RAMP_DOWN_MIN_FACTOR - starting from a stop needs more torque.
#define RAMP_UP_LETTERS 1
#define RAMP_UP_MIN_FACTOR 0.6 //speed fraction (of stepperSpeed) used on the first letter-position

//Diagnostic only: a successful home search that took more steps than this counts as struggling
//(slowHomeSearchCount). Normally the marker is found within about one revolution; this is a
//generous multiple, well below calibrate()'s 3*STEPS failure cap. An estimate, not measured.
#define EXPECTED_HOME_SEARCH_MAX_STEPS (STEPS + STEPS / 2)

//Chunk size for calibrate()'s offset move: even at 1 RPM (~29ms/step) a chunk takes ~0.6s, so
//currentPhaseProgress updates far more often than the master's 15s stuck detection.
#define CALIBRATION_OFFSET_STEP_CHUNK 20

//constants others
#define BAUDRATE 115200
//Valid stepperSpeed range - keep in sync with ESPMaster.ino's MIN_SPEED/MAX_SPEED. Enforced here
//too, so a garbled but checksum-valid speed byte never reaches the motor unclamped.
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
//Wire's onReceive()/onRequest() callbacks run inside the TWI interrupt, so variables shared with
//normal code are `volatile`. That doesn't make multi-byte access atomic: ATOMIC_BLOCK is used where
//a torn value matters physically (how far the motor turns, whether a rotation is issued). The
//self-correcting status values (currentPhaseProgress, currentlyrotating, currentPhase) stay plain
//volatile - not worth the overhead in the hot step loops.
volatile int displayedLetter = 0; //currently shown letter
//Whether displayedLetter is a confirmed position (calibrate() reached the marker) and not just its
//"reset to 0" fallback after a failure - otherwise the next move could start from a wrong position.
//Set by calibrate() (true on success, false on failure).
volatile bool positionKnown = false; //starts false: no calibration has run yet
int desiredLetter = 0; //letter to be shown
//'$', '&', '#' stand for the Ä/Ö/Ü flaps (same placeholders as ESPMaster's letters[]). Only the
//serial test path reads characters; I2C transfers positions. 'Ä' literals would be multi-byte UTF-8.
const char letters[] = {' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '$', '&', '#', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', '-', '?', '!'};
Stepper stepper(STEPS, STEPPERPIN1, STEPPERPIN3, STEPPERPIN2, STEPPERPIN4); //stepper setup
bool lastInd1 = false; //store last status of phase
bool lastInd2 = false; //store last status of phase
bool lastInd3 = false; //store last status of phase
bool lastInd4 = false; //store last status of phase
float missedSteps = 0; //cummulate steps <1, to compensate via additional step when reaching >1
volatile int currentlyrotating = 0; // 1 = drum is currently rotating, 0 = drum is standing still

//Diagnostic counters since the last reset (no persistence), reported via requestEvent() and
//GET /api/units?detail=1 - a coarse "has this unit been struggling" signal, not errors.
//
//unexpectedHomeTriggerCount: the hall sensor read LOW where no marker crossing was expected during a
//move (bumped drum, slipped steps, noise). stepLetters() then clears positionKnown, so loop()
//recalibrates on the next pass.
volatile uint8_t unexpectedHomeTriggerCount = 0;

//slowHomeSearchCount: calibrate() found the marker, but only after more than
//EXPECTED_HOME_SEARCH_MAX_STEPS - an early warning; the calibration still succeeds.
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

  //INPUT_PULLUP: an open-collector sensor without its own pull-up would float and calibrate() could
  //miss the marker. Revert if homing gets worse (that would point to a push-pull sensor).
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
    //calOffset is 2 bytes - snapshot it atomically, a TWI interrupt could update it mid-read.
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
  //Both are 2-byte ints written from different contexts - snapshot them atomically.
  int receivedNumberSnapshot;
  int displayedLetterSnapshot;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    receivedNumberSnapshot = receivedNumber;
    displayedLetterSnapshot = displayedLetter;
  }
  //Also retry while positionKnown is false, even if the numbers already match - a failed
  //calibration may have left displayedLetter equal to receivedNumber, and rotateToLetter() is what
  //forces the recalibration.
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

//Approximate letter index of the Hall marker: calibrate() puts letter 0 calOffset steps past the
//marker, so the marker sits calOffset steps behind letter 0. As coarse as stepLetters()' check (one
//letter position).
int markerLetterIndex() {
  float stepsPerLetter = (float)STEPS / (float)AMOUNTFLAPS;
  uint16_t offsetSnapshot;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    offsetSnapshot = calOffset;
  }
  int offsetLetters = ((int)round((float)offsetSnapshot / stepsPerLetter)) % AMOUNTFLAPS;
  return (AMOUNTFLAPS - offsetLetters) % AMOUNTFLAPS;
}

//Moves the drum forward by `letterSteps` letter positions with ramps at both ends (RAMP_UP_LETTERS,
//RAMP_DOWN_LETTERS; the more conservative ramp wins where they overlap). Used by both
//rotateToLetter() branches, always with letterSteps < AMOUNTFLAPS, so one call never covers a full
//revolution.
//
//`startLetter` is the absolute letter the move starts from (0 after calibrate()). calOffset can put
//the marker anywhere in the letter range, so crossing it is legitimate when it happens exactly where
//calOffset says - see the hall-sensor check below.
void stepLetters(int letterSteps, int startLetter) {
  //The one iteration i (if any) where the marker is expected: iteration i moves from letter
  //startLetter+i to +i+1 and then reads HALLPIN, so i = markerLetterIndex()-startLetter-1, the last
  //iteration included (marker at the target letter).
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

    //Plausibility check, sampled once per letter position (~45 microsteps) - the blocking Stepper
    //library allows nothing finer, so a very short blip can be missed.
    if (digitalRead(HALLPIN) == 0) {
      //One letter position of slack around the expected crossing (coarse sampling, ramps).
      bool expectedHere = (expectedCrossingStep >= 0) && (abs(i - expectedCrossingStep) <= 1);
      if (!expectedHere) {
        //Marker where none belongs: don't abort this move, just flag it - loop() recalibrates on
        //the next pass.
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
      //Direct move only if the target is ahead AND the current position is confirmed
      //(positionKnown) - otherwise full rotation with calibration.
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
        //Calibration failed (marker not found within 3*STEPS): calibrate() stopped the motor and
        //reset displayedLetter to 0 - abort instead of claiming success; loop() retries.
        if (calibrate(false) == -1) {
          //Re-stamp lastRotation: a failed search can take longer than OVERHEATINGTIMEOUT, so
          //without this the next attempt would start at once with no rest for the motor.
          lastRotation = millis();
          return;
        }
        //startMotor();
        currentPhase = PHASE_ROTATING; //calibrate() left it at PHASE_CALIBRATING, we're rotating to the target letter now
        stepLetters(posLetter, 0); //from letter 0, right after calibrate()
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
  int receiveArray[5]; //received bytes incl. checksum
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

  //A real message has letter + speed + checksum; a 2-byte packet would make the checksum the speed.
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

  //A valid checksum doesn't make a plausible letter - reject positions outside 0..44.
  if (receiveArray[0] < 0 || receiveArray[0] >= AMOUNTFLAPS) {
    return;
  }

  //Write received bytes to correct variables
  receivedNumber = receiveArray[0];
  //Clamp speed: 0 makes Stepper::setSpeed() divide by zero and step() spin forever (needs a power
  //cycle); too high risks missed steps. Same MIN_SPEED..MAX_SPEED as ESPMaster.
  stepperSpeed = constrain(receiveArray[1], MIN_SPEED, MAX_SPEED);
  if (payloadLen >= 4) {
    newCalOffset = receiveArray[2] + (receiveArray[3] << 8);
    //Same range as ESPMaster's offset endpoint (0..STEPS-1), so a garbled offset can't be stored.
    if (newCalOffset < STEPS && newCalOffset != calOffset) {
      calOffset = newCalOffset;
      //EEPROM.put() blocks for ms - deferred to loop(), we're inside the TWI interrupt here.
      pendingEepromWrite = true;
      forceRecalibrate = true; //apply the new offset right away instead of waiting for the next full rotation
    }
  }
}

void requestEvent() {
  //Busy while rotating, while a received letter isn't applied yet (e.g. during the
  //OVERHEATINGTIMEOUT cooldown, when currentlyrotating is still 0) and while positionKnown is false -
  //otherwise the master's readiness poll would take the unit as done too early.
  uint8_t effectivelyBusy = (currentlyrotating != 0 || displayedLetter != receivedNumber || !positionKnown) ? 1 : 0;
  Wire.write(effectivelyBusy);
  Wire.write(lowByte(calOffset));
  Wire.write(highByte(calOffset));
  Wire.write(currentPhase);
  Wire.write(lowByte(currentPhaseProgress));
  Wire.write(highByte(currentPhaseProgress));
  //Diagnostic counters, appended so older masters reading 3 or 6 bytes are unaffected; only
  //ANSWER_SIZE_DIAG (8) reads them.
  Wire.write(unexpectedHomeTriggerCount);
  Wire.write(slowHomeSearchCount);
  //Firmware version, appended the same way (read with ANSWER_SIZE_VERSION).
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

//Returns the unit address 0-63 (pin mapping: see addressPins).
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
  //calOffset is uint16_t, so only the upper bound matters: an erased EEPROM (0xFFFF, e.g. after an
  //ISP chip erase) would spin the motor for minutes. Same range as ESPMaster (0..STEPS-1).
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
  currentPhase = PHASE_CALIBRATING;
  bool reachedMarker = false;
  stepper.setSpeed(stepperSpeed);
  int i = 0;
  //Two-phase edge search: only a real HIGH->LOW edge counts, so a sensor stuck LOW (or a wider
  //marker zone) isn't taken as success. leavingZone stays set until HIGH was seen; a sensor that
  //never goes HIGH times out at 3*STEPS like one stuck HIGH.
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
      //Found, but slowly - counted as an early warning, still a success.
      if (i > EXPECTED_HOME_SEARCH_MAX_STEPS) {
        slowHomeSearchCount++;
      }
      //Snapshot atomically - a torn calOffset would turn the motor by a wrong amount.
      uint16_t offsetSnapshot;
      ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        offsetSnapshot = calOffset;
      }
      //Chunked instead of one blocking step() call: at 1 RPM and the largest offset that call took
      //~60s without progress updates, and ESPMaster's 15s stuck detection would reboot a unit that
      //is only homing slowly.
      uint16_t offsetStepsRemaining = offsetSnapshot;
      while (offsetStepsRemaining > 0) {
        uint16_t thisChunk = offsetStepsRemaining < CALIBRATION_OFFSET_STEP_CHUNK ? offsetStepsRemaining : CALIBRATION_OFFSET_STEP_CHUNK;
        stepper.step(ROTATIONDIRECTION * thisChunk);
        offsetStepsRemaining -= thisChunk;
        currentPhaseProgress = (uint16_t)(offsetSnapshot - offsetStepsRemaining); //live progress for the master's stuck detection
      }
      displayedLetter = 0;
      positionKnown = true; //this 0 is confirmed
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
      positionKnown = false; //this 0 is NOT confirmed
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
  currentPhase = PHASE_IDLE;
  currentPhaseProgress = 0;
  delay(100);
}

void startMotor() {
  currentPhase = PHASE_ROTATING;
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
      //Validate the entire command before changing RAM, EEPROM or the motor position.
      char* endPtr;
      long newOffset = strtol(receivedChars, &endPtr, 10);
      if (endPtr == receivedChars || *endPtr != '\0' || newOffset < 0 || newOffset >= STEPS) {
        Serial.println(F("Invalid Command"));
      } else {
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
          calOffset = (uint16_t)newOffset;
        }
        // save to eeprom
        writeToEEPROM((uint16_t)newOffset);
        calibrate(true);
      }
    }
  }
}
#endif
