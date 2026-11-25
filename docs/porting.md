# 平台移植与 Flash 驱动对接

本篇使用 [device_sample.c](../samples/device_sample.c) 作为接入骨架：完成本篇后，测试能够启动，轮询持续运行，结束时串口打印结果。

切换模式和显示测试数据在[下一篇](usage.md)说明，避免把驱动对接和业务显示混在一起。

## 第一步：选择需要的接入片段

按职责查阅以下文件，将需要的逻辑放入应用对应模块：

| 文件 | 作用 |
| --- | --- |
| `src/flash_stress.c` | 测试框架核心 |
| `samples/port_sample.c` | 读、写、擦除、忙查询和毫秒计时到 Flash 驱动的包装 |
| `samples/spinor_sample.c` | 没有现成驱动时，用标准 SPI NOR 指令实现 `board_flash_*` |
| `samples/device_sample.c` | 各模式的测试区与参数、启动和轮询 |
| `samples/mode_sample.c` | 启动、切换和停止测试 |
| `samples/show_sample.c` | 错误日志、结束报告和屏幕读数 |
| `samples/freertos_sample.c` | FreeRTOS 任务轮询和跨任务请求 |
| 你的驱动源文件 | 实际的 SPI 初始化、读写擦除指令和状态寄存器读取 |

跨文件调用的 `stress_app_*` 函数由应用自己的头文件声明。框架只依赖 `stdint.h`、`stdbool.h`、`string.h`，不分配内存，也不创建线程。

| 平台 | 轮询位置 | 计时来源 |
| --- | --- | --- |
| 裸机 | 主循环调用 `stress_app_process()` | 毫秒计数器，只用于统计擦写耗时 |
| FreeRTOS | 一个任务调用 `stress_app_process()` | 同上，也可用系统 tick 换算 |

## 第二步：对接 Flash 驱动

[port_sample.c](../samples/port_sample.c) 把框架需要的五个函数接到应用的驱动接口。将 `board_flash_*` 和 `board_get_tick_ms` 替换为实际驱动：

```c
#include "flash_stress.h"

/*Include the declarations of the stress_app_* functions of the application here*/

extern int32_t board_flash_read(uint32_t addr, uint8_t * buf, uint32_t len);
extern int32_t board_flash_page_program(uint32_t addr, const uint8_t * buf, uint32_t len);
extern int32_t board_flash_sector_erase(uint32_t addr);
extern uint8_t board_flash_read_status(void);

bool stress_app_read(uint32_t addr, uint8_t * buf, uint32_t len)
{
    return board_flash_read(addr, buf, len) >= 0;
}

bool stress_app_prog(uint32_t addr, const uint8_t * buf, uint32_t len)
{
    return board_flash_page_program(addr, buf, len) >= 0;
}

bool stress_app_erase(uint32_t addr)
{
    return board_flash_sector_erase(addr) >= 0;
}

bool stress_app_busy(void)
{
    uint8_t status = board_flash_read_status();

    return (status & 0x01U) != 0U;
}
```

这几个函数应满足以下约定：

| 函数 | 必须满足的行为 |
| --- | --- |
| 读取 | 1 字节到一个扇区的任意长度、任意起始地址和任意缓冲区对齐都要正确；走应用平时使用的驱动路径，READ 模式才能测到驱动本身 |
| 页编程 | 向已擦除区域写不超过一页的数据；返回前必须发送或复制完数据，框架会复用缓冲区；可以不等编程结束就返回 |
| 扇区擦除 | 擦除地址所在的一个扇区；可以不等擦除结束就返回 |
| 忙查询 | 擦除或编程进行中返回 `true`；驱动的写和擦本来就等待完成时，接口表的 `.busy` 填 `NULL` |
| 计时 | 自由运行的毫秒计数，只用于统计耗时；不需要时 `.tick` 填 `NULL` |

`busy` 返回 `true` 期间，框架不会再访问 Flash，每次轮询只查询一次忙状态就返回。驱动失败时返回 `false`，框架记为 `READ IO`、`WRITE IO` 或 `ERASE IO` 错误。

### 还没有 Flash 驱动时

[spinor_sample.c](../samples/spinor_sample.c) 用标准 SPI NOR 指令实现上面四个 `board_flash_*` 函数，只需要应用提供三个 SPI 函数：

| 应用提供 | 作用 |
| --- | --- |
| `board_spi_select(bool active)` | `true` 拉低片选，`false` 拉高片选 |
| `board_spi_write(buf, len)` | 在当前片选期间发送数据，失败返回负数 |
| `board_spi_read(buf, len)` | 在当前片选期间读入数据，失败返回负数 |

例程使用的指令：

| 操作 | 指令 | 说明 |
| --- | --- | --- |
| 读取 | `0x0B` 快速读 | 3 字节地址加 1 个 dummy 字节，SPI 时钟不能超过手册中 `0x0B` 的上限 |
| 页编程 | `0x06` 写使能后 `0x02` | 数据不能跨页 |
| 扇区擦除 | `0x06` 写使能后 `0x20` | 4KB 扇区 |
| 忙查询 | `0x05` 读状态寄存器 | bit 0 为 BUSY |

指令使用 3 字节地址，容量超过 16MB 的 Flash 需要改用 4 字节地址指令或进入 4 字节地址模式。打开了块保护的 Flash 会静默忽略编程和擦除，测试会报 `ERASE` 或 `DATA` 错误，先在驱动初始化中清除保护位。每条指令都要对照所用 Flash 的手册核对。

## 第三步：配置测试区

[device_sample.c](../samples/device_sample.c) 用宏定义测试区和参数，按 Flash 手册和应用的分区表修改：

| 宏 | 例程值 | 说明 |
| --- | --- | --- |
| `STRESS_APP_SEC_SIZE` / `STRESS_APP_PAGE_SIZE` | 4096 / 256 | 扇区擦除单位和页编程大小 |
| `STRESS_APP_BASE` / `STRESS_APP_SEC_CNT` | 0x5F0000 / 14 | ROTATE 与 READ 使用的空闲区，会被擦写 |
| `STRESS_APP_END_BASE` / `STRESS_APP_END_SEC_CNT` | 0x5FE000 / 2 | ENDURANCE 专用区，会被故意擦坏 |
| `STRESS_APP_ROTATE_READS` / `STRESS_APP_READ_READS` / `STRESS_APP_END_READS` | 4 / 32 / 1 | 各模式每轮读回比较或随机读的次数 |
| `STRESS_APP_END_MAX_ERRS` | 10 | ENDURANCE 失败多少次后结束 |
| `STRESS_APP_STEPS` | 16 | 每次 `stress_app_process()` 推进的步数 |

测试区地址必须按扇区对齐，并确认确实空闲。框架启动时逐扇区检查，发现别的数据就中止并报 `AREA USED`，不擦除任何内容。ENDURANCE 区不能与其他用途重叠，至少两个扇区，断电时另一个扇区还能保留擦写次数。

## 第四步：启动并持续轮询

驱动和计时初始化完成后启动一次测试，再在主循环中持续调用 `stress_app_process()`：

```c
#include "flash_stress.h"

/*Include the declarations of the stress_app_* functions of the application here*/

/*Call once from main, after the flash driver and the tick are ready*/
void app_stress_run(void)
{
    stress_app_start(FLASH_STRESS_MODE_ROTATE);

    for (;;) {
        stress_app_process();
        /*Run other non-blocking work here*/
    }
}
```

已有主循环时，只需在循环前启动，把 `stress_app_process()` 放进原循环。每一步在 Flash 忙时立即返回，擦除期间主循环照常运行；调用越频繁，测试跑得越快。

所有调用放在同一个执行上下文，不能放进中断。`stress_app_running()` 为 `true` 期间，应用其他代码不能访问 Flash，保存参数、读写文件等操作需要等待或跳过。测试结束时 `stress_app_process()` 调用一次 `stress_app_on_end()`，打印结束报告。

## FreeRTOS 任务对接

[freertos_sample.c](../samples/freertos_sample.c) 用一个任务独占测试，其他任务通过队列发送请求：

```c
#include "FreeRTOS.h"
#include "task.h"

/*Include the declarations of the stress_app_* functions of the application here*/
#include <stdio.h>

/*Call once after the flash driver is ready*/
void app_stress_task_init(void)
{
    if (!stress_app_task_create(512U, tskIDLE_PRIORITY + 1U))
        printf("Stress task was not created\n");
}

/*From the GUI task*/
void app_menu_read(void)
{
    stress_app_request(FLASH_STRESS_MODE_READ);
}
```

`FreeRTOSConfig.h` 中需要 `INCLUDE_vTaskDelay = 1`。测试任务每 2 ms 轮询一次，周期换算为 0 tick 时按 1 tick 处理。访问同一片 Flash 的其他任务必须与测试任务互斥。`stress_app_show()` 只读数据，可以在 GUI 任务中调用；32 位 CPU 上 64 位字节数可能读到更新一半的值，只影响一次显示。

## 如何确认已经接通

- `stress_app_start()` 返回 `true`，串口持续输出 `loop N: ok=N err=0`。
- ROTATE 跑几轮后 `Fail` 仍为 0；停止后测试区读回全 `0xFF`。
- 屏幕上的 `Erase avg` 与 Flash 手册的扇区擦除典型时间接近；为 0 说明没有接计时。
- 启动后立即 `abort: AREA USED`：测试区里有别的数据，核对 `STRESS_APP_BASE` 和分区表。

这时按[切换模式与读取测试数据](usage.md)切换模式并显示读数。
