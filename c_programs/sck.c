#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define GPIO_SCK 68

void gpio_export(int gpio) {
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%d", gpio);
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    write(fd, buffer, len);
    close(fd);
}

void gpio_set_dir(int gpio, const char* dir) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    write(fd, dir, strlen(dir));
    close(fd);
}

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

int main() {
    gpio_export(GPIO_SCK);
    gpio_set_dir(GPIO_SCK, "out");
    while (1) {
        gpio_set_value(GPIO_SCK, 1);
        usleep(500000); // 0.5 second
        gpio_set_value(GPIO_SCK, 0);
        usleep(500000);
        printf("Toggled SCK pin\n");
    }
    return 0;
}
