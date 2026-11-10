/*
  Multi-Mode Robot Car  (Arduino UNO)

  Control inputs : HC-05 Bluetooth (phone app) and an IR remote
  Modes          : MANUAL  - drive with the app / remote
                   LINE    - follow a black line using two IR sensors
                   AVOID   - ultrasonic sensor on a servo, steer toward open space

  Command bytes (same from Bluetooth and IR):
     1 forward   2 backward   3 left   4 right   5 stop
     6 nudge left (0.4 s)     7 nudge right (0.4 s)
     8 manual mode   9 line-follow mode   10 obstacle-avoid mode
     >20 sets motor speed (PWM value)

  Voice control: the phone app converts spoken words into the same bytes.

  Libraries: IRremote (v4.x). SoftwareSerial is built in.
  Tip: unplug the HC-05 from D2/D3 while uploading.
*/

#include <SoftwareSerial.h>
#include <IRremote.h>

// ------------------------------------------------------------------
// Pin map
// ------------------------------------------------------------------
const uint8_t PIN_BT_RX   = 2;    // HC-05 TX  -> Arduino D2
const uint8_t PIN_BT_TX   = 3;    // HC-05 RX  <- Arduino D3 (through divider)

const uint8_t PIN_ENB     = 5;    // L298N ENB (PWM)
const uint8_t PIN_IN4     = 6;
const uint8_t PIN_IN3     = 7;
const uint8_t PIN_IN2     = 8;
const uint8_t PIN_IN1     = 9;
const uint8_t PIN_ENA     = 10;   // L298N ENA (PWM)

const uint8_t PIN_LINE_R  = A0;   // right line sensor
const uint8_t PIN_LINE_L  = A1;   // left line sensor
const uint8_t PIN_ECHO    = A2;   // HC-SR04 echo
const uint8_t PIN_TRIG    = A3;   // HC-SR04 trigger
const uint8_t PIN_SERVO   = A4;   // SG90 signal
const uint8_t PIN_IR      = A5;   // IR receiver

// ------------------------------------------------------------------
// Tunable settings
// ------------------------------------------------------------------
const uint8_t  SPEED_LINE        = 130;
const uint8_t  SPEED_AVOID       = 255;
const uint16_t NUDGE_MS          = 400;
const uint16_t STOP_DISTANCE_CM  = 20;
const uint16_t NO_ECHO_CM        = 200;   // reported when nothing is in range
const uint16_t AVOID_TURN_MS     = 450;
const uint16_t AVOID_BACKUP_MS   = 300;

// Servo angles (calibrate for how your servo is mounted)
const uint8_t SERVO_CENTER = 70;
const uint8_t SERVO_LEFT   = 140;
const uint8_t SERVO_RIGHT  = 0;

// Line sensors straddle the line: HIGH = sensor sees the black line
const uint8_t ON_LINE = HIGH;

// ------------------------------------------------------------------
// Commands and IR remote map
// ------------------------------------------------------------------
enum Command : uint8_t {
  CMD_NONE = 0,
  CMD_FORWARD, CMD_BACKWARD, CMD_LEFT, CMD_RIGHT, CMD_STOP,
  CMD_NUDGE_LEFT, CMD_NUDGE_RIGHT,
  CMD_MODE_MANUAL, CMD_MODE_LINE, CMD_MODE_AVOID
};

enum Mode : uint8_t { MODE_MANUAL, MODE_LINE, MODE_AVOID };

struct IrKey {
  uint32_t raw;
  uint8_t  cmd;
};

// Replace the raw values with the ones printed by YOUR remote
// (open the Serial Monitor at 9600 baud and press each button).
const IrKey IR_KEYS[] = {
  {0x3880, CMD_FORWARD},
  {0x2883, CMD_BACKWARD},
  {0x3886, CMD_LEFT},
  {0x2885, CMD_RIGHT},
  {0x389E, CMD_STOP},
  {0x28A2, CMD_MODE_MANUAL},
  {0x38A4, CMD_MODE_LINE},
  {0x28A7, CMD_MODE_AVOID}
};
const uint8_t IR_KEY_COUNT = sizeof(IR_KEYS) / sizeof(IR_KEYS[0]);

// ------------------------------------------------------------------
// State
// ------------------------------------------------------------------
SoftwareSerial bt(PIN_BT_RX, PIN_BT_TX);

Mode     mode       = MODE_MANUAL;
uint8_t  speedPwm   = 130;
bool     nudging    = false;
uint32_t nudgeEndMs = 0;

// ------------------------------------------------------------------
// Motor helpers
// ------------------------------------------------------------------
void setMotors(bool a1, bool a2, bool b1, bool b2) {
  digitalWrite(PIN_IN1, a1);
  digitalWrite(PIN_IN2, a2);
  digitalWrite(PIN_IN3, b1);
  digitalWrite(PIN_IN4, b2);
}

void applySpeed() {
  analogWrite(PIN_ENA, speedPwm);
  analogWrite(PIN_ENB, speedPwm);
}

void driveForward()  { setMotors(HIGH, LOW,  LOW,  HIGH); }
void driveBackward() { setMotors(LOW,  HIGH, HIGH, LOW);  }
void spinLeft()      { setMotors(HIGH, LOW,  HIGH, LOW);  }
void spinRight()     { setMotors(LOW,  HIGH, LOW,  HIGH); }
void stopMotors()    { setMotors(LOW,  LOW,  LOW,  LOW);  }

// ------------------------------------------------------------------
// Servo (software pulses, so Timer1 stays free for PWM on D9/D10)
// ------------------------------------------------------------------
void aimServo(uint8_t angle) {
  uint16_t pulseUs = map(angle, 0, 180, 500, 2400);
  for (uint8_t i = 0; i < 15; i++) {
    digitalWrite(PIN_SERVO, HIGH);
    delayMicroseconds(pulseUs);
    digitalWrite(PIN_SERVO, LOW);
    delay(20);
  }
}

// ------------------------------------------------------------------
// Ultrasonic distance in cm
// ------------------------------------------------------------------
uint16_t readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long echoUs = pulseIn(PIN_ECHO, HIGH, 25000UL);
  if (echoUs == 0) return NO_ECHO_CM;
  return echoUs / 58;
}

// ------------------------------------------------------------------
// Command handling
// ------------------------------------------------------------------
void manualDrive(uint8_t cmd) {
  switch (cmd) {
    case CMD_FORWARD:     driveForward();  break;
    case CMD_BACKWARD:    driveBackward(); break;
    case CMD_LEFT:        spinLeft();      break;
    case CMD_RIGHT:       spinRight();     break;
    case CMD_STOP:        stopMotors();    break;
    case CMD_NUDGE_LEFT:  spinLeft();  nudging = true; nudgeEndMs = millis() + NUDGE_MS; break;
    case CMD_NUDGE_RIGHT: spinRight(); nudging = true; nudgeEndMs = millis() + NUDGE_MS; break;
    default: break;
  }
}

void handleCommand(uint8_t cmd) {
  switch (cmd) {
    case CMD_MODE_MANUAL:
      mode = MODE_MANUAL;
      nudging = false;
      stopMotors();
      break;
    case CMD_MODE_LINE:
      mode = MODE_LINE;
      nudging = false;
      speedPwm = SPEED_LINE;
      applySpeed();
      break;
    case CMD_MODE_AVOID:
      mode = MODE_AVOID;
      nudging = false;
      speedPwm = SPEED_AVOID;
      applySpeed();
      break;
    default:
      if (mode == MODE_MANUAL) manualDrive(cmd);
      break;
  }
}

// ------------------------------------------------------------------
// Input polling
// ------------------------------------------------------------------
void pollBluetooth() {
  while (bt.available() > 0) {
    uint8_t b = bt.read();
    Serial.print(F("BT: "));
    Serial.println(b);
    if (b > 20) {
      speedPwm = b;
      applySpeed();
    } else {
      handleCommand(b);
    }
  }
}

uint8_t lookupIr(uint32_t raw) {
  for (uint8_t i = 0; i < IR_KEY_COUNT; i++) {
    if (IR_KEYS[i].raw == raw) return IR_KEYS[i].cmd;
  }
  return CMD_NONE;
}

void pollIr() {
  if (!IrReceiver.decode()) return;

  if (!(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
    uint32_t raw = (uint32_t)IrReceiver.decodedIRData.decodedRawData;
    Serial.print(F("IR: 0x"));
    Serial.println(raw, HEX);
    uint8_t cmd = lookupIr(raw);
    if (cmd != CMD_NONE) handleCommand(cmd);
  }
  IrReceiver.resume();
}

void pollInputs() {
  pollIr();
  pollBluetooth();
}

// Wait while still listening for commands.
// Returns false if the mode was changed during the wait.
bool waitAndListen(uint16_t ms) {
  uint32_t end = millis() + ms;
  while ((int32_t)(millis() - end) < 0) {
    pollInputs();
    if (mode != MODE_AVOID) return false;
  }
  return true;
}

// ------------------------------------------------------------------
// Autonomous modes
// ------------------------------------------------------------------
void runLineFollow() {
  bool rightOn = (digitalRead(PIN_LINE_R) == ON_LINE);
  bool leftOn  = (digitalRead(PIN_LINE_L) == ON_LINE);

  if (!rightOn && !leftOn)      driveForward();
  else if (rightOn && !leftOn)  spinRight();
  else if (!rightOn && leftOn)  spinLeft();
  else                          stopMotors();
}

uint16_t lookAt(uint8_t angle) {
  aimServo(angle);
  delay(150);
  return readDistanceCm();
}

void runObstacleAvoid() {
  uint16_t ahead = readDistanceCm();
  if (ahead > STOP_DISTANCE_CM) {
    driveForward();
    return;
  }

  stopMotors();
  if (!waitAndListen(100)) return;

  uint16_t leftCm  = lookAt(SERVO_LEFT);
  uint16_t rightCm = lookAt(SERVO_RIGHT);
  aimServo(SERVO_CENTER);

  if (leftCm > rightCm) {
    spinLeft();
    waitAndListen(AVOID_TURN_MS);
  } else if (rightCm > leftCm) {
    spinRight();
    waitAndListen(AVOID_TURN_MS);
  } else {
    driveBackward();
    if (!waitAndListen(AVOID_BACKUP_MS)) return;
    spinRight();
    waitAndListen(AVOID_TURN_MS + 150);
  }
  stopMotors();
}

// ------------------------------------------------------------------
// Arduino entry points
// ------------------------------------------------------------------
void setup() {
  pinMode(PIN_LINE_R, INPUT);
  pinMode(PIN_LINE_L, INPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_SERVO, OUTPUT);

  pinMode(PIN_ENA, OUTPUT);
  pinMode(PIN_ENB, OUTPUT);
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);

  Serial.begin(9600);
  bt.begin(9600);
  IrReceiver.begin(PIN_IR, ENABLE_LED_FEEDBACK);

  stopMotors();
  applySpeed();

  // Short servo sweep so you can see the sensor head is alive
  aimServo(SERVO_LEFT);
  aimServo(SERVO_RIGHT);
  aimServo(SERVO_CENTER);

  Serial.println(F("Robot ready - manual mode"));
}

void loop() {
  pollInputs();

  if (nudging && (int32_t)(millis() - nudgeEndMs) >= 0) {
    stopMotors();
    nudging = false;
  }

  switch (mode) {
    case MODE_LINE:   runLineFollow();    break;
    case MODE_AVOID:  runObstacleAvoid(); break;
    default:          break;
  }
}
