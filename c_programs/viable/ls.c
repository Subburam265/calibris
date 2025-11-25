#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>

#include "ls.h"

// --- Configuration ---
#define PASSWORD "luckfox"

// This function will catch signals like Ctrl+C and ignore them
void signal_handler(int signum) {
    // We do nothing here to effectively block the signal
    printf("\nSignal blocked. Please enter the password.\n");
}

void enter_safe_mode(void) {
    struct termios old_tio, new_tio;
    char input_buffer[256];
    int index = 0;
    char c;

    // --- Step 1: Trap Signals ---
    signal(SIGINT, signal_handler);  // Catches Ctrl+C
    //signal(SIGTSTP, signal_handler); // Catches Ctrl+Z

    // --- Step 2: Take Over Terminal ---
    // Get the current terminal settings
    tcgetattr(STDIN_FILENO, &old_tio);
    // Copy them to a new struct to modify
    new_tio = old_tio;
    // Disable canonical mode (line-by-line input) and echoing
    new_tio.c_lflag &= ~(ICANON | ECHO);
    // Apply the new settings immediately
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

    // --- Step 3: The Main Password Loop ---
    while (1) {
        // Clear the screen for a clean prompt
        //system("clear");
        printf("--- SYSTEM LOCKED ---\n");
        printf("Enter password to unlock: ");
        
        index = 0;
        memset(input_buffer, 0, sizeof(input_buffer));

        // Read character by character
        while ((c = getchar()) != '\n' && index < sizeof(input_buffer) - 1) {
            if (c == 127 || c == '\b') { // Handle backspace
                 if (index > 0) index--;
            } else {
                 input_buffer[index++] = c;
            }
        }
        input_buffer[index] = '\0'; // Null-terminate the string

        // Check the password
        if (strcmp(input_buffer, PASSWORD) == 0) {
            printf("\nPassword correct. Unlocking system.\n");
            break; // Exit the while loop
        } else {
            printf("\nInvalid password. Please wait...\n");
            sleep(2); // A short delay to prevent brute-forcing
        }
    }

    // --- Step 4: Restore Terminal and Exit ---
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    
}
