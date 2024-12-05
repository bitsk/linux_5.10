#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/i2c-dev.h>

#define KEYBOARD_I2C_NAME "keyboard-i2c"
#define KEYBOARD_I2C_ADDR 0x5F
#define KEYBOARD_BUF_SIZE 1   // 每次读取1个字节
#define POLL_INTERVAL_MS 10   // 10ms轮询间隔


struct keyboard_i2c {
    struct i2c_client *client;
    struct input_dev *input;
    struct timer_list timer;
    struct mutex lock;
};

static void keyboard_timer_handler(struct timer_list *t)
{
    struct keyboard_i2c *kbd = from_timer(kbd, t, timer);
    u8 key_data;
    int ret;

    if (!mutex_trylock(&kbd->lock)) {
        mod_timer(&kbd->timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
        return;
    }

    // 设置I2C适配器超时时间
    kbd->client->adapter->timeout = HZ/10; // 100ms timeout
    
    ret = i2c_master_recv(kbd->client, &key_data, KEYBOARD_BUF_SIZE);
    if (ret < 0) {
        dev_err(&kbd->client->dev, "i2c read failed: %d\n", ret);
        goto unlock;
    }

    if (key_data != 0) {
        int key_code;
        
        switch(key_data) {
        case 180: // 原KEY_LEFT
            key_code = KEY_LEFT;
            break;
        case 181: // 原KEY_UP
            key_code = KEY_UP;
            break;
        case 182: // 原KEY_DOWN
            key_code = KEY_DOWN;
            break;
        case 183: // 原KEY_RIGHT
            key_code = KEY_RIGHT;
            break;
        case 27:  // ESC
            key_code = KEY_ESC;
            break;
        case 9:   // TAB
            key_code = KEY_TAB;
            break;
        case 13:  // ENTER
            key_code = KEY_ENTER;
            break;
        case 127: // DEL
            key_code = KEY_BACKSPACE;
            break;
        default:
            // ASCII字符
            if (key_data >= 32 && key_data <= 126) {
                key_code = key_data;
            } else {
                goto unlock;
            }
            break;
        }

        input_report_key(kbd->input, key_code, 1);
        input_sync(kbd->input);
        input_report_key(kbd->input, key_code, 0);
        input_sync(kbd->input);
    }

unlock:
    mutex_unlock(&kbd->lock);
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
    
    // 初始化互斥锁
    mutex_init(&kbd->lock);

    // 设置输入设备参数
    input->name = "M5Stack CardKB";
    input->phys = "cardkb/input0";
    input->dev.parent = &client->dev;
    
    input->id.bustype = BUS_I2C;
    input->id.vendor = 0x0001;
    input->id.product = 0x9637;
    input->id.version = 0x0001;

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