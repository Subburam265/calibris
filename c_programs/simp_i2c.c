#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define I2C_BUS "/dev/i2c-4"

// Common I2C LCD addresses to try
const int LCD_ADDRESSES[] = {0x27, 0x3F, 0x20, 0x38};
const int NUM_ADDRESSES = 4;

int main() {
    int file;
    printf("I2C LCD Test Program\n");
    
    // Open I2C bus
    if ((file = open(I2C_BUS, O_RDWR)) < 0) {
        printf("Error: Could not open I2C bus %s\n", I2C_BUS);
        return 1;
    }
    
    printf("I2C bus opened successfully.\n");
    printf("Testing common LCD addresses...\n");
    
    // Try each common address
    for (int i = 0; i < NUM_ADDRESSES; i++) {
        int address = LCD_ADDRESSES[i];
        printf("Trying address 0x%02X: ", address);
        
        // Try to communicate with the device
        if (ioctl(file, I2C_SLAVE, address) < 0) {
            printf("Failed to acquire bus access\n");
            continue;
        }
        
        // Try to send a command (backlight on)
        char buf[1] = {0x08};
        if (write(file, buf, 1) != 1) {
            printf("No response\n");
        } else {
            printf("FOUND! Device responded at 0x%02X\n", address);
        }
        
        usleep(100000); // 100ms delay between attempts
    }
    
    // Try all possible I2C addresses (brute force approach)
    printf("\nScanning all possible I2C addresses (this might take a while)...\n");
    for (int address = 0x03; address < 0x78; address++) {
        // Skip quick command addresses
        if (address <= 0x07 || (address >= 0x78 && address <= 0x7F)) {
            continue;
        }
        
        if (ioctl(file, I2C_SLAVE, address) < 0) {
            continue;
        }
        
        // Try to read a byte - if it doesn't error out, device might be there
        char buf[1];
        if (read(file, buf, 1) >= 0) {
            printf("Potential device found at address 0x%02X\n", address);
        }
    }
    
    close(file);
    printf("Test completed\n");
    return 0;
}
