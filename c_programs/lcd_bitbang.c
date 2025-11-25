#include <stdint.h>
#include <stdio.h>
#include <gpiod.h>
#include <unistd.h>
#include <string.h>

// --- UPDATED CONFIGURATION (For GPIO2_A0/A1) ---
const char *CHIP_NAME = "gpiochip2"; // Chip for GPIO2_Ax pins
const int SCL_PIN = 1;               // SCL is GPIO2_A1 (Physical pin 25) -> Line offset 1
const int SDA_PIN = 0;               // SDA is GPIO2_A0 (Physical pin 24) -> Line offset 0        // Common address, might be 0x3F
const int LCD_ADDRESS = 0x3F;

// Timing delay for I2C bus. Increase if you get garbled text.
#define I2C_DELAY_USEC 100

// --- LCD Command Codes ---
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_CURSORSHIFT 0x10
#define LCD_FUNCTIONSET 0x20
#define LCD_SETCGRAMADDR 0x40
#define LCD_SETDDRAMADDR 0x80

// Flags for display entry mode
#define LCD_ENTRYRIGHT 0x00
#define LCD_ENTRYLEFT 0x02
#define LCD_ENTRYSHIFTINCREMENT 0x01
#define LCD_ENTRYSHIFTDECREMENT 0x00

// Flags for display on/off control
#define LCD_DISPLAYON 0x04
#define LCD_DISPLAYOFF 0x00
#define LCD_CURSORON 0x02
#define LCD_CURSOROFF 0x00
#define LCD_BLINKON 0x01
#define LCD_BLINKOFF 0x00

// Flags for function set
#define LCD_8BITMODE 0x10
#define LCD_4BITMODE 0x00
#define LCD_2LINE 0x08
#define LCD_1LINE 0x00
#define LCD_5x10DOTS 0x04
#define LCD_5x8DOTS 0x00

// Flag for backlight
#define LCD_BACKLIGHT 0x08 // On
#define LCD_NOBACKLIGHT 0x00 // Off

// Global GPIO variables
struct gpiod_chip *chip;
struct gpiod_line *scl_line, *sda_line;
uint8_t _displayfunction;
uint8_t _displaycontrol;
uint8_t _displaymode;
uint8_t _backlightval = LCD_BACKLIGHT;

// --- Low-Level I2C Bit-Banging Functions ---

void i2c_delay() { usleep(I2C_DELAY_USEC); }

void set_sda(int val) { gpiod_line_set_value(sda_line, val); }
void set_scl(int val) { gpiod_line_set_value(scl_line, val); }

// I2C Start Condition: SDA goes from high to low while SCL is high
void i2c_start() {
    set_sda(1); i2c_delay();
    set_scl(1); i2c_delay();
    set_sda(0); i2c_delay();
    set_scl(0); i2c_delay();
}

// I2C Stop Condition: SDA goes from low to high while SCL is high
void i2c_stop() {
    set_sda(0); i2c_delay();
    set_scl(1); i2c_delay();
    set_sda(1); i2c_delay();
}

// Write a single bit to the I2C bus
void i2c_write_bit(int bit) {
    set_sda(bit);
    i2c_delay();
    set_scl(1);
    i2c_delay();
    set_scl(0);
}

// Write a byte and check for ACK
// Returns 0 on ACK, 1 on NACK
int i2c_write_byte(uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        i2c_write_bit((byte >> (7 - i)) & 1);
    }

    // Check for ACK
    gpiod_line_request_input(sda_line, "lcd_bitbang");
    i2c_delay();
    set_scl(1);
    i2c_delay();
    int ack = gpiod_line_get_value(sda_line);
    set_scl(0);
    gpiod_line_request_output(sda_line, "lcd_bitbang", 0);
    return ack;
}

// --- Mid-Level LCD I2C Functions ---

void lcd_pulse_enable(uint8_t data) {
    i2c_write_byte(data | 0x04); // En high
    i2c_delay();
    i2c_write_byte(data & ~0x04); // En low
    i2c_delay();
}

void lcd_write4bits(uint8_t value) {
    i2c_write_byte(value | _backlightval);
    lcd_pulse_enable(value | _backlightval);
}

void lcd_send(uint8_t value, uint8_t mode) {
    uint8_t high_nibble = value & 0xF0;
    uint8_t low_nibble = (value << 4) & 0xF0;
    lcd_write4bits(high_nibble | mode);
    lcd_write4bits(low_nibble | mode);
}

void lcd_command(uint8_t value) {
    lcd_send(value, 0); // Mode 0 for command
}

void lcd_write(uint8_t value) {
    lcd_send(value, 1); // Mode 1 for data
}


// --- High-Level User Functions ---

void lcd_clear() {
    lcd_command(LCD_CLEARDISPLAY);
    usleep(2000);
}

void lcd_home() {
    lcd_command(LCD_RETURNHOME);
    usleep(2000);
}

void lcd_set_cursor(uint8_t col, uint8_t row) {
    int row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    if (row > 1) {
        row = 1; // Assuming 2-line display
    }
    lcd_command(LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

void lcd_print(const char *str) {
    while (*str) {
        lcd_write(*str++);
    }
}

void lcd_init() {
    _displayfunction = LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS;

    // Wait for LCD to power up
    usleep(50000);

    // Initial sequence to put LCD in 4-bit mode
    i2c_start();
    lcd_write4bits(0x30); usleep(4500);
    lcd_write4bits(0x30); usleep(4500);
    lcd_write4bits(0x30); usleep(150);
    lcd_write4bits(0x20); // Set 4-bit mode
    i2c_stop();

    i2c_start();
    lcd_command(LCD_FUNCTIONSET | _displayfunction);

    _displaycontrol = LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF;
    lcd_command(LCD_DISPLAYCONTROL | _displaycontrol);

    lcd_clear();

    _displaymode = LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT;
    lcd_command(LCD_ENTRYMODESET | _displaymode);
    i2c_stop();
}

// --- Main Program ---

int main() {
    chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (!chip) {
        perror("gpiod_chip_open_by_name");
        return 1;
    }

    scl_line = gpiod_chip_get_line(chip, SCL_PIN);
    sda_line = gpiod_chip_get_line(chip, SDA_PIN);

    if (!scl_line || !sda_line) {
        fprintf(stderr, "Could not get GPIO lines\n");
        gpiod_chip_close(chip);
        return 1;
    }

    if (gpiod_line_request_output(scl_line, "lcd_bitbang", 0) < 0 ||
        gpiod_line_request_output(sda_line, "lcd_bitbang", 0) < 0) {
        perror("gpiod_line_request_output");
        gpiod_chip_close(chip);
        return 1;
    }

    printf("GPIOs requested. Initializing LCD...\n");

    lcd_init();

    printf("LCD Initialized. Displaying message.\n");

    i2c_start();
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("Hello, Luckfox!");
    lcd_set_cursor(0, 1);
    lcd_print("It is working!");
    i2c_stop();


    // Cleanup
    gpiod_line_release(scl_line);
    gpiod_line_release(sda_line);
    gpiod_chip_close(chip);

    printf("Done.\n");

    return 0;
}
