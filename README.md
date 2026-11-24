# flash_stress — Flash 读写压力测试框架

## 用途

用来验证 Flash 能不能被可靠地读写。它反复对一块空闲的 Flash 区域做"擦除 → 查空 → 写入 → 读回比较"，统计通过和失败的次数，并在出错时给出具体地址和数据。

适用的场景：

- **更换 Flash 型号或供应商后**，确认新 Flash 在本板的驱动、SPI 时钟和走线下读写正常。本框架就是在更换 Flash 后出现偶发读错时加入的。
- **修改了 Flash 驱动或 SPI 时钟后**，回归验证读写是否仍然正确。
- **排查偶发的数据错误**，长时间运行看是否出现读回不一致。
- **老化测试**，不限轮数持续擦写读。

它测的是"本板实际读写路径 + Flash 芯片"这一整条链路，而不只是芯片本身。

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
- 错误详情：错误类型、出错轮次、第一个出错字节的地址、期望值、实际值、该扇区出错字节数。最近一次错误会一直保留，便于事后查看。
- 当前所处阶段（检查、擦除、查空、写入、校验、清理、完成、中止），可用于显示。
- 每轮结束和每次出错都有回调通知，由使用方决定打印还是显示。

**运行方式**

- 非阻塞：测试被拆成小步，每次调用只做一小步就返回，不等待 Flash 的擦除和写入完成。可以放在裸机主循环或 RTOS 任务里，一边测一边刷新显示。
- 可随时启动和停止。停止时会跑完当前这一轮，并把测试区擦回空白。

**安全保护**

- 启动前检查测试区：每个扇区必须是空白的，或者是上次测试留下的数据，否则拒绝运行，不擦除任何内容，避免误擦有用的数据。
- 测试区内的多个写入位置轮流使用，分摊擦写次数。
- 测试结束后把用过的扇区恢复为空白。
- 测试中途断电后，下次启动能识别遗留的测试数据并正常继续。

**可移植性**

- 框架不绑定任何 Flash 驱动、地址或打印函数，读、写、擦除、忙状态查询都通过注册的回调完成，地址和大小通过配置传入。
- 只依赖 `stdint.h`、`stdbool.h`、`string.h`，不依赖操作系统。
- 阻塞式驱动和非阻塞式驱动都能接入。

**本工程已接入的部分**

- 对接了本机的 SPI0 NOR Flash。
- 在所有带手动调试菜单的机型里都加了一项（1022、1102、1052、1202），可以启动和停止，屏幕上实时显示各项读数。
- 测试运行期间，系统其他代码对 Flash 的普通读、写、擦除会被跳过，避免相互干扰。

## 使用方法

按使用者的不同分三种。

### 一、在机器上操作（菜单）

1. 进入手动调试模式，切换到最后一项，标题显示 `Flash Stress Test`。
2. CH1 增加方向：启动测试，一直循环运行。
3. CH1 减少方向：停止测试。跑完当前一轮并清理测试区后结束。
4. 退出调试模式时，如果测试还在运行，会自动停止。

屏幕上一行一项显示：

```
Flash Stress Test
Key: CH1+ Start / CH1- Stop
State: RUNNING  VERIFY
Loops: 12
Pass: 12
Fail: 0
Write: 48 KB
Read: 240 KB
Erase: 48 KB
Error: None
```

| 行 | 含义 |
|---|---|
| `State` | `RUNNING` / `STOPPED`，后面是当前阶段。结束后是 `DONE`；`ABORT` 表示测试区里有别的数据，测试没有执行 |
| `Loops` / `Pass` / `Fail` | 已完成轮数、通过轮数、失败轮数 |
| `Write` / `Read` / `Erase` | 累计写入、读取、擦除量。不足 1MB 显示 KB，超过后显示 MB |
| `Error` | 最近一次错误，例如 `DATA @5F3005 53>52`：类型、Flash 地址、期望值、实际值。没有错误时显示 `None` |

`Error` 的类型：`DATA`（写入后读回不一致）、`ERASE`（擦除后不是全 FF）、`READ IO` / `WRITE IO` / `ERASE IO`（底层函数报失败）、`AREA USED`（测试区有别的数据）。

判断结果：`Fail` 保持为 0 说明读写正常；`Fail` 增加时看 `Error` 行和串口输出定位问题。串口同时有以 `[FLASH STRESS]` 开头的详细记录。

注意：测试运行期间不要做需要存取 Flash 的操作（保存设置、存取波形和文件），这些操作在测试期间不会被执行。

### 二、在本工程代码里调用（应用层）

包含 `spi_flash_stress.h`，用下面这组接口，不需要关心框架和驱动细节：

```c
#include "spi_flash_stress.h"

/* 启动 */
spi_flash_stress_req_t req = {0};

req.base = SPI_FLASH_STRESS_BASE;        /* 测试区起始地址 */
req.sec_cnt = SPI_FLASH_STRESS_SEC_CNT;  /* 测试区扇区数 */
req.wr_secs = SPI_FLASH_STRESS_WR_SECS;  /* 每轮写入的扇区数 */
req.reads = SPI_FLASH_STRESS_READS;      /* 每轮读回比较的遍数 */
req.loops = 0;                           /* 0 = 跑到停止为止；N = 跑 N 轮 */
spi_flash_stress_start(&req);

/* 周期调用，推进测试并取读数 */
spi_flash_stress_info_t info = {0};

spi_flash_stress_poll();
spi_flash_stress_get(&info);             /* info.loop / ok_cnt / err_cnt / wr_bytes ... */

/* 停止 */
spi_flash_stress_stop();
```

| 函数 | 作用 |
|---|---|
| `spi_flash_stress_start(&req)` | 启动测试。成功返回 0；请求非法或已有测试在运行返回 -1 |
| `spi_flash_stress_poll()` | 推进一小步。运行中返回 1，结束返回 0；结束时在串口打印结果 |
| `spi_flash_stress_stop()` | 请求停止 |
| `spi_flash_stress_running()` | 测试是否在运行 |
| `spi_flash_stress_get(&info)` | 取实时读数：是否运行、阶段名称、轮数、通过和失败数、读写擦字节数、最近一次错误 |

`poll` 要被反复调用测试才会前进。调用得越频繁、每次连续调用的次数越多，测试跑得越快。菜单里是由 `Task_RealTimeProcess` 每次调用 16 步（`flash_stress_menu.c` 里的 `STRESS_POLLS`）。

默认参数在 `spi_flash_stress.h` 的宏里：

| 宏 | 值 | 含义 |
|---|---|---|
| `SPI_FLASH_STRESS_BASE` | `0x5F0000` | 测试区起始地址 |
| `SPI_FLASH_STRESS_SEC_CNT` | 16 | 测试区扇区数（共 64KB） |
| `SPI_FLASH_STRESS_SEC_SIZE` | 4096 | 扇区大小，对应 `0x20` 扇区擦除指令 |
| `SPI_FLASH_STRESS_PAGE_SIZE` | 256 | 每条页编程指令的字节数 |
| `SPI_FLASH_STRESS_WR_SECS` | 1 | 每轮写入的扇区数 |
| `SPI_FLASH_STRESS_READS` | 4 | 每轮读回比较的遍数 |

### 三、移植到其他工程（框架层）

拷走 `flash_stress.c` 和 `flash_stress.h`，然后做三件事：实现底层函数、填配置、启动并周期调用 `poll`。

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

底层函数的要求、配置项约束和全部接口见后面的参考部分。本工程的 `spi_flash_stress.c` 可以当作移植的样例。

## 文件

| 层 | 文件 | 内容 |
|---|---|---|
| 框架 | `src/flash_stress.c`、`include/flash_stress.h` | 测试流程、计数、错误记录、流量统计 |
| 应用 | `src/spi_flash_stress.c`、`include/spi_flash_stress.h` | 本工程（F1C100S + SPI0 NOR Flash）的驱动注册、默认参数、串口打印 |
| 菜单（公共） | `USR/OSC/main/flash_stress_menu.c`、`USR/OSC/include/flash_stress_menu.h` | 按键处理、屏幕显示、周期推进，各机型共用 |
| 菜单（机型） | `USR/OSC/main/param_correct_manual_1022_00.c` | 1022 机型的菜单项 `V1022_PRARAM_ITEM_FLASH_STRESS` |
| 菜单（机型） | `USR/OSC/main/param_correct_manual_3102_00.c` | 1102、1052、1202 机型共用的菜单项 `PRARAM_ITEM_FLASH_STRESS` |

## 测试流程

以每轮写 `wr_secs` 个扇区为例，一轮依次做：

1. 逐个擦除这 `wr_secs` 个扇区。
2. 逐个扇区读回，检查全为 `0xFF`。
3. 写入测试数据，每次调用写 `prog_size` 字节，直到写满。
4. 逐个扇区读回并与写入的数据比较，重复 `reads` 遍。

全部通过则通过计数加 1；任何一步失败则失败计数加 1，记录错误详情，然后进入下一轮。

- **测试数据**：每个扇区开头是 8 字节固定标记，其余是伪随机数。
- **写入位置**：测试区按 `wr_secs` 划分成若干位置轮流使用；不能整除时末尾剩余的扇区不使用。
- **启动时**：先把测试区逐扇区读一遍，确认每个扇区是空白或带测试标记，否则中止。
- **结束时**：把用过的扇区擦回空白。

## 参考

### 底层函数（`flash_stress_ops_t`）

| 回调 | 原型 | 要求 |
|---|---|---|
| `read` | `bool (*)(uint32_t addr, uint8_t * buf, uint32_t len)` | 读出 `len` 字节，成功返回 `true`。框架每次读一个扇区 |
| `prog` | `bool (*)(uint32_t addr, const uint8_t * buf, uint32_t len)` | 向已擦除区域写 `len` 字节（不超过 `prog_size`），发出指令后即可返回 |
| `erase` | `bool (*)(uint32_t addr)` | 擦除 `addr` 起始的一个扇区，发出指令后即可返回 |
| `busy` | `bool (*)(void)` | 上一次擦除或写入仍在进行时返回 `true`。可选 |
| `on_error` | `void (*)(const flash_stress_err_t * err)` | 每检测到一次错误调用一次。可选 |
| `on_loop` | `void (*)(uint32_t loop, uint32_t ok_cnt, uint32_t err_cnt)` | 每轮结束调用一次。可选 |

两种驱动都能接：

- **非阻塞驱动**：`prog` / `erase` 发完指令就返回，同时提供 `busy`。框架在 `busy` 返回 `true` 期间不会再访问 Flash。
- **阻塞驱动**：`prog` / `erase` 内部等到操作完成才返回，`busy` 填 `NULL`。此时单次 `poll` 的耗时取决于驱动。

### 配置项（`flash_stress_cfg_t`）

| 字段 | 含义 | 约束 |
|---|---|---|
| `base` | 测试区起始地址 | 必须是 `sec_size` 的整数倍 |
| `sec_size` | 扇区大小（字节） | 大于 8 |
| `sec_cnt` | 测试区扇区数 | 大于 0 |
| `wr_secs` | 每轮写入的扇区数 | 1 到 `sec_cnt`；填 0 按 1 处理 |
| `prog_size` | 每次 `prog` 调用的字节数 | 不超过并整除 `sec_size`；填 0 按 `sec_size` 处理 |
| `reads` | 每轮读回比较的遍数 | 填 0 按 1 处理 |
| `wbuf`、`rbuf` | 写、读缓冲区 | 各 `sec_size` 字节，不能为 `NULL` |

配置不合法时 `flash_stress_init()` 返回 `FLASH_STRESS_ERR_PARAM`。

### 框架接口

| 函数 | 作用 |
|---|---|
| `flash_stress_init(p, ops, cfg)` | 初始化实例并登记接口和配置，不访问 Flash |
| `flash_stress_start(p, loops)` | 启动一次全新的测试，计数和流量清零。`loops` 为 0 表示跑到 `stop` 为止 |
| `flash_stress_poll(p)` | 推进一小步。运行中返回 `true`，结束、中止或未启动返回 `false` |
| `flash_stress_stop(p)` | 请求停止：跑完当前这一轮，清理测试区后结束 |
| `flash_stress_running(p)` | 是否仍需要继续调用 `poll` |
| `flash_stress_state(p)` | 当前状态，见下表 |
| `flash_stress_loop(p)` | 已完成的轮数 |
| `flash_stress_ok_cnt(p)` | 通过的轮数 |
| `flash_stress_err_cnt(p)` | 失败的轮数 |
| `flash_stress_last_err(p)` | 最近一次错误的详情，保留到下一次出错或重新启动 |
| `flash_stress_stat(p)` | 字节流量：`rd_bytes`、`wr_bytes`、`er_bytes`，只统计成功的底层调用 |

### 状态（`flash_stress_state_t`）

| 状态 | 含义 | 每次 `poll` 的工作量 |
|---|---|---|
| `FLASH_STRESS_ST_IDLE` | 已初始化，未启动 | 无 |
| `FLASH_STRESS_ST_CHECK` | 检查测试区是否可用 | 读一个扇区 |
| `FLASH_STRESS_ST_ERASE` | 擦除本轮的扇区 | 发起一次扇区擦除 |
| `FLASH_STRESS_ST_BLANK` | 检查擦除后是否全空白 | 读一个扇区并比较 |
| `FLASH_STRESS_ST_PROG` | 写入测试数据 | 发起一次 `prog_size` 字节的写入 |
| `FLASH_STRESS_ST_VERIFY` | 读回比较 | 读一个扇区并比较 |
| `FLASH_STRESS_ST_CLEAN` | 结束前把用过的扇区擦回空白 | 发起一次扇区擦除 |
| `FLASH_STRESS_ST_DONE` | 测试结束，测试区已恢复空白 | 无 |
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

## 本工程的对接说明

- **Flash 指令**：读用 `0x0B` 快速读，写用 `0x02` 页编程，擦除用 `0x20` 扇区擦除，忙状态读状态寄存器的 BUSY 位。这些指令由应用层直接通过 SPI 底层函数发出，不经过工程原有的 `sys_spi_flash_read()` 等函数。
- **测试区**：`0x5F0000`–`0x5FFFFF`，是按代码里的分区表推断的空闲段（参数区到 `0x54B000` 结束，FPGA 配置从 `0x600000` 开始）。
- **与普通 Flash 访问的关系**：测试运行期间，驱动 `spi_flash.c` 的三个入口 `sys_spi_flash_read()`、`sys_spi_flash_write()`、`SPI_Flash_Erase_Sector()` 直接返回 -1，不执行操作。
  - 测试期间触发的保存不会写入，也不会在测试结束后补做。
  - 测试期间的读取拿不到数据，缓冲区不会被填充。
  - 这个判断只拦截测试开始之后新发起的访问；启动测试的瞬间如果恰好有访问正在进行，两者仍会冲突。
- **串口输出**：以 `[FLASH STRESS]` 开头，包括启动参数、每轮计数、错误详情、结束时的流量和总计数。

## 注意事项

- **测试区必须是空闲区域**，会被反复擦写。
- **擦写寿命**：每轮擦写 `wr_secs` 个扇区，各位置轮流。NOR Flash 一般标称每扇区 10 万次，不限轮数长时间运行时要留意。
- **没有超时**：`busy` 一直返回 `true` 时测试会停在等待状态，`poll` 照常返回，但不再前进。
- **线程安全**：一个实例同一时间只能由一个执行上下文调用，框架内部没有锁。
- **`stdbool.h`**：`flash_stress.h` 使用标准 `bool`。本工程 `USBD/inc/usb.h` 自己定义了 `bool`，两者不能出现在同一个源文件里，所以 `spi_flash_stress.h` 只用 `stdint.h` 的类型，供工程其他文件包含。

## 验证情况

- 框架逻辑用 gcc 编译后，在一块带"忙"状态的内存模拟 Flash 上验证过：不同的 `wr_secs`、阻塞式和非阻塞式驱动、注入位翻转、中途停止、测试区非空、非法配置、中断后重新开始。
- 应用层对接真实 SPI 的部分和菜单显示，需要在板子上确认。
