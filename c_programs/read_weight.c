#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define GPIO_DOUT 69 // Example GPIO pin for HX711 DOUT (GPIO1_C1_d pin 49)
#define GPIO_SCK  68 // Example GPIO pin for HX711 SCK  (GPIO1_C2_d pin 50)

// Export GPIO pin in sysfs
void gpio_export(int gpio) {
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%d", gpio);
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    write(fd, buffer, len);
    close(fd);
}

// Set GPIO direction
void gpio_set_dir(int gpio, const char* dir) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    write(fd, dir, strlen(dir));
    close(fd);
}

// Write GPIO value
void gpio_set_value(int gpio, int value) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    int fd = open(path, O_WRONLY);
    if(value)
        write(fd, "1", 1);
    else
        write(fd, "0", 1);
    close(fd);
}

// Read GPIO value
int gpio_get_value(int gpio) {
    char path[64], value_str[3];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    int fd = open(path, O_RDONLY);
    read(fd, value_str, 3);
    close(fd);
    return atoi(value_str);
}

// HX711 read 24-bit value
long hx711_read() {
    long count = 0;
    unsigned char i;
    
    // Wait for data ready
    while(gpio_get_value(GPIO_DOUT) == 1) {
      usleep(1000); // wait 1ms
    }

    for (i = 0; i < 24; i++) {
        gpio_set_value(GPIO_SCK, 1);
        usleep(300);  // longer pulse width 300us
        count = count << 1;
        gpio_set_value(GPIO_SCK, 0);
        int bit = gpio_get_value(GPIO_DOUT);
        if(bit)
            count++;
        usleep(300);
    }

    // Set gain by sending one more pulse
    gpio_set_value(GPIO_SCK, 1);
    usleep(300);
    gpio_set_value(GPIO_SCK, 0);
    usleep(300);

    // Convert 24-bit two's complement to signed long
    if(count & 0x800000)
        count |= ~0xffffff;

    return count;
}

int main() {
    // Initialize GPIO pins
    gpio_export(GPIO_DOUT);
    gpio_export(GPIO_SCK);

    gpio_set_dir(GPIO_DOUT, "in");
    gpio_set_dir(GPIO_SCK, "out");

    // Set initial SCK low
    gpio_set_value(GPIO_SCK, 0);

    while(1) {
        long weight_raw = hx711_read();
        printf("Raw weight data: %ld\n", weight_raw);
        usleep(500000); // 0.5s delay between readings
    }

    return 0;
}
