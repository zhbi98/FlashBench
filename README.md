# FlashRel

[![License](https://img.shields.io/badge/license-Apache%202-green.svg)](./LICENSE.txt)

FlashRel（`flash_stress`）是一个 Flash 可靠性（Reliability）测试框架，用来验证 Flash 在目标板的驱动、SPI 时钟和走线下能否被可靠地读写。

它反复对一块空闲的 Flash 区域做"擦除 → 查空 → 写入 → 读回比较"，统计通过和失败的次数，并在出错时给出具体地址和数据。测的是"目标板实际读写路径 + Flash 芯片"这一整条链路，而不只是芯片本身。

## 工作方式

```text
应用启动测试 → 轮询推进 → read / prog / erase / busy → Flash 驱动 → Flash
应用显示读数 ← 计数、流量、耗时、错误 ← 读回比较
```

测试被拆成小步。应用在主循环或任务中持续轮询，每次轮询只做一小步就返回，不等待擦除和写入完成，所以可以一边测试一边刷新显示。

框架不绑定任何 Flash 驱动、地址或打印函数。读、写、擦除和忙查询通过注册的回调完成，测试区和参数通过配置传入。它只依赖 `stdint.h`、`stdbool.h`、`string.h`，不分配内存，也不依赖操作系统。

## 功能

- 三种测试模式：读写回归、驱动通信稳定性、擦写寿命。
- 伪随机测试数据，擦除后查空，写入后逐字节读回比较。
- 统计通过和失败轮数、读写擦字节流量、擦除和编程耗时。
- 出错时记录类型、轮次、地址、期望值、实际值和错误位数。
- 非阻塞运行，可随时启动和停止，阻塞式和非阻塞式驱动都能接入。
- 启动前检查测试区，发现非测试数据就拒绝运行，不擦除任何内容。

## 测试模式

| 模式 | 用途 | 每轮擦写量 | 测试区 |
| --- | --- | --- | --- |
| ROTATE | 读写回归 | `wr_secs` 个扇区 | 空闲区，结束后擦回空白 |
| READ | 驱动通信稳定性 | 只在第一轮擦写一次 | 空闲区，结束后保留数据 |
| ENDURANCE | 擦写次数和寿命 | 所有扇区 | 专用区，会被故意擦坏 |

ROTATE 每轮擦除、查空、写入、读回比较，写入位置轮流使用。READ 第一轮写入已知数据，之后只做随机读并与已知数据比较。ENDURANCE 每个扇区每轮都擦写一次，直到出错。

更换 Flash 型号、修改驱动或 SPI 时钟之后，先跑 READ 确认通信稳定，再跑 ROTATE 确认读写正常。排查偶发的数据错误时，让 READ 或 ROTATE 长时间运行。ENDURANCE 用来测擦写寿命，只在专用区上长期运行。

## 文档

这套文档按实际接入顺序展开：

1. **[框架工作原理](./docs/architecture.md)**：了解轮询推进、三种模式的流程和测试区保护。
2. **[平台移植](./docs/porting.md)**：对接 Flash 驱动，配置测试区，启动测试并持续轮询。
3. **[使用说明](./docs/usage.md)**：切换模式，读取测试数据并显示到屏幕。

结果不符合预期时查[问题定位](./docs/troubleshooting.md)。[接口参考](./docs/api-reference.md)用于查接口、配置项、状态和错误类型，[验证与测试](./docs/testing.md)说明主机测试和已验证的范围。

## 快速接入

接入需要一个支持 C99 的编译器，例程的日志宏使用 GNU 扩展，GCC 可用 `-std=gnu99`。Flash 驱动要能按任意长度、任意地址和任意缓冲区对齐读取，能发起页编程和扇区擦除，并能查询忙状态，或者擦写函数自己等待完成。

目标板上要留出一块空闲的测试区，运行 ENDURANCE 时另需一块专用区。应用要有一个执行上下文持续轮询，并提供一个真实递增的毫秒计数器。

### C 例程

`samples` 按功能展示对接与使用方法，将需要的片段放入已有驱动、平台层和业务代码。

| 文件 | 内容 |
| --- | --- |
| [port_sample.c](./samples/port_sample.c) | 读、写、擦除、忙查询和计时对接驱动 |
| [spinor_sample.c](./samples/spinor_sample.c) | 用标准 SPI NOR 指令实现驱动 |
| [run_sample.c](./samples/run_sample.c) | 测试区与参数、启动和轮询 |
| [mode_sample.c](./samples/mode_sample.c) | 启动、切换模式、停止和菜单选择 |
| [display_sample.c](./samples/display_sample.c) | 错误日志、结束报告和屏幕读数 |
| [freertos_sample.c](./samples/freertos_sample.c) | FreeRTOS 任务轮询和跨任务请求 |

文档和代码中有三种前缀。`flash_stress_*` 是框架接口，在 `src/` 中，不需要修改。`stress_*` 是例程对框架的包装，属于应用一侧。`board_*` 是板级驱动和显示函数，需要替换为实际实现。

### 接入步骤

1. **对接 Flash 驱动**：把 `port_sample.c` 中的 `board_flash_*` 换成实际驱动。还没有驱动时，用 `spinor_sample.c` 在 SPI 收发函数上实现它们。
2. **配置测试区**：按 Flash 手册和分区表修改 `run_sample.c` 中的扇区大小、页大小、测试区地址和扇区数。
3. **启动并持续轮询**：驱动和计时初始化完成后启动测试，在主循环中持续调用 `stress_process()`。
4. **切换模式和显示数据**：用 `stress_start(mode)` 切换模式，周期调用 `stress_show()` 刷新屏幕。

第 3 步的最小代码如下：

```c
#include "flash_stress.h"

/*Include the declarations of the stress_* functions of the application here*/

/*Call once from main, after the flash driver and the tick are ready*/
void app_stress_run(void)
{
    stress_start(FLASH_STRESS_MODE_ROTATE);

    for (;;) {
        stress_process();
        /*Refresh the display with stress_show(), for example every 500 ms*/
    }
}
```

已有主循环时，只需在循环前启动，把 `stress_process()` 放进原循环。轮询不能放进中断，调用间隔建议不超过几毫秒，间隔过长测试会明显变慢。

每一步的细节、FreeRTOS 的接法和不使用例程的接法见[平台移植](./docs/porting.md)。

## 运行效果

例程把读数整理成 6 行文字显示到屏幕，并在串口打印每轮的结果。下面是 READ 模式读到一位翻转时的屏幕和串口输出，取自主机上接模拟 Flash 的实际运行：

```text
READ  READ
Loop 10  Pass 9  Fail 1
Read 7MB  Write 56KB
First fail 10
Erase avg 45 max 46 ms
Error DATA @5F8820 6E>6A
```

```text
[FLASH STRESS] loop 9 DATA at 0x5F8820 expect 0x6E got 0x6A bits 1
[FLASH STRESS] loop 10: ok=10 err=1
[FLASH STRESS] READ end: ok=10 err=1 read=7MB
```

屏幕从上到下依次是模式和阶段、轮数、读写流量、首次失败的轮次、擦除耗时和最近一次错误。三种模式的完整输出和每一行的含义见[使用说明](./docs/usage.md#屏幕读数)。

## 注意事项

测试区必须是空闲区域，会被反复擦写。ROTATE 也消耗擦写寿命，NOR Flash 一般标称每扇区 10 万次，长时间运行时要留意。ENDURANCE 的区域会被故意擦坏，不要再用来做 ROTATE 或存放数据。

测试运行期间，应用其他代码不能访问同一片 Flash，保存参数等操作需要等待或跳过。框架内部没有锁，一个实例只能由一个执行上下文调用。

框架没有超时。忙查询一直返回 `true` 时，测试会停在等待状态，不再前进。

## 验证情况

| 对象 | 验证方式 | 结果 |
| --- | --- | --- |
| 框架 | 主机测试，模拟 Flash 加故障注入 | 全部通过 |
| port、run、mode、display 例程 | 主机上接模拟 Flash 运行 | 输出见使用说明 |
| spinor 例程 | 早期版本接模拟 SPI Flash 运行 | 当前版本未重跑 |
| freertos 例程 | 编译检查 | 未运行 |
| 真实硬件 | 无 | 需要在板子上确认 |

主机测试的运行方法和覆盖范围见[验证与测试](./docs/testing.md)。

## 许可证

本项目使用 [Apache License 2.0](./LICENSE.txt)。
