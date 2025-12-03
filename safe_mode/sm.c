#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <errno.h>

// Configuration
#define CONFIG_FILE "/home/pico/calibris/data/config.json"
#define I2C_DEVICE "/dev/i2c-3"
#define I2C_ADDR 0x27

// PCF8574 to LCD Pin Mapping
#define LCD_RS 0x01
#define LCD_RW 0x02
#define LCD_E  0x04
#define LCD_BACKLIGHT 0x08

// LCD Commands
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_FUNCTIONSET 0x20
#define LCD_SETDDRAMADDR 0x80

// Global file descriptor for the I2C bus
int i2c_fd;

// --- LCD Functions ---

void lcd_pulse_enable(int data) {
    unsigned char buf1 = data | LCD_E;
    unsigned char buf2 = data & ~LCD_E;
    write(i2c_fd, &buf1, 1);
    usleep(500);
    write(i2c_fd, &buf2, 1);
    usleep(500);
}

void lcd_write_4bits(int data) {
    unsigned char buf = data | LCD_BACKLIGHT;
    write(i2c_fd, &buf, 1);
    lcd_pulse_enable(data | LCD_BACKLIGHT);
}

void lcd_send(int value, int mode) {
    int high_nibble = value & 0xF0;
    int low_nibble = (value << 4) & 0xF0;
    lcd_write_4bits(high_nibble | mode);
    lcd_write_4bits(low_nibble | mode);
}

void lcd_command(int cmd) {
    lcd_send(cmd, 0);
}

void lcd_data(int data) {
    lcd_send(data, LCD_RS);
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
    usleep(2000);
}

int lcd_init() {
    // Open the I2C bus
    if ((i2c_fd = open(I2C_DEVICE, O_RDWR)) < 0) {
        fprintf(stderr, "Failed to open the i2c bus %s: %s\n", I2C_DEVICE, strerror(errno));
        return -1;
    }

    // Set the I2C slave address
    if (ioctl(i2c_fd, I2C_SLAVE, I2C_ADDR) < 0) {
        fprintf(stderr, "Failed to acquire bus access: %s\n", strerror(errno));
        close(i2c_fd);
        return -1;
    }

    // Initialize LCD in 4-bit mode
    usleep(50000);
    lcd_write_4bits(0x30);
    usleep(4500);
    lcd_write_4bits(0x30);
    usleep(4500);
    lcd_write_4bits(0x30);
    usleep(150);
    lcd_write_4bits(0x20);

    lcd_command(LCD_FUNCTIONSET | 0x08);
    lcd_command(LCD_DISPLAYCONTROL | 0x04);
    lcd_command(LCD_ENTRYMODESET | 0x02);
    lcd_clear();

    return 0;
}

void lcd_close() {
    if (i2c_fd >= 0) {
        unsigned char buf = 0x00;
        write(i2c_fd, &buf, 1);
        close(i2c_fd);
    }
}

// --- Config Parsing ---

int check_safe_mode() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open config file: %s\n", CONFIG_FILE);
        return 0;
    }

    char buffer[1024];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, fp);
    fclose(fp);
    buffer[len] = '\0';

    // Check for safe_mode: true (handles variations in spacing)
    if (strstr(buffer, "\"safe_mode\": true") != NULL ||
        strstr(buffer, "\"safe_mode\":true") != NULL) {
        return 1;
    }
    return 0;
}

// --- Main Program ---

int main() {
    printf("Calibris Safe Mode Checker\n");
    printf("==========================\n");

    // Check if safe mode is enabled
    if (!check_safe_mode()) {
        printf("Safe mode is DISABLED.  Exiting.\n");
        printf("The mw7 service should be started instead.\n");
        return 0;
    }

    printf("Safe mode is ENABLED.\n");
    printf("Initializing LCD to display safe mode message...\n");

    // Initialize LCD
    if (lcd_init() != 0) {
        fprintf(stderr, "Failed to initialize LCD.\n");
        return 1;
    }

    // Display safe mode message
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_string("** SAFE MODE **");
    lcd_set_cursor(0, 1);
    lcd_string("Device Protected");

    printf("Safe mode message displayed on LCD.\n");
    printf("Device is now in safe mode.  Press Ctrl+C to exit.\n");

    // Keep the program running to maintain the display
    // In safe mode, we just hold this state
    while (1) {
        sleep(10);
        
        // Optionally blink or update display to show it's alive
        lcd_set_cursor(15, 0);
        lcd_data('*');
        usleep(500000);
        lcd_set_cursor(15, 0);
        lcd_data(' ');
        usleep(500000);
    }

    // Cleanup (unreachable, but good practice)
    lcd_clear();
    lcd_close();

    return 0;
}
