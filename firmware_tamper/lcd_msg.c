#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lcd.h" // Your lcd.h

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <line1> <line2>\n", argv[0]);
        return 1;
    }
    const char *line1 = argv[1];
    const char *line2 = argv[2];

    if (lcd_init("/dev/i2c-3", 0x27) != 0) {
        fprintf(stderr, "Failed to initialize LCD.\n");
        return 1;
    }
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_send_string(line1);
    lcd_set_cursor(1, 0);
    lcd_send_string(line2);
    lcd_close();
    return 0;
}
