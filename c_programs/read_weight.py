import time

GPIO_DOUT = 69
GPIO_SCK = 68

# Sysfs GPIO helper functions
def gpio_export(pin):
    try:
        with open("/sys/class/gpio/export", "w") as f:
            f.write(str(pin))
    except Exception:
        pass  # Already exported

def gpio_unexport(pin):
    with open("/sys/class/gpio/unexport", "w") as f:
        f.write(str(pin))

def gpio_set_dir(pin, direction):
    with open(f"/sys/class/gpio/gpio{pin}/direction", "w") as f:
        f.write(direction)

def gpio_set_value(pin, value):
    with open(f"/sys/class/gpio/gpio{pin}/value", "w") as f:
        f.write("1" if value else "0")

def gpio_get_value(pin):
    with open(f"/sys/class/gpio/gpio{pin}/value", "r") as f:
        return int(f.read().strip())

def setup():
    gpio_export(GPIO_DOUT)
    gpio_export(GPIO_SCK)
    gpio_set_dir(GPIO_DOUT, "in")
    gpio_set_dir(GPIO_SCK, "out")
    gpio_set_value(GPIO_SCK, 0)

def cleanup():
    gpio_unexport(GPIO_DOUT)
    gpio_unexport(GPIO_SCK)

def hx711_read():
    # Wait for data ready (DOUT low)
    while gpio_get_value(GPIO_DOUT) == 1:
        time.sleep(0.001)
    count = 0
    for _ in range(24):
        gpio_set_value(GPIO_SCK, 1)
        time.sleep(0.0001)
        count = count << 1
        gpio_set_value(GPIO_SCK, 0)
        if gpio_get_value(GPIO_DOUT):
            count += 1
        time.sleep(0.0001)
    # pulse clock once more for gain 128
    gpio_set_value(GPIO_SCK, 1)
    time.sleep(0.0001)
    gpio_set_value(GPIO_SCK, 0)
    time.sleep(0.0001)
    # convert to signed 24bit
    if count & 0x800000:
        count |= ~0xffffff
    return count

if __name__ == "__main__":
    try:
        setup()
        while True:
            val = hx711_read()
            print("Raw weight data:", val)
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("Exiting...")
    finally:
        cleanup()
