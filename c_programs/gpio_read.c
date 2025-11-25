#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    int gpio_pin;
    
    printf("Please enter the GPIO pin number: ");
    scanf("%d", &gpio_pin);
    
    // Export the GPIO pin
    FILE *export_file = fopen("/sys/class/gpio/export", "w");
    if (export_file == NULL) {
        perror("Failed to open GPIO export file");
        return -1;
    }
    fprintf(export_file, "%d", gpio_pin);
    fclose(export_file);

    // Set direction as input
    char direction_path[50];
    snprintf(direction_path, sizeof(direction_path), "/sys/class/gpio/gpio%d/direction", gpio_pin);
    FILE *direction_file = fopen(direction_path, "w");
    if (direction_file == NULL) {
        perror("Failed to open GPIO direction file");
        return -1;
    }
    fprintf(direction_file, "in");
    fclose(direction_file);

    // Path for GPIO value
    char value_path[50];
    snprintf(value_path, sizeof(value_path), "/sys/class/gpio/gpio%d/value", gpio_pin);

    for (int i = 0; i < 3; i++) {
        FILE *value_file = fopen(value_path, "r");
        if (value_file == NULL) {
            perror("Failed to open GPIO value file");
            return -1;
        }
        char value = fgetc(value_file);
        fclose(value_file);

        printf("GPIO pin %d input value: %c\n", gpio_pin, value);

        sleep(1);
    }

    // Unexport GPIO pin
    FILE *unexport_file = fopen("/sys/class/gpio/unexport", "w");
    if (unexport_file == NULL) {
        perror("Failed to open GPIO unexport file");
        return -1;
    }
    fprintf(unexport_file, "%d", gpio_pin);
    fclose(unexport_file);

    return 0;
}
