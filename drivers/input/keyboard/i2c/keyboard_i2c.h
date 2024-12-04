#ifndef KEYBOARD_I2C_H
#define KEYBOARD_I2C_H

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/timer.h>

struct keyboard_i2c {
    struct i2c_client *client;
    struct input_dev *input;
    struct timer_list timer;
};

#endif /* KEYBOARD_I2C_H */