#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>

// Configuration
#define CONFIG_FILE "/home/pico/calibris/data/config.json"
#define I2C_DEVICE "/dev/i2c-3"
#define I2C_ADDR 0x27
#define CLEARANCE_TOKEN "123456"  // 6-digit clearance token (change this to your desired token)
#define MW7_SERVICE "measure_weight.service"

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

// Terminal settings for restoring later
struct termios old_tio, new_tio;

// --- Terminal Functions ---

void init_terminal() {
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio. c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    new_tio.c_cc[VMIN] = 0;   // Non-blocking
    new_tio.c_cc[VTIME] = 0;  // No timeout
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
}

void enable_blocking_input() {
    struct termios blocking_tio = old_tio;
    blocking_tio.c_lflag &= ~ECHO;  // Keep echo off for password-style input
    tcsetattr(STDIN_FILENO, TCSANOW, &blocking_tio);
}

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

// --- Service Control ---

int start_measure_weight_service() {
    printf("Starting %s.. .\n", MW7_SERVICE);
    execl("/usr/bin/sudo", "sudo", "/usr/bin/systemctl", "enable", "--now", MW7_SERVICE, NULL); 
    /*char command[256];
    snprintf(command, sizeof(command), "sudo systemctl enable %s", MW7_SERVICE);
    int result = system(command);
    if (result == 0) {
        printf("Successfully started %s\n", MW7_SERVICE);
        return 0;
    } else {
        fprintf(stderr, "Failed to start %s\n", MW7_SERVICE);
        return -1;
    }*/
    perror("Failed to switch to service");
    exit(1);
}

// --- Token Verification ---

int verify_clearance_token() {
    char token[16];
    int token_index = 0;
    char c;
    
    // Display prompt on LCD
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_string("Enter Token:");
    lcd_set_cursor(0, 1);
    lcd_string("______");  // Placeholder for 6 digits
    
    printf("\n\nEnter 6-digit clearance token: ");
    fflush(stdout);
    
    // Enable blocking input for token entry
    enable_blocking_input();
    
    // Read exactly 6 digits
    token_index = 0;
    while (token_index < 6) {
        if (read(STDIN_FILENO, &c, 1) > 0) {
            // Check if it's a digit
            if (c >= '0' && c <= '9') {
                token[token_index] = c;
                token_index++;
                
                // Update LCD with asterisks for security
                lcd_set_cursor(token_index - 1, 1);
                lcd_data('*');
                
                // Echo asterisk to console
                printf("*");
                fflush(stdout);
            }
            // Allow backspace
            else if ((c == 127 || c == 8) && token_index > 0) {
                token_index--;
                lcd_set_cursor(token_index, 1);
                lcd_data('_');
                printf("\b \b");
                fflush(stdout);
            }
            // Allow escape to cancel
            else if (c == 27) {
                printf("\nToken entry cancelled.\n");
                init_terminal();  // Restore non-blocking
                return 0;
            }
        }
    }
    token[6] = '\0';
    
    printf("\n");
    
    // Restore non-blocking terminal
    init_terminal();
    
    // Verify token
    if (strcmp(token, CLEARANCE_TOKEN) == 0) {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_string("Access Granted!");
        lcd_set_cursor(0, 1);
        lcd_string("Starting Scale. .");
        
        printf("Clearance token verified!  Access granted.\n");
        usleep(2000000);  // Show message for 2 seconds
        
        return 1;  // Token matched
    } else {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_string("Access Denied!");
        lcd_set_cursor(0, 1);
        lcd_string("Invalid Token");
        
        printf("Invalid clearance token!  Access denied.\n");
        usleep(2000000);  // Show message for 2 seconds
        
        return 0;  // Token did not match
    }
}

// --- Signal Handler ---

void cleanup_and_exit(int signum) {
    printf("\nReceived signal %d.  Cleaning up...\n", signum);
    restore_terminal();
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_string("Shutting Down...");
    usleep(1000000);
    lcd_close();
    exit(0);
}

// --- Main Program ---

int main() {
    printf("Calibris Safe Mode Checker\n");
    printf("==========================\n");

    // Set up signal handlers for graceful exit
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

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

    // Initialize terminal for non-blocking input
    init_terminal();

    // Display safe mode message
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_string("** SAFE MODE **");
    lcd_set_cursor(0, 1);
    lcd_string("Press ENTER.. .");

    printf("Safe mode message displayed on LCD.\n");
    printf("Device is now in safe mode.\n");
    printf("Press ENTER to input clearance token, or Ctrl+C to exit.\n\n");

    // Main loop - wait for Enter key
    char c;
    int blink_state = 0;
    int loop_counter = 0;
    
    while (1) {
        // Check for keypress
        if (read(STDIN_FILENO, &c, 1) > 0) {
            // Enter key pressed (newline or carriage return)
            if (c == '\n' || c == '\r') {
                printf("Enter key detected.  Requesting clearance token...\n");
                
                // Attempt token verification
                if (verify_clearance_token()) {
                    // Token verified - start the service and exit
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_string("Exiting Safe");
                    lcd_set_cursor(0, 1);
                    lcd_string("Mode...");
                    
                    // Start the measure weight service
                    if (start_measure_weight_service() == 0) {
                        printf("Successfully exited safe mode.\n");
                        printf("Weight measurement service is now running.\n");
                        
                        lcd_clear();
                        lcd_set_cursor(0, 0);
                        lcd_string("Service Started");
                        lcd_set_cursor(0, 1);
                        lcd_string("Goodbye!");
                        usleep(2000000);
                        
                        // Cleanup and exit
                        restore_terminal();
                        lcd_close();
                        return 0;
                    } else {
                        // Failed to start service, return to safe mode
                        lcd_clear();
                        lcd_set_cursor(0, 0);
                        lcd_string("Service Error!");
                        lcd_set_cursor(0, 1);
                        lcd_string("Staying Safe.. .");
                        usleep(2000000);
                    }
                }
                
                // Return to safe mode display (token failed or service failed)
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_string("** SAFE MODE **");
                lcd_set_cursor(0, 1);
                lcd_string("Press ENTER...");
            }
        }
        
        // Blink indicator to show the program is alive
        loop_counter++;
        if (loop_counter >= 10) {  // Every ~1 second
            loop_counter = 0;
            lcd_set_cursor(15, 0);
            if (blink_state) {
                lcd_data('*');
            } else {
                lcd_data(' ');
            }
            blink_state = ! blink_state;
        }
        
        usleep(100000);  // 100ms delay
    }

    // Cleanup (unreachable, but good practice)
    restore_terminal();
    lcd_clear();
    lcd_close();

    return 0;
}
