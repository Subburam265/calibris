#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

#define GPIO_DOUT 69
#define GPIO_SCK  68

// --- GPIO Utility Functions ---

// Export a GPIO pin for use
int gpio_export(int pin) {
    char buffer[3];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
        perror("Failed to open export for writing");
        return -1;
    }
    sprintf(buffer, "%d", pin);
    write(fd, buffer, strlen(buffer));
    close(fd);
    // A small delay for the system to create the directory
    usleep(100000); // 100ms
    return 0;
}

// Set the direction of a GPIO pin
int gpio_set_dir(int pin, const char *dir) {
    char path[64];
    sprintf(path, "/sys/class/gpio/gpio%d/direction", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open gpio direction for writing");
        return -1;
    }
    write(fd, dir, strlen(dir));
    close(fd);
    return 0;
}

// Set the value of a GPIO pin
int gpio_set_value(int pin, int value) {
    char path[64];
    sprintf(path, "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open gpio value for writing");
        return -1;
    }
    if (value) {
        write(fd, "1", 1);
    } else {
        write(fd, "0", 1);
    }
    close(fd);
    return 0;
}

// Get the value of a GPIO pin
int gpio_get_value(int pin) {
    char path[64];
    char value_str[3];
    sprintf(path, "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open gpio value for reading");
        return -1;
    }
    read(fd, value_str, 3);
    close(fd);
    return atoi(value_str);
}

// --- HX711 Specific Functions ---

int is_ready() {
    return gpio_get_value(GPIO_DOUT) == 0;
}

long hx711_read() {
    long count = 0;
    
    // Wait for the chip to become ready
    while (!is_ready()) {
        // You can add a timeout here if you want
    }

    // Pulse the clock 24 times to read the data
    for (int i = 0; i < 24; i++) {
        gpio_set_value(GPIO_SCK, 1);
        count = count << 1;
        gpio_set_value(GPIO_SCK, 0);
        if (gpio_get_value(GPIO_DOUT)) {
            count++;
        }
    }

    // 25th pulse to set gain for next reading
    gpio_set_value(GPIO_SCK, 1);
    gpio_set_value(GPIO_SCK, 0);

    // Convert to signed 32-bit integer
    if (count & 0x800000) {
        count |= (long)0xFFFFFFFF << 24;
    }
    
    return count;
}


int main() {
    printf("Setting up GPIO...\n");
    gpio_export(GPIO_DOUT);
    gpio_export(GPIO_SCK);
    gpio_set_dir(GPIO_DOUT, "in");
    gpio_set_dir(GPIO_SCK, "out");
    printf("Setup complete.\n");

    while (1) {
        long val = hx711_read();
        printf("Raw weight data: %ld\n", val);
        usleep(500000); // Sleep for 500ms
    }

    return 0;
}
