// ===================================================================================
// Project:   USB Multimeter based on ATtiny85
// Version:   v1.0
// Year:      2024
// Author:    RG4tech (original author project: Stefan Wagner)
// Original project : https://github.com/wagiminator/ATtiny85-USB-Tester
// Github:    https://github.com/rg4-tech
// EasyEDA:   https://oshwlab.com/gabriva?tab=project&page=1
// License:   http://creativecommons.org/licenses/by-sa/3.0/
// ===================================================================================
//
// Description:
// ------------
// Simple USB Multimeter based on ATtiny85 and INA219. The device
// measures voltage, battery and continuity test and displays the values
// on an OLED screen. You can switch between different screens by pressing the
// SET button.
//
// References:
// -----------
// The I²C OLED implementation is based on TinyOLEDdemo
// https://github.com/wagiminator/ATtiny13-TinyOLEDdemo
//
// Wiring:
// -------
//                          +-\/-+
//       --- RST ADC0 PB5  1|°   |8  Vcc
//   SET ------- ADC3 PB3  2|    |7  PB2 ADC1 -------- OLED/INA SCK
//   BUZZER ---- ADC2 PB4  3|    |6  PB1 AIN1 OC0B --- 
//                    GND  4|    |5  PB0 AIN0 OC0A --- OLED/INA SDA
//                          +----+
//
// Compilation Settings:
// ---------------------
// Core:    ATtinyCore (https://github.com/SpenceKonde/ATTinyCore)
// Board:   ATtiny25/45/85 (No bootloader)
// Chip:    ATtiny85
// Clock:   8 MHz (internal)
// B.O.D.:  2.7V
//
// Leave the rest on default settings. Don't forget to "Burn bootloader"!
// No Arduino core functions or libraries are used.
//
// Note: The internal oscillator may need to be calibrated for precise
//       energy and capacity calculation.
//
// Fuse settings: -U lfuse:w:0x62:m -U hfuse:w:0xd5:m -U efuse:w:0xff:m 
//
// Operating Instructions:
// -----------------------
// Connect the device between a power supply and a consumer.
// Use the SET button to switch between the different screens.


// ===================================================================================
// Libraries and Definitions
// ===================================================================================

// Oscillator calibration value (uncomment and set if necessary)
// #define OSCCAL_VAL  0x48

// Libraries
#include <avr/io.h>             // for GPIO
#include <avr/pgmspace.h>       // for data stored in program memory
// #include <avr/interrupt.h>      // for interrupts
//#include <util/delay.h>         // for delays

// Pin definitions
#define PIN_SDA     PB0         // I2C serial data pin
#define PIN_SCL     PB2         // I2C serial clock pin
#define PIN_SET     PB4         // SET button
#define PIN_BUZZER  PB3         // Out buzzer

// ===================================================================================
// I2C Master Implementation
// ===================================================================================

// I2C macros
#define I2C_SDA_HIGH()  DDRB &= ~(1<<PIN_SDA)   // release SDA   -> pulled HIGH by resistor
#define I2C_SDA_LOW()   DDRB |=  (1<<PIN_SDA)   // SDA as output -> pulled LOW  by MCU
#define I2C_SCL_HIGH()  DDRB &= ~(1<<PIN_SCL)   // release SCL   -> pulled HIGH by resistor
#define I2C_SCL_LOW()   DDRB |=  (1<<PIN_SCL)   // SCL as output -> pulled LOW  by MCU
#define I2C_SDA_READ()  (PINB &  (1<<PIN_SDA))  // read SDA line
#define I2C_CLOCKOUT()  I2C_SCL_HIGH();I2C_SCL_LOW()  // clock out

// I2C transmit one data byte to the slave, ignore ACK bit, no clock stretching allowed
void I2C_write(uint8_t data) {
  for(uint8_t i=8; i; i--, data<<=1) {          // transmit 8 bits, MSB first
    (data&0x80)?I2C_SDA_HIGH():I2C_SDA_LOW();   // SDA depending on bit
    I2C_CLOCKOUT();                             // clock out -> slave reads the bit
  }
  I2C_SDA_HIGH();                               // release SDA for ACK bit of slave
  I2C_CLOCKOUT();                               // 9th clock pulse is for the ignored ACK bit
}

// I2C start transmission
void I2C_start(uint8_t addr) {
  I2C_SDA_LOW();                                // start condition: SDA goes LOW first
  I2C_SCL_LOW();                                // start condition: SCL goes LOW second
  I2C_write(addr);                              // send slave address
}

// I2C restart transmission
void I2C_restart(uint8_t addr) {
  I2C_SDA_HIGH();                               // prepare SDA for HIGH to LOW transition
  I2C_SCL_HIGH();                               // restart condition: clock HIGH
  I2C_start(addr);                              // start again
}

// I2C stop transmission
void I2C_stop(void) {
  I2C_SDA_LOW();                                // prepare SDA for LOW to HIGH transition
  I2C_SCL_HIGH();                               // stop condition: SCL goes HIGH first
  I2C_SDA_HIGH();                               // stop condition: SDA goes HIGH second
}

// I2C receive one data byte from the slave (ack=0 for last byte, ack>0 if more bytes to follow)
uint8_t I2C_read(uint8_t ack) {
  uint8_t data = 0;                             // variable for the received byte
  I2C_SDA_HIGH();                               // release SDA -> will be toggled by slave
  for(uint8_t i=8; i; i--) {                    // receive 8 bits
    data <<= 1;                                 // bits shifted in right (MSB first)
    I2C_SCL_HIGH();                             // clock HIGH
    if(I2C_SDA_READ()) data |= 1;               // read bit
    I2C_SCL_LOW();                              // clock LOW -> slave prepares next bit
  }
  if(ack) I2C_SDA_LOW();                        // pull SDA LOW to acknowledge (ACK)
  I2C_CLOCKOUT();                               // clock out -> slave reads ACK bit
  return data;                                  // return the received byte
}

// ===================================================================================
// OLED Implementation (SSD1306 128x32)
// ===================================================================================

// OLED definitions
#define OLED_ADDR       0x78    // OLED write address
#define OLED_CMD_MODE   0x00    // set command mode
#define OLED_DAT_MODE   0x40    // set data mode
#define OLED_INIT_LEN   11      // 9: no screen flip, 11: screen flip

// OLED init settings
const uint8_t OLED_INIT_CMD[] PROGMEM = {
  0xA8, 0x1F,                   // set multiplex for 128x32
  0x20, 0x01,                   // set vertical memory addressing mode
  0xDA, 0x02,                   // set COM pins hardware configuration to sequential
  0x8D, 0x14,                   // enable charge pump
  0xAF,                         // switch on OLED
  0xA1, 0xC8                    // flip the screen
};

// OLED 5x16 font
// create font: https://dotmatrixtool.com/ 
const uint8_t OLED_FONT[] PROGMEM = {
  0x7C, 0x1F, 0x02, 0x20, 0x02, 0x20, 0x02, 0x20, 0x7C, 0x1F, // 0  0
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x1F, // 1  1
  0x00, 0x1F, 0x82, 0x20, 0x82, 0x20, 0x82, 0x20, 0x7C, 0x00, // 2  2
  0x00, 0x00, 0x82, 0x20, 0x82, 0x20, 0x82, 0x20, 0x7C, 0x1F, // 3  3
  0x7C, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x7C, 0x1F, // 4  4
  0x7C, 0x00, 0x82, 0x20, 0x82, 0x20, 0x82, 0x20, 0x00, 0x1F, // 5  5
  0x7C, 0x1F, 0x82, 0x20, 0x82, 0x20, 0x82, 0x20, 0x00, 0x1F, // 6  6
  0x0C, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x7C, 0x1F, // 7  7
  0x7C, 0x1F, 0x82, 0x20, 0x82, 0x20, 0x82, 0x20, 0x7C, 0x1F, // 8  8
  0x7C, 0x00, 0x82, 0x20, 0x82, 0x20, 0x82, 0x20, 0x7C, 0x1F, // 9  9
  0xF0, 0x3F, 0x8C, 0x00, 0x82, 0x00, 0x8C, 0x00, 0xF0, 0x3F, // A 10
  0xFE, 0x07, 0x00, 0x18, 0x00, 0x20, 0x00, 0x18, 0xFE, 0x07, // V 11
  0xfe, 0x00, 0x00, 0x0f, 0xe0, 0x38, 0x00, 0x0f, 0xfe, 0x00, // W 12
  0xFE, 0x3F, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x3F, // h 13
  0x80, 0x3F, 0x80, 0x00, 0x80, 0x3F, 0x80, 0x00, 0x00, 0x3F, // m 14
  0x00, 0x00, 0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, // . 15
  0x00, 0x00, 0x30, 0x06, 0x30, 0x06, 0x00, 0x00, 0x00, 0x00, // : 16
  0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, // - 17
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   18
  0xfe, 0x3f, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, // L 19
  0xc0, 0x04, 0xc0, 0x05, 0xff, 0xff, 0xc0, 0x05, 0xc0, 0x04, // Diode 20
  0xFC, 0x3F, 0xFC, 0x3F, 0xFE, 0x3F, 0xFC, 0x3F, 0xFC, 0x3F,  // BATT 21
  0x00, 0x08, 0x00, 0x24, 0x00, 0x92, 0x00, 0x24, 0x00, 0x08,  // BIP 22
  0xfc, 0x20, 0x02, 0x3f, 0x02, 0x00, 0x02, 0x3f, 0xfc, 0x20,  // Omega 23
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff   // full 24
  //0xfe, 0x3f, 0x82, 0x20, 0x82, 0x20, 0x82, 0x20, 0x7c, 0x1f, // B 19
  //0x02, 0x20, 0x02, 0x20, 0xfe, 0x3f, 0x02, 0x20, 0x02, 0x20, // I 20
  //0xfe, 0x3f, 0x82, 0x00, 0x82, 0x00, 0x82, 0x00, 0x7c, 0x00, // P 22
};

// Character definitions
#define DECIMAL 15
#define SPACE   18

// OLED BCD conversion array
const uint16_t DIVIDER[] PROGMEM = {10000, 1000, 100, 10, 1};

// OLED init function
void OLED_init(void) {
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_CMD_MODE);                     // set command mode
  for(uint8_t i=0; i<OLED_INIT_LEN; i++) 
    I2C_write(pgm_read_byte(&OLED_INIT_CMD[i]));// send the command bytes
  I2C_stop();                                   // stop transmission
}

// OLED set the cursor
void OLED_setCursor(uint8_t xpos, uint8_t ypos) {
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_CMD_MODE);                     // set command mode
  I2C_write(0x22);                              // command for min/max page
  I2C_write(ypos); I2C_write(ypos+1);           // min: ypos; max: ypos+1
  I2C_write(xpos & 0x0F);                       // set low nibble of start column
  I2C_write(0x10 | (xpos >> 4));                // set high nibble of start column
  I2C_write(0xB0 | (ypos));                     // set start page
  I2C_stop();                                   // stop transmission
}

// OLED clear screen
void OLED_clearScreen(void) {
  OLED_setCursor(0, 0);                         // set cursor at upper half
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_DAT_MODE);                     // set data mode
  uint8_t i = 0;                                // count variable
  do {I2C_write(0x00);} while(--i);             // clear upper half
  I2C_stop();                                   // stop transmission
  OLED_setCursor(0, 2);                         // set cursor at lower half
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_DAT_MODE);                     // set data mode
  do {I2C_write(0x00);} while(--i);             // clear upper half
  I2C_stop();                                   // stop transmission
}

// OLED plot a character
void OLED_plotChar(uint8_t ch) {
  ch = (ch << 1) + (ch << 3);                   // calculate position of character in font array
  I2C_write(0x00); I2C_write(0x00);             // print spacing between characters
  I2C_write(0x00); I2C_write(0x00);             // print spacing between characters
  for(uint8_t i=10; i; i--) I2C_write(pgm_read_byte(&OLED_FONT[ch++])); // print character
}

// OLED print a character
void OLED_printChar(uint8_t ch) {
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_DAT_MODE);                     // set data mode
  OLED_plotChar(ch);                            // plot the character
  I2C_stop();                                   // stop transmission
}

// OLED print a string from program memory; terminator: 255
void OLED_printPrg(const uint8_t* p) {
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_DAT_MODE);                     // set data mode
  uint8_t ch = pgm_read_byte(p);                // read first character from program memory
  while(ch < 255) {                             // repeat until string terminator
    OLED_plotChar(ch);                          // plot character on OLED
    ch = pgm_read_byte(++p);                    // read next character
  }
  I2C_stop();                                   // stop transmission
}

// OLED print 16-bit value as 5-digit decimal (BCD conversion by substraction method)
void OLED_printDec5(uint16_t value) {
  uint8_t leadflag = 0;                         // flag for leading spaces
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_DAT_MODE);                     // set data mode
  for(uint8_t digit = 0; digit < 5; digit++) {  // 5 digits
    uint8_t digitval = 0;                       // start with digit value 0
    uint16_t divider = pgm_read_word(&DIVIDER[digit]); // current divider
    while(value >= divider) {                   // if current divider fits into the value
      leadflag = 1;                             // end of leading spaces
      digitval++;                               // increase digit value
      value -= divider;                         // decrease value by divider
    }
    if(leadflag || (digit == 4)) OLED_plotChar(digitval); // print the digit
    else OLED_plotChar(SPACE);                  // or print leading space
  }
  I2C_stop();                                   // stop transmission
}

// OLED print 16-bit value as 3-digit decimal (BCD conversion by substraction method)
void OLED_printDec3(uint16_t value) {
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_DAT_MODE);                     // set data mode
  for(uint8_t digit = 2; digit < 5; digit++) {  // 3 digits
    uint8_t digitval = 0;                       // start with digit value 0
    uint16_t divider = pgm_read_word(&DIVIDER[digit]); // current divider
    while(value >= divider) {                   // if current divider fits into the value
      digitval++;                               // increase digit value
      value -= divider;                         // decrease value by divider
    }
    OLED_plotChar(digitval);                    // print the digit
  }
  I2C_stop();                                   // stop transmission
}

// OLED print 16-bit value as 1-digit decimal (BCD conversion by substraction method)
void OLED_printDec1(uint16_t value) {
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_DAT_MODE);                     // set data mode
  for(uint8_t digit = 4; digit < 5; digit++) {  // 3 digits
    uint8_t digitval = 0;                       // start with digit value 0
    uint16_t divider = pgm_read_word(&DIVIDER[digit]); // current divider
    while(value >= divider) {                   // if current divider fits into the value
      digitval++;                               // increase digit value
      value -= divider;                         // decrease value by divider
    }
    OLED_plotChar(digitval);                    // print the digit
  }
  I2C_stop();                                   // stop transmission
}

// OLED print 8-bit value as 2-digit decimal (BCD conversion by substraction method)
//void OLED_printDec8(uint8_t value) {
//  I2C_start(OLED_ADDR);                         // start transmission to OLED
//  I2C_write(OLED_DAT_MODE);                     // set data mode
//  uint8_t digitval = 0;                         // start with digit value 0
//  while(value >= 10) {                          // if current divider fits into the value
//    digitval++;                                 // increase digit value
//    value -= 10;                                // decrease value by divider
//  }
//  OLED_plotChar(digitval);                      // print first digit
//  OLED_plotChar(value);                         // print second digit
//  I2C_stop();                                   // stop transmission
//}


// ===================================================================================
// INA219 Implementation
// ===================================================================================

// INA219 register values
#define INA_ADDR        0x80                    // I2C write address of INA219
#define INA_CONFIG      0b0000011111111111      // INA config register according to datasheet
#define INA_CALIB       4010                    // INA calibration register according to R_SHUNT (= 1ohm)
#define INA_REG_CONFIG  0x00                    // INA configuration register address
#define INA_REG_CALIB   0x05                    // INA calibration register address
#define INA_REG_SHUNT   0x01                    // INA shunt voltage register address
#define INA_REG_VOLTAGE 0x02                    // INA bus voltage register address
#define INA_REG_POWER   0x03                    // INA power register address
#define INA_REG_CURRENT 0x04                    // INA current register address

// INA219 write a register value
void INA_write(uint8_t reg, uint16_t value) {
  I2C_start(INA_ADDR);                          // start transmission to INA219
  I2C_write(reg);                               // write register address
  I2C_write(value >> 8);                        // write register content high byte
  I2C_write(value);                             // write register content low  byte
  I2C_stop();                                   // stop transmission
}

// INA219 read a register
uint16_t INA_read(uint8_t reg) {
  uint16_t result;                              // result variable
  I2C_start(INA_ADDR);                          // start transmission to INA219
  I2C_write(reg);                               // write register address
  I2C_restart(INA_ADDR | 0x01);                 // restart for reading
  result = (uint16_t)(I2C_read(1) << 8) | I2C_read(0);  // read register content
  I2C_stop();                                   // stop transmission
  return result;                                // return result
}

// INA219 write inital configuration and calibration values
void INA_init(void) {
  INA_write(INA_REG_CONFIG, INA_CONFIG);        // write INA219 configuration
  INA_write(INA_REG_CALIB,  INA_CALIB);         // write INA219 calibration
}

// INA219 read voltage
uint16_t INA_readVoltage(void) {
  return((INA_read(INA_REG_VOLTAGE) >> 1) & 0xFFFC);
}

// INA219 read sensor values
uint16_t INA_readCurrent(void) {
  uint16_t result =  INA_read(INA_REG_CURRENT); // read current from INA
  if(result > 32767) result = 0;                // ignore negative currents
  return result;                                // return result
}

// ===================================================================================
// Millis Counter Implementation for Timer0
// ===================================================================================

volatile uint32_t MIL_counter = 0;              // millis counter variable

// Init millis counter
void MIL_init(void) {
  OCR0A  = 124;                                 // TOP: 124 = 1000kHz / (8 * 1kHz) - 1
  TCCR0A = (1<<WGM01);                          // timer0 CTC mode
  TCCR0B = (1<<CS01);                           // start timer0 with prescaler 8
  TIMSK  = (1<<OCIE0A);                         // enable output compare match interrupt
}

// Read millis counter
uint32_t MIL_read(void) {
  cli();                                        // disable interrupt for atomic read
  uint32_t result = MIL_counter;                // read millis counter
  sei();                                        // enable interrupts
  return result;                                // return millis counter value
}

// Timer0 compare match A interrupt service routine (every millisecond)
ISR(TIM0_COMPA_vect) {
  MIL_counter++;                                // increase millis counter
}

// ===================================================================================
// Main Function
// ===================================================================================

// Some "strings"
const uint8_t mA[]  PROGMEM = { 14, 10, 18, 255 };  // "mA "
const uint8_t mV[]  PROGMEM = { 14, 11, 18, 255 };  // "mV "
const uint8_t V[]   PROGMEM = { 11, 18, 18, 255 };  // "V  "
const uint8_t mW[]  PROGMEM = { 14, 12, 18, 255 };  // "mW "
const uint8_t mAh[] PROGMEM = { 14, 10, 13, 255 };  // "mAh"
const uint8_t mWh[] PROGMEM = { 14, 12, 13, 255 };  // "mWh"
const uint8_t Version[] PROGMEM = { 11, 1, 15, 0, 255 };  // FW Version "V1.0"
//const uint8_t Test[] PROGMEM = {0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7, 255 };  // Test display

// Main function
int main(void) {
  // Local variables
  uint16_t  voltage, current;                 // voltage in mV, current in mA
  uint16_t  res;                              // res in ohm
  uint16_t  volt_offset = 8;                  // calibration voltage
  uint8_t   primescreen = 0;                  // screen selection flag
  uint8_t   lastbutton  = 1;                  // button flag (0: button pressed)
  uint32_t  lastmillis, nowmillis;            // for timing calculation in millis
  uint32_t  bip_duration = 500;               // duration BIP for diode test

  // Set oscillator calibration value
  #ifdef OSCCAL_VAL
    OSCCAL = OSCCAL_VAL;                        // set the value if defined above
  #endif

  // Setup
  PORTB |= (1<<PIN_SET);                        // pullup for set button
  MIL_init();                                   // init millis counter
  INA_init();                                   // init INA219
  OLED_init();                                  // init OLED
  OLED_clearScreen();                           // clear screen
  lastmillis = MIL_read();                      // read millis counter
  pinMode(PIN_BUZZER, OUTPUT);

  // Loop
  while(1) {
    // timings
    nowmillis   = MIL_read();                   // read millis counter

    // Read sensor values
    voltage = INA_readVoltage();                // read voltage in mV from INA219
    current = INA_readCurrent();                // read current in mA from INA219
    res = (uint32_t)(voltage - volt_offset) * 1000 / current; // Calculate resistance value
    
    // Check SET button and set screen flag accordingly
    if(PINB & (1<<PIN_SET)) lastbutton = 0;
    else if(!lastbutton) {
      if(++primescreen > 3) primescreen = 0;
      OLED_clearScreen();
      lastbutton  = 1;
    }

    // Display values on the OLED

    switch(primescreen) {
      case 0:   // BIP (continuity tester)
                voltage = voltage - volt_offset; // calibration value read
                
                if(voltage <= 50) PORTB |= (1<<PIN_BUZZER);      // ON Buzzer
                else if(voltage > 50) PORTB &= ~(1<<PIN_BUZZER); // OFF Buzzer
                
                OLED_setCursor(0,0);
                OLED_printDec5(voltage / 1000); OLED_printChar(DECIMAL);
                OLED_printDec3(voltage % 1000); OLED_printPrg(V);

                //OLED_setCursor(0,2);
                //OLED_printDec5(current); OLED_printPrg(mA);

                OLED_setCursor(0,2);
                if(voltage < 2850) {
                  OLED_printDec5(res / 10); OLED_printChar(DECIMAL);
                  OLED_printDec1(res % 10); 
                  OLED_printChar(23); // Omega
                }
                else {
                  OLED_printChar(SPACE); OLED_printChar(SPACE); OLED_printChar(SPACE); OLED_printChar(SPACE);
                  OLED_printChar(19);
                  OLED_printChar(19);
                  OLED_printChar(19);
                  OLED_printChar(23); // Omega
                }

                if(voltage <= 50) { // vertical bar
                  OLED_setCursor(100,0);
                  OLED_printChar(24);
                  OLED_setCursor(100,1);
                  OLED_printChar(24);
                  OLED_setCursor(100,2);
                  OLED_printChar(24);
                }
                else {
                  OLED_setCursor(100,0);
                  OLED_printChar(SPACE);
                  OLED_setCursor(100,1);
                  OLED_printChar(SPACE);
                  OLED_setCursor(100,2);
                  OLED_printChar(SPACE);
                }
                
                OLED_setCursor(120,2);
                OLED_printChar(22); // BIP simbol

                break;        
      case 1:   // Battery and Voltmeter tester
                PORTB &= ~(1<<PIN_BUZZER); // OFF Buzzer
                if(voltage < 17) voltage = 0;
                
                OLED_setCursor(0,0);
                OLED_printDec5(voltage / 1000); OLED_printChar(DECIMAL);
                OLED_printDec3(voltage % 1000); OLED_printPrg(V);

                OLED_setCursor(120,0);
                OLED_printChar(21); // battery symbol

                break;
      case 2:   // Diode tester
                voltage = voltage - volt_offset; // calibration value read

                if(voltage <= 50) PORTB |= (1<<PIN_BUZZER);      // ON Buzzer
                if(voltage > 200 && voltage <= 800) {
                  if(nowmillis - lastmillis >= bip_duration) {
                    lastmillis  = nowmillis;
                    PORTB |= (1<<PIN_BUZZER); // ON Buzzer
                  }
                  else PORTB &= ~(1<<PIN_BUZZER); // OFF Buzzer
                }
                else if((voltage > 50 && voltage < 200) || voltage >= 800){
                  PORTB &= ~(1<<PIN_BUZZER); // OFF Buzzer
                }
                
                OLED_setCursor(0,0);
                if(voltage < 2800) {
                  OLED_printDec5(voltage / 1000); OLED_printChar(DECIMAL);
                  OLED_printDec3(voltage % 1000); OLED_printPrg(V);
                }
                else {
                  OLED_printChar(SPACE); OLED_printChar(SPACE); OLED_printChar(SPACE); OLED_printChar(SPACE);
                  OLED_printChar(SPACE);
                  OLED_printChar(SPACE);
                  OLED_printChar(SPACE);
                  OLED_printChar(19);
                  OLED_printChar(19);
                }

                OLED_setCursor(120,1);
                OLED_printChar(20); // Diode

                break;
      case 3:   // Firmware version
                OLED_setCursor(50,1);
                OLED_printPrg(Version);
      default:  break;
    }
    _delay_ms(50);
  }
}
