#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <string.h>
#include "lcd.h"

// --- I2C LCD Configuration ---
#define I2C_ADDR_DEFAULT 0x27 // The I2C address of your LCD
#define LCD_WIDTH 16          // Max characters per line

// --- LCD Command Defines ---
#define LCD_CHR 1 // Mode - Sending data
#define LCD_CMD 0 // Mode - Sending command

#define LINE1 0x80 // LCD RAM address for the 1st line
#define LINE2 0xC0 // LCD RAM address for the 2nd line

#define LCD_BACKLIGHT 0x08  // On
// #define LCD_BACKLIGHT 0x00  // Off

#define ENABLE 0b00000100 // Enable bit

// --- Global variables ---
static int i2c_file;

// --- Private Functions ---
void lcd_toggle_enable(int bits) {
    usleep(500);
    write(i2c_file, (unsigned char[]){(bits | ENABLE)}, 1);
    usleep(500);
    write(i2c_file, (unsigned char[]){(bits & ~ENABLE)}, 1);
    usleep(500);
}

void lcd_send_byte(int bits, int mode) {
    int bits_high = mode | (bits & 0xF0) | LCD_BACKLIGHT;
    int bits_low = mode | ((bits << 4) & 0xF0) | LCD_BACKLIGHT;

    write(i2c_file, (unsigned char[]){bits_high}, 1);
    lcd_toggle_enable(bits_high);

    write(i2c_file, (unsigned char[]){bits_low}, 1);
    lcd_toggle_enable(bits_low);
}

// --- Public Functions ---
int lcd_init(const char* i2c_bus, int i2c_addr) {
    i2c_file = open(i2c_bus, O_RDWR);
    if (i2c_file < 0) {
        perror("Failed to open the i2c bus");
        return -1;
    }

    if (ioctl(i2c_file, I2C_SLAVE, i2c_addr) < 0) {
        perror("Failed to acquire bus access and/or talk to slave");
        close(i2c_file);
        return -1;
    }

    // --- Standard LCD initialization sequence ---
    lcd_send_byte(0x33, LCD_CMD);
    lcd_send_byte(0x32, LCD_CMD);
    lcd_send_byte(0x06, LCD_CMD);
    lcd_send_byte(0x0C, LCD_CMD);
    lcd_send_byte(0x28, LCD_CMD);
    lcd_clear();
    usleep(5000);
    return 0;
}

void lcd_clear() {
    lcd_send_byte(0x01, LCD_CMD);
    usleep(2000);
}

void lcd_set_cursor(int row, int col) {
    int row_offsets[] = {LINE1, LINE2};
    if (row >= 0 && row < 2) {
        lcd_send_byte(row_offsets[row] + col, LCD_CMD);
    }
}

void lcd_send_string(const char *str) {
    while (*str) {
        lcd_send_byte(*(str++), LCD_CHR);
    }
}

void lcd_close() {
    if (i2c_file >= 0) {
        close(i2c_file);
    }
}
