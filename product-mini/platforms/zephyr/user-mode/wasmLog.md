# Zephyr log in wasm app

## Zephyr log 101

**Compile time** filtering on **module level**.

Additional **run time** filtering on **module instance level**.

~~Printk support - printk message can be redirected to the logging.Not available on user mode~~

There are four severity levels available in the system: error, warning, info and debug. For each severity level the logging API (include/zephyr/logging/log.h) has **set of dedicated macros**. Logger API also has macros for logging data.

For each level the following set of macros are available:

- `LOG_X` for standard printf-like messages, e.g. LOG_ERR.

- `LOG_HEXDUMP_X` for dumping data, e.g. LOG_HEXDUMP_WRN.

- `LOG_INST_X` for standard printf-like message associated with the particular instance, e.g. LOG_INST_INF.

- `LOG_INST_HEXDUMP_X` for dumping data associated with the particular instance, e.g. LOG_INST_HEXDUMP_DBG

> **The inst level should be more useful in Zephyr**


## Zephyr log & WAMR log comparison

Zephyr log level

```C
#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_ERR  1
#define LOG_LEVEL_WRN  2
#define LOG_LEVEL_INF  3
#define LOG_LEVEL_DBG  4
```

WAMR log level, no usage of BH_LOG_LEVEL_FATEL and LOG_ERROR, so it should be okay

```C
typedef enum {
    BH_LOG_LEVEL_FATAL = 0,
    BH_LOG_LEVEL_ERROR = 1,
    BH_LOG_LEVEL_WARNING = 2,
    BH_LOG_LEVEL_DEBUG = 3,
    BH_LOG_LEVEL_VERBOSE = 4
} LogLevel;
```

Some CONFIG_LOG_FUNC_NAME_PREFIX_WRN=y