#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define GPIO_DOUT 69
#define GPIO_SCK  68

void gpio_export(int gpio) {
    char buffer[64];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if(fd < 0) {
        if(errno == EBUSY) return; // Already exported
        perror("export");
        return;
    }
    snprintf(buffer, sizeof(buffer), "%d", gpio);
    if(write(fd, buffer, strlen(buffer)) < 0)
        perror("write export");
    close(fd);
}

void gpio_unexport(int gpio) {
    char buffer[64];
    int fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if(fd < 0) {
        perror("unexport");
        return;
    }
    snprintf(buffer, sizeof(buffer), "%d", gpio);
    if(write(fd, buffer, strlen(buffer)) < 0)
        perror("write unexport");
    close(fd);
}

void gpio_set_dir(int gpio, const char* dir) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    if(fd < 0) {
        perror("direction");
        return;
    }
    if(write(fd, dir, strlen(dir)) < 0)
        perror("write direction");
    close(fd);
}

int gpio_open_value_fd(int gpio, int flags) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    int fd = open(path, flags);
    if(fd < 0)
        perror("open value");
    return fd;
}

void gpio_write_value_fd(int fd, int value) {
    if(fd < 0) return;
    if(lseek(fd, 0, SEEK_SET) < 0)
        perror("lseek write");
    ssize_t written = write(fd, value ? "1" : "0", 1);
    if(written != 1)
        perror("write value");
    if(fsync(fd) < 0)
        perror("fsync");
    else
        printf("Wrote %c to GPIO\n", value ? '1' : '0');
}

int gpio_read_value_fd(int fd) {
    if(fd < 0) return -1;
    char buf[2];
    if(lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek read");
        return -1;
    }
    ssize_t rd = read(fd, buf, 1);
    if(rd != 1) {
        perror("read value");
        return -1;
    }
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
    gpio_export(GPIO_DOUT);
    gpio_export(GPIO_SCK);
    usleep(100000); // Allow sysfs files to be created

    gpio_set_dir(GPIO_DOUT, "in");
    gpio_set_dir(GPIO_SCK, "out");

    int dout_fd = gpio_open_value_fd(GPIO_DOUT, O_RDONLY);
    int sck_fd = gpio_open_value_fd(GPIO_SCK, O_WRONLY);

    if(dout_fd < 0 || sck_fd < 0) {
        fprintf(stderr, "Failed to open GPIO value files\n");
        return 1;
    }

    gpio_write_value_fd(sck_fd, 0); // Initialize clock low

    while(1) {
        long val = hx711_read(sck_fd, dout_fd);
        printf("Raw weight data: %ld\n", val);
        usleep(500000);
    }

    close(dout_fd);
    close(sck_fd);
    gpio_unexport(GPIO_DOUT);
    gpio_unexport(GPIO_SCK);

    return 0;
}
