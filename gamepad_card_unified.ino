/*
  MemCARDuino - Arduino PlayStation 1 Memory Card reader
  Combined with PSX GAMEPAD TO USB (SPI Conflict Fixed)
*/
// Memory card reader library
#include "Arduino.h"              // Arduino own library
#include <SPI.h>                  // Arduino own library

// USB gamepad library
#include <PsxControllerBitBang.h> // External library
#include <Joystick.h>             // External library

// Device Firmware identifier
#define IDENTIFIER "MCDINO"   // MemCARDuino
#define VERSION 0x09          // Firmware version byte (Major.Minor)

// Commands
#define GETID 0xA0            // Get identifier
#define GETVER 0xA1           // Get firmware version
#define MCREAD 0xA2           // Memory Card Read (frame)
#define MCWRITE 0xA3          // Memory Card Write (frame)

// PocketStation commands
#define PSINFO  0xB0          // PocketStation info dump
#define PSBIOS  0xB1          // PocketStation BIOS dump
#define PSTIME  0xB2          // Set PocketStation date and time

// Test commands
#define TEST 0x54             // ASCII 'T' - test command

// Responses
#define ERROR 0xE0            // Invalid command received (error)

// Memory Card Responses
#define RW_NO_CARD 0xFF       // No Memory Card connected
#define RW_GOOD 0x47          // Good

// Misc
#define MAX_RETRY_COUNT 5     // Number of retries before fallback

#ifndef ICACHE_RAM_ATTR
#define ICACHE_RAM_ATTR
#endif

const byte LedRead    = A4; // A4---[470R]---(+LED-)---GND
const byte LedWrite   = A5; // A5---[470R]---(+LED-)---GND
const byte CardAckPin = 12; // Ack
const byte CardAttPin =  2; // Att
const byte CmndPin    = 16; // MOSI
const byte DataPin    = 14; // MISO 14---[4.7K]---3.3V
const byte ClockPin   = 15; // SCK
const byte JoyAttPin  = 10; // D10

const unsigned long POLLING_INTERVAL = 1000U / 50U;

#define JOYSTICK_COUNT 1  // Single PSX controller

// Declare Joystick array for one controller
Joystick_ Joystick[JOYSTICK_COUNT] = {
Joystick_(
  0x03, 
  JOYSTICK_TYPE_JOYSTICK, 
  16, 
  0, 
  true, 
  true, 
  false, 
  true, 
  true, 
  false, 
  false, 
  false, 
  false, 
  false, 
  false)
};

PsxControllerBitBang<JoyAttPin, CmndPin, DataPin, ClockPin> psx1;

#define dstart(...)
#define debug(...)
#define debugln(...)

boolean haveController1 = false;

volatile int state = HIGH;
bool CompatibleMode = false;
bool SlowSPIMode = false;
byte ReadData[128];
int failedRWCount = 0;

// Helper functions to open and close hardware SPI safely
void enableHardwareSPI() {
  digitalWrite(JoyAttPin, HIGH); // Force Gamepad CS HIGH to prevent bus noise
  SPI.begin();
  SPI.beginTransaction(SPISettings(125000, LSBFIRST, SPI_MODE3));
}

void disableHardwareSPI() {
  SPI.end(); // Release pins back to software GPIO for bit-banging
}

// Set up pins for communication
void PinSetup(){
  pinMode(LedRead, OUTPUT);
  pinMode(LedWrite, OUTPUT);
  digitalWrite(LedRead, LOW);
  digitalWrite(LedWrite, LOW);
  
  pinMode(CardAttPin, OUTPUT);
  pinMode(JoyAttPin, OUTPUT);
  digitalWrite(CardAttPin, HIGH);
  digitalWrite(JoyAttPin, HIGH);

  pinMode(CardAckPin, INPUT_PULLUP);

  // Enable pullup on MISO Data line
#if defined (__AVR_ATmega32U4__)
  PORTB = (1<<PB3);
#endif

  attachInterrupt(digitalPinToInterrupt(CardAckPin), ACK, RISING);
}

// Acknowledge routine
ICACHE_RAM_ATTR void ACK()
{
  state = !state;
}

// Send a command to PlayStation port using SPI
byte SendCommand(byte CommandByte, int Timeout, int Delay)
{
    if(!CompatibleMode) Timeout = 3000;
    state = HIGH;

    if (Delay > 0) 
    {
      delayMicroseconds(Delay);
    }

    byte data = SPI.transfer(CommandByte);

    while(state == HIGH)
    {
      Timeout--;
      delayMicroseconds(1);
      if(Timeout == 0){
        CompatibleMode = true;
        break;
      }
    }
    delayMicroseconds(20);

  return data;
}

void AnalyzeStatus(byte status){
  if(status == RW_GOOD) return;
}

// Read a frame from Memory Card
void ReadFrame(unsigned int Address)
{
  enableHardwareSPI();

  digitalWrite(LedRead, HIGH);
  byte AddressMSB = Address & 0xFF;
  byte AddressLSB = (Address >> 8) & 0xFF;
  byte StatusByte = 0;

  CompatibleMode = false;

  digitalWrite(CardAttPin, LOW);
  delayMicroseconds(20);

  SendCommand(0x81, 500, 70);
  SendCommand(0x52, 500, 45);
  SendCommand(0x00, 500, 45);
  SendCommand(0x00, 500, 45);
  SendCommand(AddressMSB, 500, 45);
  SendCommand(AddressLSB, 500, 45);
  SendCommand(0x00, 2800, 45);
  SendCommand(0x00, 2800, 0);
  SendCommand(0x00, 2800, 0);
  SendCommand(0x00, 2800, 0);

  for (int i = 0; i < 128; i++)
  {
    Serial.write(SendCommand(0x00, 150, 0));
  }

  Serial.write(SendCommand(0x00, 500, 0));
  StatusByte = SendCommand(0x00, 500, 0);

  Serial.write(StatusByte);

  digitalWrite(CardAttPin, HIGH);

  AnalyzeStatus(StatusByte);
  
  digitalWrite(LedRead, LOW);

  disableHardwareSPI();
}

// Write a frame to Memory Card
void WriteFrame(unsigned int Address){
  
  digitalWrite(LedWrite, HIGH);
  byte AddressMSB = Address & 0xFF;
  byte AddressLSB = (Address >> 8) & 0xFF;
  int DelayCounter = 30;

  CompatibleMode = false;

  for (int i = 0; i < 128; i++)
  {
    while(!Serial.available())
    {
      DelayCounter--;
      if(DelayCounter == 0){
        digitalWrite(LedWrite, LOW);
        return;
      }
      delay(1);
    }
    ReadData[i] = Serial.read();
  }

  enableHardwareSPI();

  digitalWrite(CardAttPin, LOW);
  delayMicroseconds(20);

  SendCommand(0x81, 300, 45);
  SendCommand(0x57, 300, 45);
  SendCommand(0x00, 300, 45);
  SendCommand(0x00, 300, 45);
  SendCommand(AddressMSB, 300, 45);
  SendCommand(AddressLSB, 300, 45);

  for (int i = 0; i < 128; i++)
  {
    SendCommand(ReadData[i], 150, 0);
  }

  SendCommand(Serial.read(), 200, 0);
  SendCommand(0x00, 200, 0);
  SendCommand(0x00, 200, 0);
  Serial.write(SendCommand(0x00, 0, 0));

  delayMicroseconds(500);

  digitalWrite(CardAttPin, HIGH);
  
  digitalWrite(LedWrite, LOW);

  disableHardwareSPI();
}

// Get info from PocketStation
void PSInfo(){
  enableHardwareSPI();

  digitalWrite(CardAttPin, LOW);
  delayMicroseconds(20);

  SendCommand(0x81, 300, 45);
  SendCommand(0x5A, 300, 45);
  Serial.write(SendCommand(0x0, 300, 45));

  for(int i=0; i < 0x12; i++){
    Serial.write(SendCommand(0x00, 300, 45));
  }

  digitalWrite(CardAttPin, HIGH);

  disableHardwareSPI();
}

// Dump 16KB BIOS in parts
void PSBios(byte partNum){
  byte paramSize = 0;
  byte dataSize = 0;
  unsigned short address = partNum * 128;

  enableHardwareSPI();

  digitalWrite(CardAttPin, LOW);
  delayMicroseconds(20);

  SendCommand(0x81, 300, 45);
  SendCommand(0x5B, 300, 45);
  SendCommand(0x01, 300, 45);

  paramSize = SendCommand(0x00, 300, 45);
  Serial.write(paramSize);

  if(paramSize != 0x05){
    digitalWrite(CardAttPin, HIGH);
    disableHardwareSPI();
    return;
  }

  SendCommand(address & 0xFF, 300, 45);
  SendCommand(address >> 8, 300, 45);
  SendCommand(0x00, 300, 45);
  SendCommand(0x04, 300, 45);

  SendCommand(0x80, 300, 45);

  dataSize = SendCommand(0x00, 300, 45);

  Serial.write(dataSize);

  if(dataSize != 0x80){
    digitalWrite(CardAttPin, HIGH);
    disableHardwareSPI();
    return;
  }

  for(int i = 0; i < 128; i++){
    Serial.write(SendCommand(0x80, 300, 45));
  }

  SendCommand(0x00, 300, 45);

  Serial.write(RW_GOOD);

  digitalWrite(CardAttPin, HIGH);

  disableHardwareSPI();
}

// Set PocketStation Data and Time
void PSTime(){
  byte paramSize = 0;
  byte dataSize = 0;
  int DelayCounter = 30;

  enableHardwareSPI();

  digitalWrite(CardAttPin, LOW);
  delayMicroseconds(20);

  SendCommand(0x81, 300, 45);
  SendCommand(0x5C, 300, 45);
  SendCommand(0x00, 300, 45);

  paramSize = SendCommand(0x00, 300, 45);
  Serial.write(paramSize);

  if(paramSize != 0x00){
    digitalWrite(CardAttPin, HIGH);
    disableHardwareSPI();
    return;
  }

  dataSize = SendCommand(0x00, 300, 45);

  if(dataSize != 0x08){
    digitalWrite(CardAttPin, HIGH);
    disableHardwareSPI();
    return;
  }

  Serial.write(dataSize);

  for (int i = 0; i < 8; i++)
  {
    while(!Serial.available())
    {
      DelayCounter--;
      if(DelayCounter == 0){
        digitalWrite(CardAttPin, HIGH);
        disableHardwareSPI();
        return;
      }
      delay(1);
    }

    ReadData[i] = Serial.read();
  }

  for (int i = 0; i < 8; i++)
  {
    SendCommand(ReadData[i], 300, 45);
  }

  SendCommand(0x00, 300, 45);

  Serial.write(RW_GOOD);

  digitalWrite(CardAttPin, HIGH);

  disableHardwareSPI();
}

void setup(){
  Serial.begin(115200);
  PinSetup();
  gamepadsetup();
}

void loop(){
  // Handle Memory Card Serial commands when available
  if(Serial.available() > 0)
  {
    switch(Serial.read())
    {
      default:
        Serial.write(ERROR);
        break;

      case GETID:
        Serial.write(IDENTIFIER);
        break;

      case GETVER:
        Serial.write(VERSION);
        break;

      case TEST:
        Serial.print(IDENTIFIER);
        Serial.print(" ");
        Serial.print(VERSION >> 4);
        Serial.print(".");
        Serial.println(VERSION & 0xF, HEX);
        break;

      case PSINFO:
        PSInfo();
        break;

      case PSBIOS:
        delay(5);
        PSBios(Serial.read());
        break;

      case PSTIME:
        PSTime();
        break;

      case MCREAD:
        delay(5);
        ReadFrame(Serial.read() | Serial.read() << 8);
        break;

      case MCWRITE:
        delay(5);
        WriteFrame(Serial.read() | Serial.read() << 8);
        break;
    }
  }

  // Poll gamepad bit-bang routine only when memory card serial requests are idle
  gamepadloop();
}

void gamepadsetup() {
  for (int i = 0; i < JOYSTICK_COUNT; i++) {
    Joystick[i].begin(false);
    Joystick[i].setXAxisRange(ANALOG_MIN_VALUE, ANALOG_MAX_VALUE);
    Joystick[i].setYAxisRange(ANALOG_MIN_VALUE, ANALOG_MAX_VALUE);
    Joystick[i].setRxAxisRange(ANALOG_MIN_VALUE, ANALOG_MAX_VALUE);
    Joystick[i].setRyAxisRange(ANALOG_MIN_VALUE, ANALOG_MAX_VALUE);
  }

  pinMode(LED_BUILTIN, OUTPUT);
  dstart(115200);
  debugln(F("Ready!"));
}

void gamepadloop() {
  static unsigned long last = 0;

  if (millis() - last >= POLLING_INTERVAL) {
    last = millis();

    if (!haveController1) {
      if (psx1.begin()) {
        debugln(F("Controller 1 found!"));
        if (!psx1.enterConfigMode()) {
          debugln(F("Cannot enter config mode"));
        } else {
          if (!psx1.enableAnalogSticks()) {
            debugln(F("Cannot enable analog sticks"));
          }
          if (!psx1.exitConfigMode()) {
            debugln(F("Cannot exit config mode"));
          }
        }
        haveController1 = true;
      }
    } else {
      if (!psx1.read()) {
        debugln(F("Controller 1 lost"));
        haveController1 = false;
      } else {
        byte x, y;

        Joystick[0].setButton(0, psx1.buttonPressed(PSB_TRIANGLE));
        Joystick[0].setButton(1, psx1.buttonPressed(PSB_CROSS));
        Joystick[0].setButton(2, psx1.buttonPressed(PSB_SQUARE));
        Joystick[0].setButton(3, psx1.buttonPressed(PSB_CIRCLE));
        Joystick[0].setButton(4, psx1.buttonPressed(PSB_L1));
        Joystick[0].setButton(5, psx1.buttonPressed(PSB_R1));
        Joystick[0].setButton(6, psx1.buttonPressed(PSB_L2));
        Joystick[0].setButton(7, psx1.buttonPressed(PSB_R2));
        Joystick[0].setButton(8, psx1.buttonPressed(PSB_SELECT));
        Joystick[0].setButton(9, psx1.buttonPressed(PSB_START));
        Joystick[0].setButton(10, psx1.buttonPressed(PSB_L3));
        Joystick[0].setButton(11, psx1.buttonPressed(PSB_R3));
        Joystick[0].setButton(12, psx1.buttonPressed(PSB_PAD_UP));
        Joystick[0].setButton(13, psx1.buttonPressed(PSB_PAD_DOWN));
        Joystick[0].setButton(14, psx1.buttonPressed(PSB_PAD_LEFT));
        Joystick[0].setButton(15, psx1.buttonPressed(PSB_PAD_RIGHT));

        if (psx1.getLeftAnalog(x, y)) {
          Joystick[0].setXAxis(x);
          Joystick[0].setYAxis(y);
        }
        if (psx1.getRightAnalog(x, y)) {
          Joystick[0].setRxAxis(x);
          Joystick[0].setRyAxis(y);
        }

        Joystick[0].sendState();
      }
    }
  }
}
