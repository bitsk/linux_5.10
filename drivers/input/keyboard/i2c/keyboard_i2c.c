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
    
    // 字母键(大写)
    ['A'] = KEY_A, ['B'] = KEY_B, ['C'] = KEY_C, ['D'] = KEY_D, ['E'] = KEY_E,
    ['F'] = KEY_F, ['G'] = KEY_G, ['H'] = KEY_H, ['I'] = KEY_I, ['J'] = KEY_J,
    ['K'] = KEY_K, ['L'] = KEY_L, ['M'] = KEY_M, ['N'] = KEY_N, ['O'] = KEY_O,
    ['P'] = KEY_P, ['Q'] = KEY_Q, ['R'] = KEY_R, ['S'] = KEY_S, ['T'] = KEY_T,
    ['U'] = KEY_U, ['V'] = KEY_V, ['W'] = KEY_W, ['X'] = KEY_X, ['Y'] = KEY_Y,
    ['Z'] = KEY_Z,
    
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
    unsigned short key_code;

    if (!mutex_trylock(&kbd->lock)) {
        mod_timer(&kbd->timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
        return;
    }

    kbd->client->adapter->timeout = HZ/10;
    
    ret = i2c_master_recv(kbd->client, &key_data, KEYBOARD_BUF_SIZE);
    if (ret < 0) {
        dev_err(&kbd->client->dev, "i2c read failed: %d\n", ret);
        goto unlock;
    }

    if (key_data != 0) {
        key_code = keycode_map[key_data];
        if (key_code) {
            input_report_key(kbd->input, key_code, 1);
            input_sync(kbd->input);
            input_report_key(kbd->input, key_code, 0);
            input_sync(kbd->input);
        }
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

    // 设置支持的按键类型
    __set_bit(EV_KEY, input->evbit);
    
    // 设置功能键
    __set_bit(KEY_ESC, input->keybit);
    __set_bit(KEY_BACKSPACE, input->keybit);
    __set_bit(KEY_TAB, input->keybit);
    __set_bit(KEY_ENTER, input->keybit);
    __set_bit(KEY_LEFT, input->keybit);
    __set_bit(KEY_RIGHT, input->keybit);
    __set_bit(KEY_UP, input->keybit);
    __set_bit(KEY_DOWN, input->keybit);
    
    // 设置字母键
    for (i = KEY_A; i <= KEY_Z; i++) {
        __set_bit(i, input->keybit);
    }
    
    // 设置数字键
    for (i = KEY_0; i <= KEY_9; i++) {
        __set_bit(i, input->keybit);
    }

    // 设置符号键
    __set_bit(KEY_SPACE, input->keybit);
    __set_bit(KEY_MINUS, input->keybit);
    __set_bit(KEY_EQUAL, input->keybit);
    __set_bit(KEY_LEFTBRACE, input->keybit);
    __set_bit(KEY_RIGHTBRACE, input->keybit);
    __set_bit(KEY_SEMICOLON, input->keybit);
    __set_bit(KEY_APOSTROPHE, input->keybit);
    __set_bit(KEY_GRAVE, input->keybit);
    __set_bit(KEY_BACKSLASH, input->keybit);
    __set_bit(KEY_COMMA, input->keybit);
    __set_bit(KEY_DOT, input->keybit);
    __set_bit(KEY_SLASH, input->keybit);

    // 设置功能键(F1-F12)
    for (i = KEY_F1; i <= KEY_F12; i++) {
        __set_bit(i, input->keybit);
    }

    // 初始化定时器
    timer_setup(&kbd->timer, keyboard_timer_handler, 0);
    mod_timer(&kbd->timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));

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