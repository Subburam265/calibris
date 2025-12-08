#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

// --- Configuration ---
const char* I2C_BUS = "/dev/i2c-3"; // The I2C bus device
const int I2C_ADDR  = 0x27;         // Common LCD addresses: 0x27 or 0x3F

int file_i2c;

// --- LCD Bit Mapping (PCF8574) ---
// P7..P0 = D7, D6, D5, D4, Backlight, En, Rw, Rs
#define LCD_BACKLIGHT 0x08
#define LCD_NOBACKLIGHT 0x00

#define En 0b00000100  // Enable bit
#define Rw 0b00000010  // Read/Write bit
#define Rs 0b00000001  // Register Select bit

// Helper: Write a raw byte to the I2C bus
void i2c_write_byte(unsigned char val) {
    if (write(file_i2c, &val, 1) != 1) {
        printf("Error: Failed to write to I2C bus\n");
    }
}

// Helper: Toggle the Enable pin to latch data
void lcd_pulse(unsigned char val) {
    i2c_write_byte(val | En | LCD_BACKLIGHT); 
    usleep(2000); 
    i2c_write_byte((val & ~En) | LCD_BACKLIGHT); 
    usleep(1000); 
}

// Send a byte to the LCD (Command or Data)
// mode: 0 = Command, 1 = Data (Character)
void lcd_send_byte(unsigned char val, int mode) {
    unsigned char high_nib = val & 0xF0;
    unsigned char low_nib = (val << 4) & 0xF0;
    unsigned char mode_bits = (mode == 1) ? Rs : 0;

    lcd_pulse(high_nib | mode_bits | LCD_BACKLIGHT);
    lcd_pulse(low_nib | mode_bits | LCD_BACKLIGHT);
}

void lcd_clear() {
    lcd_send_byte(0x01, 0); 
    usleep(2000); 
}

void lcd_set_cursor(int line, int index) {
    // Offset for 16x2 and 20x4 screens
    int row_offsets[] = { 0x00, 0x40, 0x14, 0x54 };
    lcd_send_byte(0x80 | (row_offsets[line] + index), 0);
}

void lcd_print(const char *str) {
    while (*str) {
        lcd_send_byte((unsigned char)(*str), 1);
        str++;
    }
}

void lcd_init() {
    // Initialization sequence for 4-bit mode
    usleep(50000); 
    lcd_pulse(0x30);
    usleep(5000);
    lcd_pulse(0x30);
    usleep(200);
    lcd_pulse(0x30);
    
    lcd_pulse(0x20); // Switch to 4-bit mode

    // Function set
    lcd_send_byte(0x28, 0); // 4-bit, 2 line, 5x8 dots
    lcd_send_byte(0x0C, 0); // Display ON, Cursor OFF
    lcd_send_byte(0x06, 0); // Entry mode: Auto increment
    lcd_clear();
}

int main() {
    // 1. Open the I2C Bus
    if ((file_i2c = open(I2C_BUS, O_RDWR)) < 0) {
        perror("Failed to open the i2c bus");
        return 1;
    }

    // 2. Connect to the Specific LCD Address
    if (ioctl(file_i2c, I2C_SLAVE, I2C_ADDR) < 0) {
        perror("Failed to acquire bus access and/or talk to slave");
        return 1;
    }

    printf("I2C Bus Opened. Starting LCD...\n");

    lcd_init();

    int count = 0;
    char buffer[32];

    // 3. Main Loop
    while(1) {
        // Line 1
        lcd_set_cursor(0, 0);
        lcd_print("LCD Working!");

        // Line 2
        lcd_set_cursor(1, 0);
        sprintf(buffer, "Count: %d     ", count);
        lcd_print(buffer);
        
        printf("Displayed Count: %d\n", count);
        
        count++;
        sleep(1);
    }

    close(file_i2c);
    return 0;
}
