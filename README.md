# flash_stress — Flash 读写压力测试框架

## 用途

用来验证 Flash 能不能被可靠地读写。它反复对一块空闲的 Flash 区域做"擦除 → 查空 → 写入 → 读回比较"，统计通过和失败的次数，并在出错时给出具体地址和数据。

适用的场景：

- **更换 Flash 型号或供应商后**，确认新 Flash 在目标板的驱动、SPI 时钟和走线下读写正常。
- **修改了 Flash 驱动或 SPI 时钟后**，回归验证读写是否仍然正确。
- **排查偶发的数据错误**，长时间运行看是否出现读回不一致。
- **寿命测试**，在专用区域上持续擦写，记录擦写次数和首次失败的位置。

它测的是"目标板实际读写路径 + Flash 芯片"这一整条链路，而不只是芯片本身。

## 三种测试模式

由配置项 `mode` 选择，默认（0）是 ROTATE。

| 模式 | 用途 | 擦写 | 测试区要求 |
|---|---|---|---|
| `FLASH_STRESS_MODE_ROTATE` | 读写回归：擦除 → 查空 → 写入 → 读回比较，写入位置轮流使用 | 每轮 `wr_secs` 个扇区 | 空闲区域，结束后擦回空白 |
| `FLASH_STRESS_MODE_READ` | **驱动通信稳定性**：写一次已知数据，之后一直读 | 每次运行每扇区 1 次 | 空闲区域，结束后保留已知数据 |
| `FLASH_STRESS_MODE_ENDURANCE` | **擦写次数和寿命**：测试区每个扇区每轮都擦写一次，直到出错 | 每轮所有扇区 | 专用区域，会被故意擦坏 |

**READ（驱动通信稳定性）**

- 启动检查和 ROTATE 相同（测试区必须是空白或测试数据）。第一轮把每个扇区擦写一次已知的伪随机数据，之后每轮只读，和已知数据比较。每次运行每个扇区只擦写 1 次，相对 10 万次的寿命可以忽略。结束后保留数据，下次启动时被识别为测试数据，再写一次。
- 一定要写已知数据：如果只读不写，空白区域读到的全是 `0xFF`，连 Flash 完全不响应、MISO 被上拉成全 `0xFF` 都会显示通过；只和"自己上次读到的内容"比较，也发现不了每次都错得一样的系统性错误（例如 dummy 周期配错导致整体错位）。和已知数据比较，这两类问题都能发现。
- 期望数据由扇区地址算出，不需要额外缓冲区。
- 每个扇区每轮做 `reads` 次随机读。随机读混合了整扇区、1～16 字节的短读、约一页长度的跨页读、任意长度，起始地址任意，目标缓冲区故意 0～3 字节不对齐。这些是驱动最常出错的地方（分段、FIFO 边界、DMA 对齐、长度为奇数）。
- 目标范围外放保护字节，驱动写出范围（DMA 越界、多写一个字节）报 `READ_OVERRUN`；目标范围内预填期望值的反码，驱动"返回成功但没填数据"一定会被发现。

**ENDURANCE（擦写次数和寿命）**

- 每轮对测试区的每个扇区依次做：擦除 → 查空 → 写入 → 读回比较。一轮就是每个扇区一次擦写循环，所以轮数 = 擦写次数。
- 写入的数据奇数轮是伪随机数，偶数轮全为 `0x00`。随机数据每轮只编程约一半的位；全 `0x00` 保证每个位都经历完整的编程-擦除，擦写次数才名副其实。
- 每个扇区开头的 16 字节记录本次的擦写次数。测试结束后**不擦除**测试区；下次启动时从扇区头读回次数继续累计（`stat.cycle_base`），断电、复位、多次分段运行不会丢掉已经做过的次数。换新芯片时测试区是空白的，自动从 0 开始。
- 出错时不中断本轮，其余扇区照常擦写，保证各扇区次数一致。记录首次失败的次数（`stat.first_fail`）、出错的字节数和位数；`max_errs` 轮失败后自动结束。
- 注册了 `tick` 时记录擦除和页编程耗时（平均、最小、最大）。擦除时间随磨损变长，是寿命的早期信号。
- 测试区至少用 2 个扇区：一次只擦一个扇区，断电时另一个扇区的扇区头还在，次数不会丢。

## 阅读顺序

1. [平台移植与 Flash 驱动对接](./docs/porting.md)：接好读写擦除和计时，启动测试并持续轮询。
2. [切换模式与读取测试数据](./docs/usage.md)：切换三种模式，读取计数、流量、耗时和错误并显示到屏幕。

按需查阅：本文的[参考](#参考)一节列出全部接口、配置项、状态和错误类型。

## 快速接入

### 接入要求

- C99；例程的日志宏使用 GNU 扩展 `##__VA_ARGS__`，GCC 可使用 `-std=gnu99`。
- 驱动能按任意长度、任意地址和任意缓冲区对齐读取，能发起页编程和扇区擦除，能查询忙状态，或者擦写函数本身等待完成。
- 一块空闲的测试区；运行 ENDURANCE 时另需一块专用区。
- 一个执行上下文持续轮询；框架不分配内存、不创建线程，回调在轮询中执行。

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

已有主循环时，只需在循环前启动，把 `stress_app_process()` 放进原循环。FreeRTOS 中由一个任务轮询，其他任务通过 `stress_app_request()` 切换模式，见 [FreeRTOS 任务对接](./docs/porting.md#freertos-任务对接)。

## 实现的功能

**测试能力**

- 擦除后检查是否全为 `0xFF`，写入后读回逐字节比较。
- 测试数据是伪随机数，每一轮、每个地址都不同，能发现固定数据测不出的位错误。
- 每轮写入量可配置为 1 到 n 个扇区，最小单位是 1 个扇区。
- 每次写入后可以读回比较多遍，在不增加擦写次数的前提下加大读取的测试量。
- 可以跑固定轮数，也可以不限轮数一直跑到手动停止。

**统计和反馈**

- 通过轮数、失败轮数、已完成轮数。
- 字节流量统计：累计读取、写入、擦除的字节数（64 位，不会溢出）。
- 错误详情：错误类型、出错轮次、第一个出错字节的地址、期望值、实际值、出错字节数和位数。最近一次错误会一直保留，便于事后查看。
- 当前所处阶段（检查、擦除、查空、写入、校验、读取、清理、完成、中止），可用于显示。
- 每轮结束和每次出错都有回调通知，由使用方决定打印还是显示。

**运行方式**

- 非阻塞：测试被拆成小步，每次调用只做一小步就返回，不等待 Flash 的擦除和写入完成。可以放在裸机主循环或 RTOS 任务里，一边测一边刷新显示。
- 可随时启动和停止。停止时会跑完当前这一轮，ROTATE 还会把测试区擦回空白。

**安全保护**

- 启动前检查测试区：每个扇区必须是空白的，或者是上次测试留下的数据，否则拒绝运行，不擦除任何内容，避免误擦有用的数据。三种模式都做这项检查。
- 测试区内的多个写入位置轮流使用，分摊擦写次数（ROTATE）。
- 测试结束后把用过的扇区和启动时发现的遗留测试数据都恢复为空白（ROTATE）。ENDURANCE 保留数据以便累计次数。
- 测试中途断电后，下次启动能识别遗留的测试数据并正常继续。断电正好发生在擦除或第一页写入过程中时，那个扇区的内容无法识别，启动会以 `AREA USED` 中止；确认测试区确实空闲后，用 `force = true` 启动一次即可。
- 启动时先等待 Flash 空闲，不会在别的擦写还没完成时就去读。

**可移植性**

- 框架不绑定任何 Flash 驱动、地址或打印函数，读、写、擦除、忙状态查询都通过注册的回调完成，地址和大小通过配置传入。
- 只依赖 `stdint.h`、`stdbool.h`、`string.h`，不依赖操作系统。
- 阻塞式驱动和非阻塞式驱动都能接入。

## 直接使用框架接口

拷走 `src/flash_stress.c` 和 `src/flash_stress.h`，然后做三件事：实现底层函数、填配置、启动并周期调用 `poll`。按功能拆分的完整片段见 [C 例程](#c-例程)。

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
        flash_stress_poll(&stress);           /* 每次只推进一小步 */
        /* 其他工作、刷新显示 ... */
    }
}
```

需要一次性阻塞跑完时，循环调用到返回 `false` 即可：`while (flash_stress_poll(&stress)) { }`

底层函数的要求、配置项约束和全部接口见后面的参考部分。

## 目录

| 目录 | 内容 |
|---|---|
| `src/` | 框架：`flash_stress.c`、`flash_stress.h`，测试流程、计数、错误记录、流量和耗时统计 |
| `samples/` | 按功能拆分的接入例程，见 [C 例程](#c-例程) |
| `docs/` | [平台移植](./docs/porting.md)和[使用说明](./docs/usage.md) |
| `test/` | 框架的主机测试：`sim_norflash.c` 在内存模拟 Flash 上注入故障，验证三种模式 |

## 测试流程

**ROTATE** 以每轮写 `wr_secs` 个扇区为例，一轮依次做：

1. 逐个擦除这 `wr_secs` 个扇区。
2. 逐个扇区读回，检查全为 `0xFF`。
3. 写入测试数据，每次调用写 `prog_size` 字节，直到写满。
4. 逐个扇区读回并与写入的数据比较，重复 `reads` 遍。

全部通过则通过计数加 1；任何一步失败则失败计数加 1，记录错误详情，然后进入下一轮。

- **测试数据**：每个扇区开头是 16 字节扇区头（8 字节标记 + 擦写次数 + 次数取反），其余是伪随机数。
- **写入位置**：测试区按 `wr_secs` 划分成若干位置轮流使用；不能整除时末尾剩余的扇区不使用。
- **启动时**：先等 Flash 空闲，再把测试区逐扇区读一遍，确认每个扇区是空白或带测试标记，否则中止。
- **结束时**：把用过的扇区和启动时发现的遗留测试数据擦回空白。

**READ** 启动检查同 ROTATE。之后一轮依次处理每个扇区：由地址生成该扇区的已知数据；第一轮先擦除 → 查空 → 写入（只此一次）；然后做 `reads` 次随机读与已知数据比较。出错时记录，然后继续下一个扇区。停止请求在当前扇区处理完后生效。结束后保留数据。

**ENDURANCE** 一轮依次处理测试区的每个扇区：擦除 → 查空 → 写入 → 读回比较 `reads` 遍，一个扇区完成后再处理下一个。出错时记录，然后继续下一个扇区。启动时先检查测试区，并从扇区头读回已有的擦写次数；结束时保留数据。

## 参考

### 底层函数（`flash_stress_ops_t`）

| 回调 | 原型 | 要求 |
|---|---|---|
| `read` | `bool (*)(uint32_t addr, uint8_t * buf, uint32_t len)` | 读出 `len` 字节，成功返回 `true`。ROTATE/ENDURANCE 每次读一个扇区；READ 的长度、地址和 `buf` 对齐都是随机的 |
| `prog` | `bool (*)(uint32_t addr, const uint8_t * buf, uint32_t len)` | 向已擦除区域写 `len` 字节（不超过 `prog_size`），发出指令后即可返回，但返回前必须已经发送或拷贝完 `buf` |
| `erase` | `bool (*)(uint32_t addr)` | 擦除 `addr` 起始的一个扇区，发出指令后即可返回 |
| `busy` | `bool (*)(void)` | 上一次擦除或写入仍在进行时返回 `true`。可选 |
| `on_error` | `void (*)(const flash_stress_err_t * err)` | 每检测到一次错误调用一次。可选 |
| `on_loop` | `void (*)(uint32_t loop, uint32_t ok_cnt, uint32_t err_cnt)` | 每轮结束调用一次。可选 |
| `tick` | `uint32_t (*)(void)` | 自由运行的计数器，单位任意，用于擦除和编程计时。可选。非阻塞驱动的耗时量到 `poll` 发现不忙为止，`poll` 调用越密越准 |

两种驱动都能接：

- **非阻塞驱动**：`prog` / `erase` 发完指令就返回，同时提供 `busy`。框架在 `busy` 返回 `true` 期间不会再访问 Flash。
- **阻塞驱动**：`prog` / `erase` 内部等到操作完成才返回，`busy` 填 `NULL`。此时单次 `poll` 的耗时取决于驱动。

### 配置项（`flash_stress_cfg_t`）

| 字段 | 含义 | 约束 |
|---|---|---|
| `base` | 测试区起始地址 | 必须是 `sec_size` 的整数倍 |
| `sec_size` | 扇区大小（字节） | 大于 16 |
| `sec_cnt` | 测试区扇区数 | 大于 0，测试区不能超出 32 位地址空间 |
| `wr_secs` | ROTATE 每轮写入的扇区数，其他模式忽略 | 1 到 `sec_cnt`；填 0 按 1 处理 |
| `prog_size` | 每次 `prog` 调用的字节数；READ 用它作页大小来选读长度 | 不超过并整除 `sec_size`；填 0 按 `sec_size` 处理 |
| `reads` | 每次写入后读回比较的遍数；READ 为每个扇区每轮的随机读次数 | 填 0 按 1 处理 |
| `wbuf`、`rbuf` | 写、读缓冲区；READ 用 `wbuf` 存期望数据 | 各 `sec_size` 字节，不能为 `NULL` |
| `mode` | 测试模式 | `FLASH_STRESS_MODE_ROTATE`（0）、`_READ`、`_ENDURANCE` |
| `max_errs` | 失败多少轮后自动结束 | 0 表示出错不结束 |
| `force` | 跳过测试区内容检查，任何内容都可能被擦除 | 只在确认测试区空闲时用，例如擦除中途断电后 |

配置不合法时 `flash_stress_init()` 返回 `FLASH_STRESS_ERR_PARAM`。

### 框架接口

| 函数 | 作用 |
|---|---|
| `flash_stress_init(p, ops, cfg)` | 初始化实例并登记接口和配置，不访问 Flash |
| `flash_stress_start(p, loops)` | 启动一次全新的测试，计数和流量清零。`loops` 为 0 表示跑到 `stop` 为止 |
| `flash_stress_poll(p)` | 推进一小步。运行中返回 `true`，结束、中止或未启动返回 `false` |
| `flash_stress_stop(p)` | 请求停止：跑完当前这一轮（READ 为当前扇区），ROTATE 还会清理测试区 |
| `flash_stress_running(p)` | 是否仍需要继续调用 `poll` |
| `flash_stress_state(p)` | 当前状态，见下表 |
| `flash_stress_loop(p)` | 已完成的轮数 |
| `flash_stress_ok_cnt(p)` | 通过的轮数 |
| `flash_stress_err_cnt(p)` | 失败的轮数 |
| `flash_stress_last_err(p)` | 最近一次错误的详情，保留到下一次出错或重新启动 |
| `flash_stress_stat(p)` | 统计：字节流量 `rd_bytes`、`wr_bytes`、`er_bytes`（只统计成功的底层调用）；`cycle_base`（ENDURANCE 启动时读回的已有次数）；`first_fail`（首次失败的轮次或擦写次数，从 1 起，0 表示没有）；`er_time`、`pg_time`（擦除和编程耗时，需要 `tick`） |
| `flash_stress_cycles(p)` | ENDURANCE 每个扇区累计的擦写次数，包括以前运行的次数 |

### 状态（`flash_stress_state_t`）

| 状态 | 含义 | 每次 `poll` 的工作量 |
|---|---|---|
| `FLASH_STRESS_ST_IDLE` | 已初始化，未启动 | 无 |
| `FLASH_STRESS_ST_CHECK` | 检查测试区是否可用 | 读一个扇区 |
| `FLASH_STRESS_ST_ERASE` | 擦除本轮的扇区 | 发起一次扇区擦除 |
| `FLASH_STRESS_ST_BLANK` | 检查擦除后是否全空白 | 读一个扇区并比较 |
| `FLASH_STRESS_ST_PROG` | 写入测试数据 | 发起一次 `prog_size` 字节的写入 |
| `FLASH_STRESS_ST_VERIFY` | 读回比较 | 读一个扇区并比较 |
| `FLASH_STRESS_ST_READ` | READ 模式的随机读 | 读一次并与已知数据比较 |
| `FLASH_STRESS_ST_CLEAN` | ROTATE 结束前把用过的扇区擦回空白 | 发起一次扇区擦除 |
| `FLASH_STRESS_ST_DONE` | 测试结束。ROTATE 测试区已恢复空白，READ 和 ENDURANCE 保留数据 | 无 |
| `FLASH_STRESS_ST_ABORT` | 测试区不可用，未擦除任何内容 | 无 |

等待擦除或写入完成期间，`poll` 只调用一次 `busy` 就返回。

### 错误类型（`flash_stress_res_t`）

| 值 | 含义 |
|---|---|
| `FLASH_STRESS_ERR_PARAM` | 参数非法，或实例当前不能接受这次调用 |
| `FLASH_STRESS_ERR_NOT_BLANK` | 测试区里有非测试数据，测试中止 |
| `FLASH_STRESS_ERR_ERASE` / `ERR_PROG` / `ERR_READ` | 对应的底层函数返回失败 |
| `FLASH_STRESS_ERR_ERASE_VERIFY` | 擦除后读回不是全 `0xFF` |
| `FLASH_STRESS_ERR_DATA_VERIFY` | 写入后读回与写入的数据不一致 |
| `FLASH_STRESS_ERR_READ_OVERRUN` | READ 模式：读函数写出了请求的缓冲区范围。`addr` 是这次读的起始地址，`bad_cnt` 是被改写的保护字节数 |

READ 模式随机读和已知数据不一致时报 `FLASH_STRESS_ERR_DATA_VERIFY`。

比较类错误同时给出 `bad_cnt`（错误字节数）和 `bad_bits`（错误位数）。位数少而分散多是存储单元磨损，整字节或成片出错多是传输问题。

## 注意事项

- **测试区必须是空闲区域**，会被反复擦写。
- **擦写寿命**：ROTATE 每轮擦写 `wr_secs` 个扇区，各位置轮流。NOR Flash 一般标称每扇区 10 万次，不限轮数长时间运行时要留意；要测寿命请用 ENDURANCE，并放在专用区域。
- **ENDURANCE 的区域**：会被故意擦坏，只能用专用区域，并且不要再拿来做 ROTATE 或存放数据。次数只记在区域里的扇区头，手动擦除该区域会清零次数。
- **与其他 Flash 访问的关系**：测试运行期间，应用其他代码不能访问同一片 Flash，保存参数、读写文件等操作需要等待或跳过。
- **没有超时**：`busy` 一直返回 `true` 时测试会停在等待状态，`poll` 照常返回，但不再前进。
- **线程安全**：一个实例同一时间只能由一个执行上下文调用，框架内部没有锁。

## 验证情况

- 框架的主机测试在 `test/sim_norflash.c`：`gcc -std=c99 -Wall -Wextra -Isrc test/sim_norflash.c src/flash_stress.c -o sim_norflash && ./sim_norflash`。在一块带"忙"状态的内存模拟 Flash 上注入故障，覆盖启动时 Flash 忙、遗留数据清理、擦除中途断电与 `force`；READ 模式每次运行每扇区只擦写一次、链路断开读成全 `0xFF` 被发现、位翻转、缓冲区越界、DMA 不对齐、奇数长度少读、测试区有别的数据时中止；ENDURANCE 的磨损失效次数、各扇区次数一致、断电后累计次数、耗时统计、阻塞式驱动；以及非法配置。
- `samples` 的例程在主机上编译运行过：port、device、mode、show 接内存模拟 Flash，spinor 再接一块按 SPI 指令工作的模拟 Flash（检查写使能、忙状态和页回绕）。启动 ROTATE、运行中切换到 READ（等 ROTATE 跑完当前一轮并清理后才切换）、菜单切到 ENDURANCE、停止、测试区有别的数据时中止，两种模拟的输出一致，Flash 忙时没有收到任何读写擦指令，屏幕读数和串口输出与 [usage.md](./docs/usage.md) 中的示例一致。FreeRTOS 例程只做了编译检查。
- 在真实硬件上的 SPI 时序和驱动对接需要在板子上确认。
