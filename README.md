# FlashBench

[![License](https://img.shields.io/badge/license-Apache%202-green.svg)](./LICENSE.txt)

FlashBench（`flash_stress`）是一个 Flash 读写压力测试框架，用来验证 Flash 在目标板的驱动、SPI 时钟和走线下能否被可靠地读写。

它反复对一块空闲的 Flash 区域做"擦除 → 查空 → 写入 → 读回比较"，统计通过和失败的次数，并在出错时给出具体地址和数据。测的是"目标板实际读写路径 + Flash 芯片"这一整条链路，而不只是芯片本身。

## 工作方式

```text
应用启动测试 → 轮询推进 → read / prog / erase / busy → Flash 驱动 → Flash
应用显示读数 ← 计数、流量、耗时、错误 ← 读回比较
```

测试被拆成小步，应用在主循环或任务中持续轮询，每次轮询只做一小步就返回，不等待擦除和写入完成。框架不绑定任何 Flash 驱动、地址或打印函数：读、写、擦除和忙查询通过注册的回调完成，测试区和参数通过配置传入。

## 适用场景

- **更换 Flash 型号或供应商后**，确认新 Flash 在目标板上读写正常。
- **修改了 Flash 驱动或 SPI 时钟后**，回归验证读写是否仍然正确。
- **排查偶发的数据错误**，长时间运行看是否出现读回不一致。
- **寿命测试**，在专用区域上持续擦写，记录擦写次数和首次失败的位置。

## 功能

- 三种测试模式：读写回归、驱动通信稳定性、擦写寿命。
- 伪随机测试数据，擦除后查空，写入后逐字节读回比较。
- 通过、失败轮数，读、写、擦字节流量，擦除和编程耗时统计。
- 错误详情：类型、轮次、地址、期望值、实际值、错误字节数和位数。
- 非阻塞运行，可随时启动和停止；阻塞式和非阻塞式驱动都能接入。
- 启动前检查测试区，发现非测试数据就拒绝运行，不擦除任何内容。
- 只依赖 `stdint.h`、`stdbool.h`、`string.h`，不分配内存，不依赖操作系统。

## 测试模式

由配置项 `mode` 选择，默认（0）是 ROTATE。

| 模式 | 用途 | 擦写量 | 测试区要求 |
| --- | --- | --- | --- |
| `FLASH_STRESS_MODE_ROTATE` | 读写回归：擦除 → 查空 → 写入 → 读回比较，写入位置轮流使用 | 每轮 `wr_secs` 个扇区 | 空闲区域，结束后擦回空白 |
| `FLASH_STRESS_MODE_READ` | 驱动通信稳定性：写一次已知数据，之后一直读 | 每次运行每扇区 1 次 | 空闲区域，结束后保留已知数据 |
| `FLASH_STRESS_MODE_ENDURANCE` | 擦写次数和寿命：每个扇区每轮都擦写一次，直到出错 | 每轮所有扇区 | 专用区域，会被故意擦坏 |

更换 Flash 或修改驱动后，先跑 READ 确认通信稳定，再跑 ROTATE 确认读写正常；ENDURANCE 只在专用区上长期运行。各模式的流程和设计考虑见[框架工作原理](./docs/architecture.md)。

## 阅读顺序

1. [框架工作原理](./docs/architecture.md)：了解轮询推进、三种模式的流程、测试数据和测试区保护。
2. [平台移植与 Flash 驱动对接](./docs/porting.md)：接好读写擦除和计时，启动测试并持续轮询。
3. [切换模式与读取测试数据](./docs/usage.md)：切换三种模式，读取计数、流量、耗时和错误并显示到屏幕。

按需查阅：[接口参考](./docs/api-reference.md)列出全部接口、配置项、状态和错误类型。

## 快速接入

### 接入要求

- C99；例程的日志宏使用 GNU 扩展 `##__VA_ARGS__`，GCC 可使用 `-std=gnu99`。
- 驱动能按任意长度、任意地址和任意缓冲区对齐读取，能发起页编程和扇区擦除，能查询忙状态，或者擦写函数本身等待完成。
- 一块空闲的测试区；运行 ENDURANCE 时另需一块专用区。
- 一个执行上下文持续轮询；框架不创建线程，回调在轮询中执行。
- 一个真实递增的计数器（例程为毫秒），用于限制每次轮询的占用时间和统计擦写耗时。

### C 例程

`samples` 按功能展示对接与使用方法。将需要的片段放入已有驱动、平台层和业务代码，`stress_app_*` 和 `board_*` 函数的声明由应用自己的头文件管理。

| 文件 | 内容 |
| --- | --- |
| [port_sample.c](./samples/port_sample.c) | 读、写、擦除、忙查询和毫秒计时对接 Flash 驱动 |
| [spinor_sample.c](./samples/spinor_sample.c) | 没有现成驱动时，用标准 SPI NOR 指令实现 `board_flash_*` |
| [device_sample.c](./samples/device_sample.c) | 各模式的测试区与参数、启动和主循环轮询 |
| [mode_sample.c](./samples/mode_sample.c) | 启动、运行中切换模式、停止和菜单选择 |
| [show_sample.c](./samples/show_sample.c) | 错误日志、结束报告和 6 行屏幕读数 |
| [freertos_sample.c](./samples/freertos_sample.c) | FreeRTOS 任务轮询和跨任务请求 |

### 接入步骤

1. **选择接入片段**

   根据平台选择驱动、轮询、模式和显示片段，将对应逻辑接入应用。框架接口见 `src/flash_stress.h`。

2. **对接 Flash 驱动**

   将 `board_flash_read`、`board_flash_page_program`、`board_flash_sector_erase`、`board_flash_read_status` 替换为实际驱动。读取要支持任意长度和对齐；页编程返回前要发送或复制完数据。还没有驱动时，用 `spinor_sample.c` 在 SPI 收发函数上实现它们。

3. **配置测试区**

   按 Flash 手册和分区表修改 `device_sample.c` 中的扇区大小、页大小、测试区地址和扇区数。

4. **启动并持续轮询**

   驱动和计时初始化完成后调用 `stress_app_start(FLASH_STRESS_MODE_ROTATE)`，在主循环中持续调用 `stress_app_process()`，不能放进中断。

5. **切换模式和显示数据**

   用 `stress_app_start(mode)` 切换模式，周期调用 `stress_app_show()` 刷新屏幕，测试结束时串口打印结果。

### 主循环对接

```c
#include "flash_stress.h"

/*Include the declarations of the stress_app_* functions of the application here*/

/*Call once from main, after the flash driver and the tick are ready*/
void app_stress_run(void)
{
    stress_app_start(FLASH_STRESS_MODE_ROTATE);

    for (;;) {
        stress_app_process();
        /*Refresh the display with stress_app_show(), for example every 500 ms*/
    }
}
```

已有主循环时，只需在循环前启动，把 `stress_app_process()` 放进原循环。

### 轮询间隔与测试速度

`stress_app_process()` 每次调用一直工作到下面任一条件成立才返回：

- **需要等待 Flash**：刚发出擦除或页编程指令，`flash_stress_waiting()` 返回 `true`。
- **时间片用完**：连续工作达到 `STRESS_APP_SLICE`（例程为 5 ms），把 CPU 让给应用的其他工作。

由此带来三点要求：

- **调用间隔决定测试速度。** 等待 Flash 的时间按调用间隔取整：每次擦除和每页写入都至少等到下一次调用。间隔几毫秒时影响很小；间隔几百毫秒时，一个 4KB 扇区的 16 次页编程就要多等几秒，测试会明显变慢。建议间隔不超过几毫秒，或在主循环中连续调用。
- **计时函数必须真实递增。** 时间片靠 `stress_app_tick()` 计算。它返回常数时，读回比较阶段不会因超时退出，只有遇到擦除或编程才返回，主循环会被长时间占用。
- **`flash_stress_waiting()` 会访问 Flash。** 它内部调用注册的 `busy` 函数，必须和 `flash_stress_poll()` 在同一个执行上下文中调用，不能像读数接口那样放到显示任务里。

### FreeRTOS 接入

[freertos_sample.c](./samples/freertos_sample.c) 用一个任务独占测试并每 2 ms 轮询一次，其他任务通过 `stress_app_request()` 切换模式。配置和任务创建见 [FreeRTOS 任务对接](./docs/porting.md#freertos-任务对接)。

### 切换模式与显示

| 例程接口 | 作用 |
| --- | --- |
| `stress_app_start(mode)` | 启动测试；正在运行时先让当前测试收尾，结束后自动启动新模式 |
| `stress_app_stop()` | 停止测试，并取消等待中的切换 |
| `stress_app_select(item)` | 菜单或按键示例：0、1、2 启动三种模式，其他值停止 |
| `stress_app_show()` | 把读数整理成 6 行文字交给显示函数，周期调用即可 |

读数接口只读取实例中的计数，不访问 Flash，不影响测试。完整说明见[切换模式与读取测试数据](./docs/usage.md)。

## 直接使用框架接口

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

> 上面的内层循环只在等待 Flash 时退出。阻塞式驱动（`busy` 为 `NULL`）或需要限制占用时间时，还要像 `device_sample.c` 那样加时间片。

需要一次性阻塞跑完时，循环调用到返回 `false` 即可：`while (flash_stress_poll(&stress)) { }`

底层函数的要求、配置项约束和全部接口见[接口参考](./docs/api-reference.md)。

## 接入检查

| 现象 | 优先检查 |
| --- | --- |
| `stress_app_start()` 返回 `false` | 模式取值、测试区是否按扇区对齐、页大小是否整除扇区大小、驱动函数是否都已注册 |
| 启动即 `abort: AREA USED` | 测试区是否空闲、地址宏是否算错 |
| 屏幕一直停在 `ERASE` 或 `WRITE` | 忙查询是否一直返回 `true`、状态寄存器读法、写使能；框架没有超时 |
| 测试能跑但一轮很慢 | 轮询的调用间隔是否过长，见[轮询间隔与测试速度](#轮询间隔与测试速度) |
| 测试期间主循环卡顿 | 计时函数是否递增、`STRESS_APP_SLICE` 是否过大、驱动的单次读写是否过慢 |
| `Erase avg` 为 0 | 没有接计时 |

接通的标志和更多现象见[如何确认已经接通](./docs/porting.md#如何确认已经接通)与[从现象定位问题](./docs/usage.md#从现象定位问题)。

## 注意事项

- **测试区必须是空闲区域**，会被反复擦写。
- **擦写寿命**：ROTATE 每轮擦写 `wr_secs` 个扇区，各位置轮流。NOR Flash 一般标称每扇区 10 万次，不限轮数长时间运行时要留意；要测寿命请用 ENDURANCE，并放在专用区域。
- **ENDURANCE 的区域**：会被故意擦坏，只能用专用区域，并且不要再拿来做 ROTATE 或存放数据。次数只记在区域里的扇区头，手动擦除该区域会清零次数。
- **与其他 Flash 访问的关系**：测试运行期间，应用其他代码不能访问同一片 Flash，保存参数、读写文件等操作需要等待或跳过。
- **轮询间隔**：调用间隔决定测试速度，计时函数必须真实递增，见[轮询间隔与测试速度](#轮询间隔与测试速度)。
- **没有超时**：`busy` 一直返回 `true` 时测试会停在等待状态，`poll` 照常返回，但不再前进。
- **线程安全**：一个实例同一时间只能由一个执行上下文调用，框架内部没有锁。`flash_stress_waiting()` 也属于这一类调用。

## 目录

| 目录 | 内容 |
| --- | --- |
| `src/` | 框架：`flash_stress.c`、`flash_stress.h`，测试流程、计数、错误记录、流量和耗时统计 |
| `samples/` | 按功能拆分的接入例程，见 [C 例程](#c-例程) |
| `docs/` | [工作原理](./docs/architecture.md)、[平台移植](./docs/porting.md)、[使用说明](./docs/usage.md)和[接口参考](./docs/api-reference.md) |
| `test/` | 框架的主机测试：`sim_norflash.c` 在内存模拟 Flash 上注入故障，验证三种模式 |

## 验证情况

- **框架主机测试**：`test/sim_norflash.c`，在一块带"忙"状态的内存模拟 Flash 上注入故障。

  ```text
  gcc -std=c99 -Wall -Wextra -Isrc test/sim_norflash.c src/flash_stress.c -o sim_norflash && ./sim_norflash
  ```

  覆盖启动时 Flash 忙、遗留数据清理、擦除中途断电与 `force`；READ 模式每次运行每扇区只擦写一次、链路断开读成全 `0xFF` 被发现、位翻转、缓冲区越界、DMA 不对齐、奇数长度少读、测试区有别的数据时中止；ENDURANCE 的磨损失效次数、各扇区次数一致、断电后累计次数、耗时统计、阻塞式驱动；以及非法配置。
- **例程**：在主机上编译运行过。port、device、mode、show 接内存模拟 Flash，spinor 再接一块按 SPI 指令工作的模拟 Flash（检查写使能、忙状态和页回绕）。启动 ROTATE、运行中切换到 READ（等 ROTATE 跑完当前一轮并清理后才切换）、菜单切到 ENDURANCE、停止、测试区有别的数据时中止，两种模拟的输出一致，Flash 忙时没有收到任何读写擦指令，屏幕读数和串口输出与[使用说明](./docs/usage.md)中的示例一致。FreeRTOS 例程只做了编译检查。
- **例程的推进方式**：上一条的运行结果来自按固定步数推进的版本。之后 `device_sample.c` 改为按等待状态和时间片推进，改动后只做了编译检查，没有重新接模拟 Flash 运行。
- **`flash_stress_waiting()`**：在带"忙"状态的模拟 Flash 上按"在等就不调用 `poll`"的方式跑过 3 轮 ROTATE，全部通过；这个测试没有并入 `test/sim_norflash.c`。
- **真实硬件**：SPI 时序和驱动对接需要在板子上确认。

## 许可证

本项目使用 [Apache License 2.0](./LICENSE.txt)。
