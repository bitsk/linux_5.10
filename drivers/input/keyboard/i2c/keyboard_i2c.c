#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

// 添加调试开关
#define DEBUG 1

#if DEBUG
#define kbd_dbg(dev, fmt, ...) \
    dev_dbg(dev, "%s: " fmt, __func__, ##__VA_ARGS__)
#define kbd_info(dev, fmt, ...) \
    dev_info(dev, "%s: " fmt, __func__, ##__VA_ARGS__)
#define kbd_err(dev, fmt, ...) \
    dev_err(dev, "%s: " fmt, __func__, ##__VA_ARGS__)
#else
#define kbd_dbg(dev, fmt, ...)
#define kbd_info(dev, fmt, ...)
#define kbd_err(dev, fmt, ...)
#endif

#define KEYBOARD_I2C_NAME "keyboard-i2c"
#define KEYBOARD_I2C_ADDR 0x5F
#define KEYBOARD_BUF_SIZE 1   // 每次读取1个字节
#define POLL_INTERVAL_MS 20   // 增加轮询间隔到20ms
#define MAX_RETRIES 3  // 最大重试次数

#define DEBUG_I2C 1  // 专门用于I2C调试的开关

// 添加I2C调试宏
#if DEBUG_I2C
#define i2c_dbg(dev, fmt, ...) \
    dev_info(dev, "I2C: " fmt, ##__VA_ARGS__)
#else
#define i2c_dbg(dev, fmt, ...)
#endif

struct keyboard_i2c {
    struct i2c_client *client;
    struct input_dev *input;
    struct timer_list timer;
    struct workqueue_struct *wq;  // 添加专用工作队列
    struct work_struct recovery_work;
    struct work_struct read_work;
    bool initialized;             // 添加初始化标志
};

static void keyboard_timer_handler(struct timer_list *t)
{
    struct keyboard_i2c *kbd = from_timer(kbd, t, timer);
    
    if (kbd->initialized && kbd->wq) {
        queue_work(kbd->wq, &kbd->read_work);
    } else {
        mod_timer(&kbd->timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
    }
}

static void keyboard_read_work(struct work_struct *work)
{
    struct keyboard_i2c *kbd = container_of(work, struct keyboard_i2c, read_work);
    u8 key_data;
    int ret;
    static unsigned long last_read_jiffies;
    static int total_reads;
    static int failed_reads;
    static int consecutive_failures = 0;

    if (!kbd->initialized)
        return;

    total_reads++;
    ret = i2c_master_recv(kbd->client, &key_data, KEYBOARD_BUF_SIZE);
    if (ret < 0) {
        failed_reads++;
        consecutive_failures++;
        
        if (consecutive_failures >= 3) {
            queue_work(kbd->wq, &kbd->recovery_work);
        }
        
        kbd_err(&kbd->client->dev, 
                "I2C read failed: %d (total: %d, failed: %d, consecutive: %d)\n",
                ret, total_reads, failed_reads, consecutive_failures);
        goto out;
    }

    consecutive_failures = 0;
    
    if (last_read_jiffies) {
        i2c_dbg(&kbd->client->dev, "Read interval: %u ms, success rate: %d%%\n",
                jiffies_to_msecs(jiffies - last_read_jiffies),
                ((total_reads - failed_reads) * 100) / total_reads);
    }
    last_read_jiffies = jiffies;

    if (key_data != 0) {
        int key_code;
        
        kbd_dbg(&kbd->client->dev, "Received raw key data: 0x%02x\n", key_data);
        
        // 映射自定义键值到标准Linux键值
        switch(key_data) {
        case 180: // 原KEY_LEFT
            key_code = KEY_LEFT;
            kbd_dbg(&kbd->client->dev, "Mapped to KEY_LEFT\n");
            break;
        case 181: // 原KEY_UP
            key_code = KEY_UP;
            kbd_dbg(&kbd->client->dev, "Mapped to KEY_UP\n");
            break;
        case 182: // 原KEY_DOWN
            key_code = KEY_DOWN;
            kbd_dbg(&kbd->client->dev, "Mapped to KEY_DOWN\n");
            break;
        case 183: // 原KEY_RIGHT
            key_code = KEY_RIGHT;
            kbd_dbg(&kbd->client->dev, "Mapped to KEY_RIGHT\n");
            break;
        case 27:  // ESC
            key_code = KEY_ESC;
            kbd_dbg(&kbd->client->dev, "Mapped to KEY_ESC\n");
            break;
        case 9:   // TAB
            key_code = KEY_TAB;
            kbd_dbg(&kbd->client->dev, "Mapped to KEY_TAB\n");
            break;
        case 13:  // ENTER
            key_code = KEY_ENTER;
            kbd_dbg(&kbd->client->dev, "Mapped to KEY_ENTER\n");
            break;
        case 127: // DEL
            key_code = KEY_BACKSPACE;
            kbd_dbg(&kbd->client->dev, "Mapped to KEY_BACKSPACE\n");
            break;
        default:
            if (key_data >= 32 && key_data <= 126) {
                key_code = key_data;
                kbd_dbg(&kbd->client->dev, "ASCII character: '%c'\n", key_data);
            } else {
                kbd_dbg(&kbd->client->dev, "Invalid key data: 0x%02x\n", key_data);
                goto out;
            }
            break;
        }

        input_report_key(kbd->input, key_code, 1);
        input_sync(kbd->input);
        input_report_key(kbd->input, key_code, 0);
        input_sync(kbd->input);
    }

out:
    if (kbd->initialized) {
        mod_timer(&kbd->timer, 
            jiffies + msecs_to_jiffies(consecutive_failures > 0 ? 
                POLL_INTERVAL_MS * 2 : POLL_INTERVAL_MS));
    }
}

static void keyboard_recovery_work(struct work_struct *work)
{
    struct keyboard_i2c *kbd = container_of(work, struct keyboard_i2c, recovery_work);
    
    if (!kbd->initialized)
        return;

    i2c_dbg(&kbd->client->dev, "Attempting bus recovery...\n");
    if (i2c_recover_bus(kbd->client->adapter) == 0) {
        i2c_dbg(&kbd->client->dev, "Bus recovery successful\n");
    } else {
        i2c_dbg(&kbd->client->dev, "Bus recovery failed\n");
    }
}

static int keyboard_i2c_probe(struct i2c_client *client,
                            const struct i2c_device_id *id)
{
    struct keyboard_i2c *kbd;
    struct input_dev *input;
    int error;
    int i;

    // 检查I2C适配器状态
    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        dev_err(&client->dev, "I2C functionality check failed!\n");
        return -ENODEV;
    }

    i2c_dbg(&client->dev, "Adapter name: %s\n", client->adapter->name);
    i2c_dbg(&client->dev, "Adapter nr: %d\n", client->adapter->nr);
    i2c_dbg(&client->dev, "Client address: 0x%02x\n", client->addr);

    // 尝试进行简单的I2C通信测试
    error = i2c_smbus_read_byte(client);
    if (error < 0) {
        dev_err(&client->dev, "I2C communication test failed: %d\n", error);
        return error;
    }
    i2c_dbg(&client->dev, "I2C communication test passed\n");

    kbd = devm_kzalloc(&client->dev, sizeof(*kbd), GFP_KERNEL);
    if (!kbd)
        return -ENOMEM;

    input = devm_input_allocate_device(&client->dev);
    if (!input)
        return -ENOMEM;

    kbd->client = client;
    kbd->input = input;
    
    // 创建专用工作队列
    kbd->wq = create_singlethread_workqueue("keyboard_i2c_wq");
    if (!kbd->wq) {
        dev_err(&client->dev, "Failed to create workqueue\n");
        return -ENOMEM;
    }

    // 初始化工作
    INIT_WORK(&kbd->recovery_work, keyboard_recovery_work);
    INIT_WORK(&kbd->read_work, keyboard_read_work);

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

    // 设置I2C客户端参数
    client->flags |= I2C_CLIENT_WAKE;
    client->adapter->retries = 3;
    client->adapter->timeout = HZ / 5;  // 200ms timeout

    i2c_dbg(&client->dev, "I2C adapter settings:\n");
    i2c_dbg(&client->dev, "  Retries: %d\n", client->adapter->retries);
    i2c_dbg(&client->dev, "  Timeout: %d ms\n", 
            jiffies_to_msecs(client->adapter->timeout));
    
    // 打印I2C适配器功能
    i2c_dbg(&client->dev, "I2C adapter capabilities:\n");
    i2c_dbg(&client->dev, "  I2C: %s\n", 
            i2c_check_functionality(client->adapter, I2C_FUNC_I2C) ? "yes" : "no");
    i2c_dbg(&client->dev, "  SMBUS BYTE: %s\n",
            i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE) ? "yes" : "no");
    i2c_dbg(&client->dev, "  SMBUS BYTE DATA: %s\n",
            i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA) ? "yes" : "no");
    i2c_dbg(&client->dev, "  SMBUS WORD DATA: %s\n",
            i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_WORD_DATA) ? "yes" : "no");
    i2c_dbg(&client->dev, "  SMBUS BLOCK DATA: %s\n",
            i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BLOCK_DATA) ? "yes" : "no");

    // 注册输入设备
    error = input_register_device(input);
    if (error) {
        dev_err(&client->dev, "Failed to register input device\n");
        del_timer_sync(&kbd->timer);
        return error;
    }

    i2c_set_clientdata(client, kbd);

    kbd_info(&client->dev, "Initializing M5Stack CardKB keyboard\n");
    kbd_dbg(&client->dev, "I2C adapter: %s\n", client->adapter->name);
    kbd_dbg(&client->dev, "I2C address: 0x%02x\n", client->addr);
    
    // 打印I2C适配器功能
    kbd_dbg(&client->dev, "I2C adapter functionality:\n");
    kbd_dbg(&client->dev, "  I2C: %s\n", 
            i2c_check_functionality(client->adapter, I2C_FUNC_I2C) ? "yes" : "no");
    kbd_dbg(&client->dev, "  SMBUS QUICK: %s\n",
            i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_QUICK) ? "yes" : "no");

    kbd_info(&client->dev, "Keyboard initialized successfully\n");
    kbd->initialized = true;
    return 0;
}

static int keyboard_i2c_remove(struct i2c_client *client)
{
    struct keyboard_i2c *kbd = i2c_get_clientdata(client);
    
    kbd->initialized = false;  // 防止新的工作被调度
    
    del_timer_sync(&kbd->timer);
    
    if (kbd->wq) {
        flush_workqueue(kbd->wq);
        destroy_workqueue(kbd->wq);
    }
    
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

// 添加模块参数来控制调试级别
static int debug_level;
module_param(debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Debug level (0=none, 1=error, 2=info, 3=debug)");