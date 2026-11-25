# 切换模式与读取测试数据

完成[驱动对接](porting.md)后，测试已经能启动并持续轮询。本篇说明如何选择和切换测试模式，如何读取测试数据并显示到屏幕。

## 选择测试模式

| 模式 | 用途 | 擦写量 | 例程测试区 |
| --- | --- | --- | --- |
| `FLASH_STRESS_MODE_ROTATE` | 读写回归：每轮擦除、写入、读回比较，写入位置轮流使用 | 每轮 1 个扇区 | 空闲区，结束后擦回空白 |
| `FLASH_STRESS_MODE_READ` | 驱动通信稳定性：第一轮写一次已知数据，之后只读 | 每次运行每扇区 1 次 | 空闲区，结束后保留已知数据 |
| `FLASH_STRESS_MODE_ENDURANCE` | 擦写次数与寿命：每轮每个扇区擦写一次，直到出错 | 每轮每扇区 1 次 | 专用区，断电后次数继续累计 |

更换 Flash 或修改驱动后，先跑 READ 确认通信稳定，再跑 ROTATE 确认读写正常；ENDURANCE 只在专用区上长期运行。

## 启动与切换

[mode_sample.c](../samples/mode_sample.c) 展示以下接口：

| 例程接口 | 作用 |
| --- | --- |
| `stress_app_start(mode)` | 没有测试运行时直接启动；正在运行时先停止当前测试，结束后自动启动新模式 |
| `stress_app_stop()` | 停止测试，并取消等待中的切换 |
| `stress_app_select(item)` | 菜单或按键示例：0、1、2 启动三种模式，其他值停止 |
| `stress_app_mode()` | 正在运行或最近一次运行的模式 |

在按键或菜单事件中直接调用：

```c
#include "flash_stress.h"

/*Include the declarations of the stress_app_* functions of the application here*/
#include <stdio.h>

void app_key_read(void)
{
    if (!stress_app_start(FLASH_STRESS_MODE_READ))
        printf("READ was not started\n");
}
```

切换不会立刻打断正在运行的测试，而是等它按下表收尾：

| 正在运行 | 停止生效的时机 |
| --- | --- |
| ROTATE | 跑完当前一轮，再把用过的扇区擦回空白 |
| READ | 读完当前扇区 |
| ENDURANCE | 跑完当前一轮，保证各扇区擦写次数一致 |

收尾完成后，`stress_app_process()` 调用 `stress_app_on_end()`：先打印结束报告，再启动等待中的模式。返回 `true` 表示已经启动或切换已登记；模式取值非法或配置不合法时返回 `false`。FreeRTOS 中由其他任务调用 `stress_app_request(mode)`，切换在测试任务中执行。

## 读取测试数据

例程通过 `stress_app_get_object()` 取得测试实例，用框架接口读取数据。这些接口只读取实例中的计数，不访问 Flash：

| 接口 | 读数 |
| --- | --- |
| `flash_stress_state(p)` | 当前阶段：`CHECK`、`ERASE`、`BLANK`、`PROG`、`VERIFY`、`READ`、`CLEAN`、`DONE`、`ABORT` |
| `flash_stress_loop(p)` / `flash_stress_ok_cnt(p)` / `flash_stress_err_cnt(p)` | 已完成、通过、失败的轮数 |
| `flash_stress_cycles(p)` | ENDURANCE 每个扇区累计的擦写次数，包括以前运行的次数 |
| `flash_stress_stat(p)` 的 `rd_bytes` / `wr_bytes` / `er_bytes` | 累计读、写、擦字节数，64 位 |
| `flash_stress_stat(p)` 的 `first_fail` | 首次失败的轮次或擦写次数，0 表示没有失败 |
| `flash_stress_stat(p)` 的 `er_time` / `pg_time` | 擦除和页编程耗时：`cnt`、`min`、`max`、`sum`，平均值为 `sum / cnt`；需要接计时 |
| `flash_stress_last_err(p)` | 最近一次错误：类型、地址、期望值、实际值、错误字节数和位数 |

## 显示到屏幕

[show_sample.c](../samples/show_sample.c) 的 `stress_app_show()` 把读数整理成 6 行文字，逐行交给 `board_display_line(row, text)`。模拟板上 READ 运行两轮后的输出如下：

```text
READ  READ
Loop 2  Pass 2  Fail 0
Read 1MB  Write 56KB
First fail 0
Erase avg 46 max 46 ms
Error None
```

| 行 | 内容 |
| --- | --- |
| 0 | 模式和当前阶段 |
| 1 | 已完成、通过、失败的轮数 |
| 2 | 累计读写量，不足 1MB 显示 KB |
| 3 | ENDURANCE 显示擦写次数和首次失败的次数，其他模式显示首次失败的轮次 |
| 4 | 扇区擦除平均和最长耗时，单位毫秒；扇区磨损后会变长 |
| 5 | 最近一次错误：类型、地址、期望值 > 实际值；没有错误时为 `None` |

把 `board_display_line()` 换成 LCD 绘制或 GUI 标签即可，例如 LVGL 中对每行调用一次 `lv_label_set_text()`。周期调用即可，例如每 500 ms 一次；读数不访问 Flash，不影响测试。

## 错误日志与结束报告

- `stress_app_on_error()`：每检测到一次错误调用一次，打印轮次、错误类型、地址、期望值、实际值和错误位数。`err` 只在回调期间有效，需要保存时复制内容。
- `stress_app_on_loop()`：每轮结束调用一次；ENDURANCE 每 1000 次打印一行，避免刷屏。
- `stress_app_report()`：测试结束时由 `stress_app_on_end()` 调用，打印通过、失败轮数和读取量；中止时打印原因和地址。

模拟板上的串口输出：

```text
[FLASH STRESS] loop 3: ok=4 err=0
[FLASH STRESS] ROTATE end: ok=4 err=0 read=136KB
[FLASH STRESS] ENDURANCE end: ok=6 err=0 read=104KB
[FLASH STRESS] cycles=6 first fail=0
[FLASH STRESS] ROTATE abort: AREA USED at 0x5F3000
```

| 错误名称 | 含义 |
| --- | --- |
| `DATA` | 读回的数据和写入的不一致 |
| `ERASE` | 擦除后不是全 `0xFF` |
| `OVERRUN` | READ 模式下驱动写出了缓冲区范围 |
| `READ IO` / `WRITE IO` / `ERASE IO` | 驱动函数返回失败 |
| `AREA USED` | 测试区有别的数据，测试没有执行 |

错误位数少且分散，多为存储单元磨损；整字节或成片出错，多为通信问题。

## 从现象定位问题

| 现象 | 检查顺序 |
| --- | --- |
| `stress_app_start()` 返回 `false` | 模式取值 → 测试区是否按扇区对齐 → 页大小是否整除扇区大小 → 驱动函数是否都已注册 |
| 启动即 `AREA USED` | 测试区是否空闲 → 地址宏是否算错 → 擦除中途断电的残留：确认空闲后在 `stress_app_init()` 中设置 `cfg.force = true` 启动一次 |
| 一直停在 `ERASE` 或 `WRITE` | 忙查询是否一直返回 `true` → 状态寄存器读法 → 写使能；框架没有超时 |
| READ 每轮都是 `DATA` 错误 | 读指令和 dummy 周期 → SPI 时钟 → 连线；读回全 `0xFF` 多为 Flash 没有响应 |
| 偶发 `DATA`，错误位数为 1 | SPI 时钟余量、走线和电源，降低时钟复测 |
| `OVERRUN` 或只在缓冲区不对齐时出错 | 驱动的 DMA 长度和对齐处理 |
| `Erase avg` 为 0 | 没有接计时 |
