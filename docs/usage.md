# 切换模式与读取测试数据

[README](../README.md) · [工作原理](architecture.md) · [平台移植](porting.md) · **使用说明** · [问题定位](troubleshooting.md) · [接口参考](api-reference.md) · [验证与测试](testing.md)

完成[驱动对接](porting.md)后，测试已经能启动并持续轮询。本篇说明如何选择和切换测试模式，如何读取测试数据并显示到屏幕。

## 选择测试模式

三种模式的用途和擦写量见 [README 的测试模式](../README.md#测试模式)。例程为它们准备了两块测试区：

| 模式 | 例程测试区 | 结束后 |
| --- | --- | --- |
| ROTATE | 空闲区，每轮擦写 1 个扇区 | 擦回空白 |
| READ | 与 ROTATE 共用空闲区 | 保留已知数据 |
| ENDURANCE | 专用区，每轮每扇区擦写 1 次 | 保留数据和擦写次数 |

更换 Flash 或修改驱动后，先跑 READ 确认通信稳定，再跑 ROTATE 确认读写正常。ENDURANCE 只在专用区上长期运行。

## 启动与切换

[mode_sample.c](../samples/mode_sample.c) 提供四个接口。`stress_start(mode)` 启动测试或切换到另一种模式，`stress_stop()` 停止测试并取消等待中的切换。`stress_select(item)` 是菜单示例，0、1、2 分别启动三种模式，其他值停止。`stress_mode()` 返回正在运行或最近一次运行的模式。

在按键或菜单事件中直接调用即可：

```c
#include "flash_stress.h"

/*Include the declarations of the stress_* functions of the application here*/
#include <stdio.h>

void app_key_read(void)
{
    if (!stress_start(FLASH_STRESS_MODE_READ))
        printf("READ was not started\n");
}
```

`stress_start()` 返回 `true` 表示已经启动或切换已登记；模式取值非法或配置不合法时返回 `false`。

没有测试运行时，它直接启动。正在运行时，它不会立刻打断当前测试，而是等它收尾。ROTATE 跑完当前一轮，再把用过的扇区擦回空白。READ 读完当前扇区。ENDURANCE 跑完当前一轮，保证各扇区擦写次数一致。

收尾完成后，`stress_process()` 调用 `stress_on_end()`：先打印结束报告，再启动等待中的模式。FreeRTOS 中由其他任务调用 `stress_request(mode)`，切换在测试任务中执行。

## 读取测试数据

例程通过 `stress_object()` 取得测试实例，用框架接口读取数据。这些接口只读取实例中的计数，不访问 Flash，不影响测试。

`flash_stress_state()` 返回当前阶段。`flash_stress_loop()`、`flash_stress_ok_cnt()` 和 `flash_stress_err_cnt()` 分别返回已完成、通过和失败的轮数。`flash_stress_cycles()` 返回 ENDURANCE 累计的擦写次数，`flash_stress_last_err()` 返回最近一次错误。

流量和耗时在 `flash_stress_stat()` 返回的结构体中。`rd_bytes`、`wr_bytes`、`er_bytes` 是累计读、写、擦的字节数，都是 64 位。`first_fail` 是首次失败的轮次或擦写次数，0 表示没有失败。`er_time` 和 `pg_time` 是擦除和页编程的耗时，需要接计时，平均值为 `sum / cnt`。

各接口和字段的完整说明见[接口参考](api-reference.md#读数接口)。

## 屏幕读数

[display_sample.c](../samples/display_sample.c) 的 `stress_show()` 把读数整理成 6 行文字，逐行交给 `board_display_line(row, text)`。把这个函数换成 LCD 绘制或 GUI 标签即可，例如 LVGL 中对每行调用一次 `lv_label_set_text()`。周期调用，例如每 500 ms 一次。

本节和下一节的输出都是例程在主机上接模拟 Flash 实际运行得到的。模拟 Flash 的扇区擦除约 45 ms，页编程约 0.7 ms，测试区和参数为例程默认值，错误由模拟 Flash 故意注入。真实硬件上的数值会不同。

### ROTATE 运行中

```text
ROTATE  VERIFY
Loop 8  Pass 8  Fail 0
Read 220KB  Write 36KB
First fail 0
Erase avg 45 max 46 ms
Error None
```

第一行是模式和当前阶段。阶段会在 `CHECK`、`ERASE`、`BLANK`、`WRITE`、`VERIFY`、`READ`、`CLEAN` 之间变化，结束后停在 `DONE`，测试区不可用时停在 `ABORT`。

第二行是已完成、通过、失败的轮数。第三行是累计的读写字节数，不足 1 MB 时以 KB 显示。第四行是首次失败的轮次，0 表示还没有失败。

第五行是扇区擦除的平均和最长耗时，可以和 Flash 手册对照，为 0 说明没有接计时。第六行是最近一次错误，没有错误时为 `None`。

### READ 发现错误

```text
READ  READ
Loop 10  Pass 9  Fail 1
Read 7MB  Write 56KB
First fail 10
Erase avg 45 max 46 ms
Error DATA @5F8820 6E>6A
```

READ 只在第一轮写入一次已知数据，所以写入量停在 56 KB，也就是 14 个 4 KB 扇区，读取量则不断增加。

最后一行的错误依次是类型、Flash 地址、期望值 > 实际值。这里地址 `0x5F8820` 上期望读到 `0x6E`，实际读到 `0x6A`，只有一位不同。错误会一直保留在屏幕上，直到出现下一次错误或重新启动测试。

### ENDURANCE 擦写到失效

```text
ENDURANCE  DONE
Loop 1510  Pass 1500  Fail 10
Read 23MB  Write 11MB
Cycle 1510  First fail 1501
Erase avg 48 max 52 ms
Error ERASE @5FF1A3 FF>EF
```

ENDURANCE 的第四行多出 `Cycle`，是每个扇区累计的擦写次数，包括以前运行的次数。`First fail 1501` 表示第 1501 次擦写时首次失败，之后又失败了 9 次，达到例程设定的 10 次后自动结束。

模拟 Flash 的擦除时间随擦写次数变长，所以平均耗时由开始时的 45 ms 升到 48 ms；真实 Flash 磨损后也有同样的趋势。最后的错误是 `ERASE`：擦除后地址 `0x5FF1A3` 读回 `0xEF`，有一位已经擦不掉了。

### 测试区不可用

```text
ROTATE  ABORT
Loop 0  Pass 0  Fail 0
Read 16KB  Write 0KB
First fail 0
Erase avg 0 max 0 ms
Error AREA USED @5F3010 FF>75
```

启动检查在地址 `0x5F3010` 发现了非测试数据，测试停在 `ABORT`。写入量为 0，说明没有擦除或写入任何内容。

## 串口日志

`stress_on_loop()` 在每轮结束时打印一行，`stress_report()` 在测试结束时打印结果。下面是 ROTATE 从启动到手动停止的日志：

```text
[FLASH STRESS] loop 0: ok=1 err=0
[FLASH STRESS] loop 1: ok=2 err=0
[FLASH STRESS] loop 2: ok=3 err=0
...
[FLASH STRESS] loop 54: ok=55 err=0
[FLASH STRESS] ROTATE end: ok=55 err=0 read=1MB
```

`stress_on_error()` 在每检测到一次错误时打印轮次、错误类型、地址、期望值、实际值和错误位数。它收到的 `err` 只在回调期间有效，需要保存时复制内容。下面是 READ 读到一位翻转后停止的日志：

```text
[FLASH STRESS] loop 9 DATA at 0x5F8820 expect 0x6E got 0x6A bits 1
[FLASH STRESS] loop 10: ok=10 err=1
[FLASH STRESS] READ end: ok=10 err=1 read=7MB
```

ENDURANCE 下每 1000 轮才打印一行进度，避免刷屏。结束时多打印一行累计擦写次数和首次失败的次数：

```text
[FLASH STRESS] loop 999: ok=1000 err=0
[FLASH STRESS] loop 1500 ERASE at 0x5FE1A3 expect 0xFF got 0xEF bits 1
[FLASH STRESS] loop 1500 ERASE at 0x5FF1A3 expect 0xFF got 0xEF bits 1
...
[FLASH STRESS] ENDURANCE end: ok=1500 err=10 read=23MB
[FLASH STRESS] cycles=1510 first fail=1501
```

测试区不可用时只打印一行中止原因。运行中切换模式时，先打印当前测试的结束报告，再开始新模式：

```text
[FLASH STRESS] ROTATE abort: AREA USED at 0x5F3010
```

```text
[FLASH STRESS] loop 7: ok=8 err=0
[FLASH STRESS] ROTATE end: ok=8 err=0 read=216KB
[FLASH STRESS] loop 0: ok=1 err=0
[FLASH STRESS] READ end: ok=1 err=0 read=737KB
```

日志和屏幕上的错误名称含义如下：

| 错误名称 | 含义 |
| --- | --- |
| `DATA` | 读回的数据和写入的不一致 |
| `ERASE` | 擦除后不是全 `0xFF` |
| `OVERRUN` | READ 模式下驱动写出了缓冲区范围 |
| `READ IO` | 读函数返回失败 |
| `WRITE IO` | 写入函数返回失败 |
| `ERASE IO` | 擦除函数返回失败 |
| `AREA USED` | 测试区有别的数据，测试没有执行 |

错误位数少且分散，多为存储单元磨损；整字节或成片出错，多为通信问题。结果不符合预期时，按[问题定位](troubleshooting.md)中对应的现象检查。
