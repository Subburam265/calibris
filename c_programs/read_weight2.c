#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#define GPIO_DOUT 69
#define GPIO_SCK  68

void gpio_export(int gpio) {
    char buffer[64];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if(fd < 0) return;
    snprintf(buffer, sizeof(buffer), "%d", gpio);
    write(fd, buffer, strlen(buffer));
    close(fd);
}

void gpio_unexport(int gpio) {
    char buffer[64];
    int fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if(fd < 0) return;
    snprintf(buffer, sizeof(buffer), "%d", gpio);
    write(fd, buffer, strlen(buffer));
    close(fd);
}

void gpio_set_dir(int gpio, const char* dir) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    if(fd < 0) return;
    write(fd, dir, strlen(dir));
    close(fd);
}

int gpio_open_value_fd(int gpio, int flags) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    return open(path, flags);
}

void gpio_write_value_fd(int fd, int value) {
    if(fd < 0) return;
    if(value)
        write(fd, "1", 1);
    else
        write(fd, "0", 1);
    fsync(fd);
}

int gpio_read_value_fd(int fd) {
    if(fd < 0) return -1;
    char buf[2];
    lseek(fd, 0, SEEK_SET);
    if(read(fd, buf, 1) != 1)
        return -1;
    buf[1] = '\0';
    return (buf[0] == '1') ? 1 : 0;
}

long hx711_read(int sck_fd, int dout_fd) {
    long count = 0;
    int bit;

    // Wait for data ready (DOUT low)
    while(gpio_read_value_fd(dout_fd) == 1) {
        usleep(1000); // 1 ms
    }

    for(int i = 0; i < 24; i++) {
        gpio_write_value_fd(sck_fd, 1);
        usleep(300); // 300 us pulse width

        count <<= 1;

        gpio_write_value_fd(sck_fd, 0);
        usleep(300);

        bit = gpio_read_value_fd(dout_fd);
        if(bit)
            count |= 1;
    }

    // Pulse clock once more to set gain=128
    gpio_write_value_fd(sck_fd, 1);
    usleep(300);
    gpio_write_value_fd(sck_fd, 0);
    usleep(300);

    // Convert 24-bit two's complement to signed long
    if(count & 0x800000)
        count |= ~0xffffff;

    return count;
}

int main() {
    // Export and set directions
    gpio_export(GPIO_DOUT);
    gpio_export(GPIO_SCK);
    usleep(100000); // Allow sysfs to create gpio files

    gpio_set_dir(GPIO_DOUT, "in");
    gpio_set_dir(GPIO_SCK, "out");

    int dout_fd = gpio_open_value_fd(GPIO_DOUT, O_RDONLY);
    int sck_fd = gpio_open_value_fd(GPIO_SCK, O_WRONLY);

    if(dout_fd < 0 || sck_fd < 0) {
        perror("Failed to open GPIO value file descriptors");
        return 1;
    }

    gpio_write_value_fd(sck_fd, 0); // Set clock low initially

    while(1) {
        long raw = hx711_read(sck_fd, dout_fd);
        printf("Raw weight data: %ld\n", raw);
        usleep(500000); // 0.5 second delay
    }

    close(dout_fd);
    close(sck_fd);

    gpio_unexport(GPIO_DOUT);
    gpio_unexport(GPIO_SCK);

    return 0;
}
