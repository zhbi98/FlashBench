# 平台移植与 Flash 驱动对接

[README](../README.md) · [工作原理](architecture.md) · **平台移植** · [使用说明](usage.md) · [问题定位](troubleshooting.md) · [接口参考](api-reference.md) · [验证与测试](testing.md)

理解[工作原理](architecture.md)后，下一步是把框架接到实际的 Flash 驱动。本篇使用 [run_sample.c](../samples/run_sample.c) 作为接入骨架：完成本篇后，测试能够启动，轮询持续运行，结束时串口打印结果。

切换模式和显示测试数据在[下一篇](usage.md)说明，避免把驱动对接和业务显示混在一起。只想用框架、不用例程时，直接看[不使用例程时](#不使用例程时)。

## 第一步：选择需要的接入片段

框架核心是 `src/flash_stress.c`，`samples` 下的例程按职责拆分，完整清单见 [README 的 C 例程](../README.md#c-例程)。

本篇用到其中四个。`port_sample.c` 把框架需要的函数接到应用的驱动接口；没有现成驱动时，`spinor_sample.c` 用标准 SPI NOR 指令实现驱动。`run_sample.c` 定义测试区与参数，负责启动和轮询。`freertos_sample.c` 演示 FreeRTOS 下的任务轮询和跨任务请求。

跨文件调用的 `stress_*` 函数由应用自己的头文件声明。框架只依赖 `stdint.h`、`stdbool.h`、`string.h`，不分配内存，也不创建线程。

轮询的位置按平台选择：裸机放在主循环，FreeRTOS 放在一个专用任务。两者都用毫秒计数器计时，FreeRTOS 也可以用系统 tick 换算。

## 第二步：对接 Flash 驱动

[port_sample.c](../samples/port_sample.c) 把框架需要的五个函数接到应用的驱动接口。将 `board_flash_*` 和 `board_get_tick_ms` 替换为实际驱动：

```c
#include "flash_stress.h"

/*Include the declarations of the stress_* functions of the application here*/

extern int32_t board_flash_read(uint32_t addr, uint8_t * buf, uint32_t len);
extern int32_t board_flash_page_program(uint32_t addr, const uint8_t * buf, uint32_t len);
extern int32_t board_flash_sector_erase(uint32_t addr);
extern uint8_t board_flash_read_status(void);

bool stress_read(uint32_t addr, uint8_t * buf, uint32_t len)
{
    return board_flash_read(addr, buf, len) >= 0;
}

bool stress_prog(uint32_t addr, const uint8_t * buf, uint32_t len)
{
    return board_flash_page_program(addr, buf, len) >= 0;
}

bool stress_erase(uint32_t addr)
{
    return board_flash_sector_erase(addr) >= 0;
}

bool stress_busy(void)
{
    uint8_t status = board_flash_read_status();

    return (status & 0x01U) != 0U;
}
```

这几个函数应满足以下约定：

| 函数 | 必须做到 | 备注 |
| --- | --- | --- |
| 读取 | 任意长度、地址、缓冲区对齐都正确 | 走应用平时使用的驱动路径 |
| 页编程 | 返回前发送或复制完数据 | 可以不等编程结束就返回 |
| 扇区擦除 | 擦除地址所在的一个扇区 | 可以不等擦除结束就返回 |
| 忙查询 | 擦除或编程进行中返回 `true` | 阻塞式驱动填 `NULL` |
| 计时 | 返回自由运行的毫秒计数 | 只用框架时可填 `NULL` |

读取的长度从 1 字节到一个扇区。只有走平时的驱动路径，READ 模式才能测到驱动本身。页编程写入不超过一页的数据，框架会复用缓冲区，所以返回前必须取走数据。

`busy` 返回 `true` 期间，框架不会再访问 Flash，每次轮询只查询一次忙状态就返回。驱动失败时返回 `false`，框架记为 `READ IO`、`WRITE IO` 或 `ERASE IO` 错误。

### 还没有 Flash 驱动时

[spinor_sample.c](../samples/spinor_sample.c) 用标准 SPI NOR 指令实现上面四个 `board_flash_*` 函数，只需要应用提供三个 SPI 函数。`board_spi_select(active)` 控制片选，`true` 拉低，`false` 拉高。`board_spi_write(buf, len)` 和 `board_spi_read(buf, len)` 在片选期间发送和读入数据，失败时返回负数。

例程使用的指令：

| 操作 | 指令 | 说明 |
| --- | --- | --- |
| 读取 | `0x0B` 快速读 | 3 字节地址加 1 个 dummy 字节 |
| 页编程 | `0x06` 后 `0x02` | 先写使能，数据不能跨页 |
| 扇区擦除 | `0x06` 后 `0x20` | 先写使能，4 KB 扇区 |
| 忙查询 | `0x05` | 读状态寄存器，bit 0 为 BUSY |

使用前对照所用 Flash 的手册核对每条指令，SPI 时钟不能超过手册中 `0x0B` 指令的上限。指令使用 3 字节地址，容量超过 16 MB 的 Flash 要改用 4 字节地址指令，或进入 4 字节地址模式。

打开了块保护的 Flash 会静默忽略编程和擦除，测试会报 `ERASE` 或 `DATA` 错误。遇到这种情况，先在驱动初始化中清除保护位。

## 第三步：配置测试区

[run_sample.c](../samples/run_sample.c) 用宏定义测试区和参数，宏名前缀为 `STRESS_`。按 Flash 手册和应用的分区表修改：

| 宏 | 例程值 | 说明 |
| --- | --- | --- |
| `SEC_SIZE` | 4096 | 扇区擦除单位 |
| `PAGE_SIZE` | 256 | 页编程大小 |
| `BASE` | 0x5F0000 | ROTATE 与 READ 测试区的起始地址 |
| `SEC_CNT` | 14 | ROTATE 与 READ 测试区的扇区数 |
| `END_BASE` | 0x5FE000 | ENDURANCE 专用区的起始地址 |
| `END_SEC_CNT` | 2 | ENDURANCE 专用区的扇区数 |
| `ROTATE_READS` | 4 | ROTATE 每轮读回比较的遍数 |
| `READ_READS` | 32 | READ 每个扇区每轮的随机读次数 |
| `END_READS` | 1 | ENDURANCE 每次读回比较的遍数 |
| `END_MAX_ERRS` | 10 | ENDURANCE 失败多少次后结束 |
| `SLICE` | 5 | 每次轮询最多连续工作的时间，毫秒 |

测试区地址必须按扇区对齐。ROTATE 与 READ 的测试区会被擦写，必须确实空闲。ENDURANCE 专用区会被故意擦坏，不能与其他用途重叠，并且至少两个扇区，断电时另一个扇区还能保留擦写次数。

框架启动时逐扇区检查测试区，发现别的数据就中止并报 `AREA USED`，不擦除任何内容。

## 第四步：启动并持续轮询

驱动和计时初始化完成后启动一次测试，再在主循环中持续调用 `stress_process()`：

```c
#include "flash_stress.h"

/*Include the declarations of the stress_* functions of the application here*/

/*Call once from main, after the flash driver and the tick are ready*/
void app_stress_run(void)
{
    stress_start(FLASH_STRESS_MODE_ROTATE);

    for (;;) {
        stress_process();
        /*Run other non-blocking work here*/
    }
}
```

已有主循环时，只需在循环前启动，把 `stress_process()` 放进原循环。所有调用放在同一个执行上下文，不能放进中断。

`stress_running()` 为 `true` 期间，应用其他代码不能访问 Flash，保存参数、读写文件等操作需要等待或跳过。测试结束时 `stress_process()` 调用一次 `stress_on_end()`，打印结束报告。

### 每次调用做多少事

`stress_process()` 不按固定步数推进。每次调用一直工作，直到下面三种情况之一出现才返回：

1. **需要等待 Flash**：刚发出擦除或写入指令，`flash_stress_waiting()` 返回 `true`。
2. **时间片用完**：连续工作的时间达到 `STRESS_SLICE`。
3. **测试结束**：`flash_stress_poll()` 返回 `false`。

擦除或写入进行中，继续轮询不会前进，所以把时间让给主循环。读回比较不需要等待，用时间片限制连续占用的时间。

**调用间隔决定测试速度。** 每次擦除和每页写入都至少等到下一次调用，所以调用间隔建议不超过几毫秒。调用越频繁，测试跑得越快，也可以在主循环中连续调用。间隔几百毫秒调用一次时，一个 4 KB 扇区的 16 次页编程就要多等几秒。

**计时函数必须真实递增。** 时间片靠 `stress_tick()` 计算。它返回常数时，读回比较阶段不会因超时退出，主循环会被长时间占用。阻塞式驱动没有等待状态，完全依靠时间片退出。

时间片取应用能接受的最长连续占用时间。单次读一个扇区并比较的耗时超过时间片时，每次调用至少仍会做完这一步。另外，`flash_stress_waiting()` 会访问 Flash，只能在轮询的上下文中调用。

## FreeRTOS 任务对接

[freertos_sample.c](../samples/freertos_sample.c) 用一个任务独占测试，其他任务通过队列发送请求：

```c
#include "FreeRTOS.h"
#include "task.h"

/*Include the declarations of the stress_* functions of the application here*/
#include <stdio.h>

/*Call once after the flash driver is ready*/
void app_stress_task_init(void)
{
    if (!stress_task_create(512U, tskIDLE_PRIORITY + 1U))
        printf("Stress task was not created\n");
}

/*From the GUI task*/
void app_menu_read(void)
{
    stress_request(FLASH_STRESS_MODE_READ);
}
```

`FreeRTOSConfig.h` 中需要 `INCLUDE_vTaskDelay = 1`。测试任务每 2 ms 轮询一次，周期换算为 0 tick 时按 1 tick 处理。系统 tick 周期较长时，实际轮询间隔是一个 tick，测试会相应变慢。

访问同一片 Flash 的其他任务必须与测试任务互斥。`stress_show()` 只读数据，可以在 GUI 任务中调用；32 位 CPU 上 64 位字节数可能读到更新一半的值，只影响一次显示。

## 不使用例程时

不用例程时，拷走 `src/flash_stress.c` 和 `src/flash_stress.h`，然后做三件事：实现底层函数、填配置、启动并持续轮询。

```c
#include "flash_stress.h"

static flash_stress_t stress;
static uint8_t wbuf[4096];
static uint8_t rbuf[4096];

/* 1. 注册底层函数 */
static const flash_stress_ops_t ops = {
    .read = my_read,          /* 读 */
    .prog = my_prog,          /* 发起写入 */
    .erase = my_erase,        /* 发起擦除一个扇区 */
    .busy = my_busy,          /* 上一次擦除/写入是否还在进行，可选 */
    .on_error = my_on_error,  /* 出错通知，可选 */
    .on_loop = my_on_loop,    /* 每轮结束通知，可选 */
    .tick = my_tick,          /* 毫秒计数，用于擦写计时，可选 */
};

void test_begin(void)
{
    /* 2. 填配置 */
    flash_stress_cfg_t cfg = {0};

    cfg.base = 0x5F0000;      /* 测试区起始地址，按扇区对齐 */
    cfg.sec_size = 4096;      /* 扇区（最小擦除单位）大小 */
    cfg.sec_cnt = 16;         /* 测试区扇区数 */
    cfg.wr_secs = 1;          /* 每轮写入的扇区数，1..sec_cnt */
    cfg.prog_size = 256;      /* 每次写入调用的字节数，须整除 sec_size */
    cfg.reads = 4;            /* 每轮写入后读回比较的遍数 */
    cfg.wbuf = wbuf;          /* 各需要 sec_size 字节 */
    cfg.rbuf = rbuf;
    cfg.mode = FLASH_STRESS_MODE_ROTATE;  /* 或 _READ / _ENDURANCE */

    /* 3. 启动 */
    flash_stress_init(&stress, &ops, &cfg);   /* 只登记，不访问 Flash */
    flash_stress_start(&stress, 0);           /* 0 = 一直跑到 stop；N = 跑 N 轮 */
}

void main_loop(void)
{
    while (1) {
        /* 一直推进到需要等待 Flash 为止 */
        while (flash_stress_poll(&stress)) {
            if (flash_stress_waiting(&stress)) break;
        }
        /* 其他工作、刷新显示 ... */
    }
}
```

内层循环只在等待 Flash 时退出。使用阻塞式驱动（`busy` 为 `NULL`）或需要限制占用时间时，还要像 `run_sample.c` 那样加时间片。需要一次性阻塞跑完时，循环调用 `flash_stress_poll()` 到返回 `false` 即可。

底层函数的要求、配置项约束和全部接口见[接口参考](api-reference.md)。

## 下一步

对照[确认已经接通](troubleshooting.md#确认已经接通)检查测试是否正常运行，然后按[使用说明](usage.md)切换模式并显示读数。
