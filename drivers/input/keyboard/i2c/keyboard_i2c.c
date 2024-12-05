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
#define KEY_FN_BASE 128  // Fn组合键的基础值

// 在文件开头添加键码映射表
static const unsigned short keycode_map[256] = {
    [0 ... 255] = 0,  // 默认所有键码映射为0
    
    // 控制键
    [8]   = KEY_BACKSPACE,
    [9]   = KEY_TAB,
    [13]  = KEY_ENTER,
    [27]  = KEY_ESC,
    [127] = KEY_BACKSPACE,
    
    // 空格
    [' '] = KEY_SPACE,
    
    // 数字键
    ['0'] = KEY_0, ['1'] = KEY_1, ['2'] = KEY_2, ['3'] = KEY_3, ['4'] = KEY_4,
    ['5'] = KEY_5, ['6'] = KEY_6, ['7'] = KEY_7, ['8'] = KEY_8, ['9'] = KEY_9,
    
    // 字母键(小写)
    ['a'] = KEY_A, ['b'] = KEY_B, ['c'] = KEY_C, ['d'] = KEY_D, ['e'] = KEY_E,
    ['f'] = KEY_F, ['g'] = KEY_G, ['h'] = KEY_H, ['i'] = KEY_I, ['j'] = KEY_J,
    ['k'] = KEY_K, ['l'] = KEY_L, ['m'] = KEY_M, ['n'] = KEY_N, ['o'] = KEY_O,
    ['p'] = KEY_P, ['q'] = KEY_Q, ['r'] = KEY_R, ['s'] = KEY_S, ['t'] = KEY_T,
    ['u'] = KEY_U, ['v'] = KEY_V, ['w'] = KEY_W, ['x'] = KEY_X, ['y'] = KEY_Y,
    ['z'] = KEY_Z,
    
    // 符号键
    ['-']  = KEY_MINUS,
    ['=']  = KEY_EQUAL,
    ['[']  = KEY_LEFTBRACE,
    [']']  = KEY_RIGHTBRACE,
    [';']  = KEY_SEMICOLON,
    ['\''] = KEY_APOSTROPHE,
    ['`']  = KEY_GRAVE,
    ['\\'] = KEY_BACKSLASH,
    [',']  = KEY_COMMA,
    ['.']  = KEY_DOT,
    ['/']  = KEY_SLASH,
    
    // 方向键
    [180] = KEY_LEFT,
    [181] = KEY_UP,
    [182] = KEY_DOWN,
    [183] = KEY_RIGHT,

    // Fn组合键(128-175)
    [128] = KEY_F1,
    [129] = KEY_F2,
    [130] = KEY_F3,
    [131] = KEY_F4,
    [132] = KEY_F5,
    [133] = KEY_F6,
    [134] = KEY_F7,
    [135] = KEY_F8,
    [136] = KEY_F9,
    [137] = KEY_F10,
    [138] = KEY_F11,
    [139] = KEY_F12,
};

// 在文件开头添加按键定义
static const unsigned int keyboard_keys[] = {
    // 功能键
    KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_ENTER,
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
    
    // 字母键 A-Z
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
    KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
    KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    
    // 数字键 0-9
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
    KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    
    // 符号键
    KEY_SPACE, KEY_MINUS, KEY_EQUAL,
    KEY_LEFTBRACE, KEY_RIGHTBRACE,
    KEY_SEMICOLON, KEY_APOSTROPHE,
    KEY_GRAVE, KEY_BACKSLASH,
    KEY_COMMA, KEY_DOT, KEY_SLASH,
    
    // 功能键 F1-F12
    KEY_F1, KEY_F2, KEY_F3, KEY_F4,
    KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,
};

struct keyboard_i2c {
    struct i2c_client *client;
    struct input_dev *input;
    struct timer_list timer;
    struct mutex lock;
    bool shift_state;
    bool device_present;  // 添加设备存在标志
};

static void keyboard_timer_handler(struct timer_list *t)
{
    struct keyboard_i2c *kbd = from_timer(kbd, t, timer);
    u8 key_data;
    int ret;
    unsigned short key_code;
    bool is_upper = false;

    if (!mutex_trylock(&kbd->lock)) {
        goto reschedule;
    }

    // 如果设备不存在，定期检查是否已连接
    if (!kbd->device_present) {
        ret = i2c_smbus_read_byte(kbd->client);
        if (ret >= 0) {
            kbd->device_present = true;
            dev_info(&kbd->client->dev, "Keyboard device connected\n");
        }
        goto unlock;
    }

    // 使用 i2c_smbus_read_byte 替代 i2c_master_recv
    ret = i2c_smbus_read_byte(kbd->client);
    if (ret < 0) {
        if (ret != -ENXIO && ret != -EREMOTEIO) {  // 忽略常见的无设备错误
            dev_err(&kbd->client->dev, "i2c read failed: %d\n", ret);
        }
        kbd->device_present = false;  // 标记设备已断开
        dev_info(&kbd->client->dev, "Keyboard device disconnected\n");
        goto unlock;
    }

    key_data = (u8)ret;
    if (key_data != 0) {
        // 检查是否是大写字母
        if (key_data >= 'A' && key_data <= 'Z') {
            is_upper = true;
            key_data = key_data - 'A' + 'a';  // 转换为小写
        }

        key_code = keycode_map[key_data];
        if (key_code) {
            if (is_upper) {
                // 按下shift
                input_report_key(kbd->input, KEY_LEFTSHIFT, 1);
                input_sync(kbd->input);
            }

            // 报告按键
            input_report_key(kbd->input, key_code, 1);
            input_sync(kbd->input);
            input_report_key(kbd->input, key_code, 0);
            input_sync(kbd->input);

            if (is_upper) {
                // 释放shift
                input_report_key(kbd->input, KEY_LEFTSHIFT, 0);
                input_sync(kbd->input);
            }
        }
    }

unlock:
    mutex_unlock(&kbd->lock);
reschedule:
    mod_timer(&kbd->timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
}

static int keyboard_i2c_probe(struct i2c_client *client,
                            const struct i2c_device_id *id)
{
    struct keyboard_i2c *kbd;
    struct input_dev *input;
    int error;
    int i;

    // 首先检查设备是否存在
    error = i2c_smbus_read_byte(client);
    if (error < 0) {
        dev_info(&client->dev, "Keyboard device not detected\n");
        return -ENODEV;  // 设备不存在，返回错误
    }

    kbd = devm_kzalloc(&client->dev, sizeof(*kbd), GFP_KERNEL);
    if (!kbd)
        return -ENOMEM;

    input = devm_input_allocate_device(&client->dev);
    if (!input)
        return -ENOMEM;

    kbd->client = client;
    kbd->input = input;
    kbd->device_present = true;  // 设备存在
    
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

    // 设置支持的按键类型
    __set_bit(EV_KEY, input->evbit);
    
    // 注册所有支持的按键
    for (i = 0; i < ARRAY_SIZE(keyboard_keys); i++) {
        __set_bit(keyboard_keys[i], input->keybit);
    }

    // 添加对SHIFT键的支持
    __set_bit(KEY_LEFTSHIFT, input->keybit);

    // 初始化定时器
    timer_setup(&kbd->timer, keyboard_timer_handler, 0);
    mod_timer(&kbd->timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));

    error = input_register_device(input);
    if (error) {
        dev_err(&client->dev, "Failed to register input device\n");
        del_timer_sync(&kbd->timer);
        return error;
    }

    dev_info(&client->dev, "Keyboard device initialized successfully\n");
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