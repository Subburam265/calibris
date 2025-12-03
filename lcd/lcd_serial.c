#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lcd.h"

// Adjust bus and address for your setup
#define I2C_BUS "/dev/i2c-3"   // or "/dev/i2c-1" depending on pin mux
#define I2C_ADDR 0x27          // LCD I2C address (commonly 0x27 or 0x3F)

int main() {
    // Initialize LCD
    if (lcd_init(I2C_BUS, I2C_ADDR) < 0) {
        return -1;
    }

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string("Type in Serial");

    char buffer[128];

    while (1) {
        // Read a line from stdin (serial console)
        if (fgets(buffer, sizeof(buffer), stdin)) {
            buffer[strcspn(buffer, "\n")] = 0;  // strip newline

            lcd_clear();

            // Print first line (up to 16 chars)
            lcd_set_cursor(0, 0);
            lcd_send_string(buffer);

            // If longer than 16 chars, print next 16 on line 2
            if (strlen(buffer) > 16) {
                lcd_set_cursor(1, 0);
                lcd_send_string(buffer + 16);
            }

            printf("LCD shows: %s\n", buffer);
        }
        usleep(100000); // small delay
    }

    lcd_close();
    return 0;
} 
