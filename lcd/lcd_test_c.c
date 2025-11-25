#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <string.h>
#include <errno.h>

// I2C Device details
const char *I2C_DEVICE = "/dev/i2c-3"; // Using I2C bus 3
const int I2C_ADDR = 0x27;            // Device address 0x27

// PCF8574 to LCD Pin Mapping
// These are bit masks for the 8-bit port on the PCF8574
const int LCD_RS = 0x01; // Register Select: 0 for command, 1 for data
const int LCD_RW = 0x02; // Read/Write: 0 for write, 1 for read (we won't use read)
const int LCD_E  = 0x04; // Enable pin
const int LCD_BACKLIGHT = 0x08; // Backlight control bit
const int LCD_D4 = 0x10;
const int LCD_D5 = 0x20;
const int LCD_D6 = 0x40;
const int LCD_D7 = 0x80;

// LCD Commands
const int LCD_CLEARDISPLAY = 0x01;
const int LCD_RETURNHOME = 0x02;
const int LCD_ENTRYMODESET = 0x04;
const int LCD_DISPLAYCONTROL = 0x08;
const int LCD_CURSORSHIFT = 0x10;
const int LCD_FUNCTIONSET = 0x20;
const int LCD_SETCGRAMADDR = 0x40;
const int LCD_SETDDRAMADDR = 0x80;

// Global file descriptor for the I2C bus
int i2c_fd;

// --- LOW-LEVEL FUNCTIONS ---

// Pulses the Enable pin to latch data
void lcd_pulse_enable(int data) {
    write(i2c_fd, (unsigned char[]){data | LCD_E}, 1);
    usleep(500); // Enable pulse must be >450ns
    write(i2c_fd, (unsigned char[]){data & ~LCD_E}, 1);
    usleep(500); // commands need > 37us to settle
}

// Sends a 4-bit nibble to the LCD
void lcd_write_4bits(int data) {
    write(i2c_fd, (unsigned char[]){data | LCD_BACKLIGHT}, 1);
    lcd_pulse_enable(data | LCD_BACKLIGHT);
}

// Sends a full byte (command or data) to the LCD
void lcd_send(int value, int mode) {
    int high_nibble = value & 0xF0;
    int low_nibble = (value << 4) & 0xF0;
    lcd_write_4bits(high_nibble | mode);
    lcd_write_4bits(low_nibble | mode);
}

// --- HIGH-LEVEL FUNCTIONS ---

void lcd_command(int cmd) {
    lcd_send(cmd, 0); // RS=0 for command mode
}

void lcd_data(int data) {
    lcd_send(data, LCD_RS); // RS=1 for data mode
}

void lcd_string(const char *s) {
    while (*s) {
        lcd_data(*s++);
    }
}

void lcd_set_cursor(int col, int row) {
    int row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    lcd_command(LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

void lcd_clear() {
    lcd_command(LCD_CLEARDISPLAY);
    usleep(2000); // clearDisplay command takes a long time
}

// Initializes the LCD in 4-bit mode
void lcd_init() {
    // This sequence is critical and timing-sensitive!
    // It's from the HD44780 datasheet for 4-bit initialization.
    usleep(50000); // Wait for >40ms after power-on

    // Put LCD into 4-bit mode
    lcd_write_4bits(0x30);
    usleep(4500);
    lcd_write_4bits(0x30);
    usleep(4500);
    lcd_write_4bits(0x30);
    usleep(150);
    lcd_write_4bits(0x20);

    // Now in 4-bit mode, configure the LCD
    lcd_command(LCD_FUNCTIONSET | 0x08); // 2 lines, 5x8 dots
    lcd_command(LCD_DISPLAYCONTROL | 0x04); // Display on, Cursor off, Blink off
    lcd_command(LCD_ENTRYMODESET | 0x02); // Increment cursor, no display shift
    lcd_clear();
}

int main() {
    // Open the I2C bus
    if ((i2c_fd = open(I2C_DEVICE, O_RDWR)) < 0) {
        fprintf(stderr, "Failed to open the i2c bus %s: %s\n", I2C_DEVICE, strerror(errno));
        return 1;
    }

    // Set the I2C slave address
    if (ioctl(i2c_fd, I2C_SLAVE, I2C_ADDR) < 0) {
        fprintf(stderr, "Failed to acquire bus access and/or talk to slave: %s\n", strerror(errno));
        close(i2c_fd);
        return 1;
    }

    printf("I2C bus opened successfully. Initializing LCD...\n");
    
    lcd_init();

    printf("Writing to display.\n");
    lcd_set_cursor(0, 0);
    lcd_string("Hello, Luckfox!");

    lcd_set_cursor(0, 1);
    lcd_string("I2C in C!");
    
    sleep(5); // Display for 5 seconds

    lcd_clear();
    lcd_string("Goodbye!");
    sleep(2);

    // Turn off backlight before exiting
    write(i2c_fd, (unsigned char[]){0x00}, 1);

    close(i2c_fd);
    printf("Done.\n");

    return 0;
}
