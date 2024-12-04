#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/timer.h>
#include "keyboard_i2c.h"

#define KEYBOARD_I2C_NAME "keyboard-i2c"
#define KEYBOARD_I2C_ADDR 0x5F
#define KEYBOARD_BUF_SIZE 1   // 每次读取1个字节
#define POLL_INTERVAL_MS 10   // 10ms轮询间隔

// 特殊键值定义
#define KEY_ESC    27
#define KEY_DEL    127
#define KEY_TAB    9
#define KEY_ENTER  13
#define KEY_LEFT   180
#define KEY_UP     181
#define KEY_DOWN   182
#define KEY_RIGHT  183

static void keyboard_timer_handler(struct timer_list *t)
{
    struct keyboard_i2c *kbd = from_timer(kbd, t, timer);
    u8 key_data;
    int ret;

    // 从I2C设备读取按键数据
    ret = i2c_master_recv(kbd->client, &key_data, KEYBOARD_BUF_SIZE);
    if (ret < 0) {
        dev_err(&kbd->client->dev, "i2c read failed\n");
        goto restart_timer;
    }

    // 处理按键数据
    if (key_data != 0) {
        switch(key_data) {
        case KEY_LEFT:
            input_report_key(kbd->input, KEY_LEFT, 1);
            break;
        case KEY_RIGHT:
            input_report_key(kbd->input, KEY_RIGHT, 1);
            break;
        case KEY_UP:
            input_report_key(kbd->input, KEY_UP, 1);
            break;
        case KEY_DOWN:
            input_report_key(kbd->input, KEY_DOWN, 1);
            break;
        case KEY_ESC:
            input_report_key(kbd->input, KEY_ESC, 1);
            break;
        case KEY_TAB:
            input_report_key(kbd->input, KEY_TAB, 1);
            break;
        case KEY_ENTER:
            input_report_key(kbd->input, KEY_ENTER, 1);
            break;
        case KEY_DEL:
            input_report_key(kbd->input, KEY_BACKSPACE, 1);
            break;
        default:
            // 普通ASCII字符
            if (key_data >= 32 && key_data <= 126) {
                input_report_key(kbd->input, key_data, 1);
            }
            break;
        }
        input_sync(kbd->input);

        // 释放按键
        switch(key_data) {
        case KEY_LEFT:
            input_report_key(kbd->input, KEY_LEFT, 0);
            break;
        case KEY_RIGHT:
            input_report_key(kbd->input, KEY_RIGHT, 0);
            break;
        case KEY_UP:
            input_report_key(kbd->input, KEY_UP, 0);
            break;
        case KEY_DOWN:
            input_report_key(kbd->input, KEY_DOWN, 0);
            break;
        case KEY_ESC:
            input_report_key(kbd->input, KEY_ESC, 0);
            break;
        case KEY_TAB:
            input_report_key(kbd->input, KEY_TAB, 0);
            break;
        case KEY_ENTER:
            input_report_key(kbd->input, KEY_ENTER, 0);
            break;
        case KEY_DEL:
            input_report_key(kbd->input, KEY_BACKSPACE, 0);
            break;
        default:
            // 普通ASCII字符
            if (key_data >= 32 && key_data <= 126) {
                input_report_key(kbd->input, key_data, 0);
            }
            break;
        }
        input_sync(kbd->input);
    }

restart_timer:
    mod_timer(&kbd->timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
}

static int keyboard_i2c_probe(struct i2c_client *client,
                            const struct i2c_device_id *id)
{
    struct keyboard_i2c *kbd;
    struct input_dev *input;
    int error;
    int i;

    kbd = devm_kzalloc(&client->dev, sizeof(*kbd), GFP_KERNEL);
    if (!kbd)
        return -ENOMEM;

    input = devm_input_allocate_device(&client->dev);
    if (!input)
        return -ENOMEM;

    kbd->client = client;
    kbd->input = input;

    // 设置输入设备参数
    input->name = "M5Stack CardKB";
    input->id.bustype = BUS_I2C;

    // 设置支持的按键
    __set_bit(EV_KEY, input->evbit);
    
    // 设置特殊按键
    __set_bit(KEY_ESC, input->keybit);
    __set_bit(KEY_BACKSPACE, input->keybit);
    __set_bit(KEY_TAB, input->keybit);
    __set_bit(KEY_ENTER, input->keybit);
    __set_bit(KEY_LEFT, input->keybit);
    __set_bit(KEY_RIGHT, input->keybit);
    __set_bit(KEY_UP, input->keybit);
    __set_bit(KEY_DOWN, input->keybit);
    
    // 设置ASCII字符按键
    for (i = 32; i <= 126; i++) {
        __set_bit(i, input->keybit);
    }

    // 初始化定时器
    timer_setup(&kbd->timer, keyboard_timer_handler, 0);
    mod_timer(&kbd->timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));

    // 注册输入设备
    error = input_register_device(input);
    if (error) {
        dev_err(&client->dev, "Failed to register input device\n");
        del_timer_sync(&kbd->timer);
        return error;
    }

    i2c_set_clientdata(client, kbd);
    return 0;
}

static int keyboard_i2c_remove(struct i2c_client *client)
{
    struct keyboard_i2c *kbd = i2c_get_clientdata(client);
    del_timer_sync(&kbd->timer);
    return 0;
}

static const struct i2c_device_id keyboard_i2c_id[] = {
    { KEYBOARD_I2C_NAME, 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, keyboard_i2c_id);

static const struct of_device_id keyboard_i2c_of_match[] = {
    { .compatible = "keyboard-i2c" },
    { }
};
MODULE_DEVICE_TABLE(of, keyboard_i2c_of_match);

static struct i2c_driver keyboard_i2c_driver = {
    .driver = {
        .name = KEYBOARD_I2C_NAME,
        .of_match_table = keyboard_i2c_of_match,
    },
    .probe = keyboard_i2c_probe,
    .remove = keyboard_i2c_remove,
    .id_table = keyboard_i2c_id,
};

module_i2c_driver(keyboard_i2c_driver);

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("M5Stack CardKB I2C Keyboard Driver");
MODULE_LICENSE("GPL");