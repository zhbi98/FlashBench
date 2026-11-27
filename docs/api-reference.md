# 接口参考

[README](../README.md) · [工作原理](architecture.md) · [平台移植](porting.md) · [使用说明](usage.md) · [问题定位](troubleshooting.md) · **接口参考** · [验证与测试](testing.md)

本篇列出框架的全部接口、配置项、状态和错误类型，声明见 [flash_stress.h](../src/flash_stress.h)。接入步骤见[平台移植](porting.md)，读数的用法见[使用说明](usage.md)。

## 底层函数

由应用实现，通过 `flash_stress_ops_t` 注册：

```c
bool read(uint32_t addr, uint8_t * buf, uint32_t len);
bool prog(uint32_t addr, const uint8_t * buf, uint32_t len);
bool erase(uint32_t addr);
bool busy(void);
void on_error(const flash_stress_err_t * err);
void on_loop(uint32_t loop, uint32_t ok_cnt, uint32_t err_cnt);
uint32_t tick(void);
```

| 回调 | 必需 | 作用 |
| --- | --- | --- |
| `read` | 是 | 读出 `len` 字节，成功返回 `true` |
| `prog` | 是 | 向已擦除区域写 `len` 字节 |
| `erase` | 是 | 擦除 `addr` 起始的一个扇区 |
| `busy` | 否 | 擦除或写入仍在进行时返回 `true` |
| `on_error` | 否 | 每检测到一次错误调用一次 |
| `on_loop` | 否 | 每轮结束调用一次 |
| `tick` | 否 | 自由运行的计数器，用于擦写计时 |

`read` 在 ROTATE 和 ENDURANCE 中每次读一个扇区，在 READ 中长度、地址和 `buf` 对齐都是随机的。`prog` 的 `len` 不超过 `prog_size`，发出指令后即可返回，但返回前必须发送或拷贝完 `buf`。`erase` 发出指令后即可返回。`tick` 的单位任意。

两种驱动都能接入：

- **非阻塞驱动**：`prog` 和 `erase` 发完指令就返回，必须提供 `busy`。
- **阻塞驱动**：`prog` 和 `erase` 等到操作完成才返回，`busy` 填 `NULL`。

非阻塞驱动在 `busy` 返回 `true` 期间，框架不会再访问 Flash。阻塞驱动下，单次 `poll` 的耗时取决于驱动。

## 配置项

`flash_stress_cfg_t` 的字段：

| 字段 | 含义 | 约束 |
| --- | --- | --- |
| `base` | 测试区起始地址 | `sec_size` 的整数倍 |
| `sec_size` | 扇区大小，单位字节 | 大于 16 |
| `sec_cnt` | 测试区扇区数 | 大于 0 |
| `wr_secs` | ROTATE 每轮写入的扇区数 | 1 到 `sec_cnt`，0 按 1 处理 |
| `prog_size` | 每次 `prog` 调用的字节数 | 整除 `sec_size`，0 按 `sec_size` 处理 |
| `reads` | 读回比较的遍数 | 0 按 1 处理 |
| `wbuf`、`rbuf` | 写、读缓冲区 | 各 `sec_size` 字节，不能为 `NULL` |
| `mode` | 测试模式 | 默认（0）是 ROTATE |
| `max_errs` | 失败多少轮后自动结束 | 0 表示出错不结束 |
| `force` | 跳过测试区内容检查 | 只在确认测试区空闲时用 |

测试区不能超出 32 位地址空间。`mode` 取 `FLASH_STRESS_MODE_` 加 `ROTATE`、`READ` 或 `ENDURANCE`。`wr_secs` 只在 ROTATE 中有效，其他模式忽略。

READ 模式下，`reads` 是每个扇区每轮的随机读次数，`prog_size` 还用作页大小来选读长度，`wbuf` 用来存期望数据。

`force` 为 `true` 时任何内容都可能被擦除，典型用途是擦除中途断电后恢复。配置不合法时 `flash_stress_init()` 返回 `FLASH_STRESS_ERR_PARAM`。

## 控制接口

| 函数 | 作用 | 说明 |
| --- | --- | --- |
| `flash_stress_init(p, ops, cfg)` | 初始化实例 | 只登记，不访问 Flash |
| `flash_stress_start(p, loops)` | 启动一次全新的测试 | `loops` 为 0 表示不限轮数 |
| `flash_stress_stop(p)` | 请求停止 | 当前一轮结束后生效 |
| `flash_stress_poll(p)` | 推进一小步 | 测试结束后返回 `false` |
| `flash_stress_waiting(p)` | 是否在等待擦写完成 | 会调用 `busy` |

这些接口必须在同一个执行上下文中调用，不能放进中断。

`start` 把计数和流量清零。`poll` 在运行中返回 `true`，在结束、中止或未启动时返回 `false`。`stop` 之后，ROTATE 和 ENDURANCE 跑完当前一轮，READ 读完当前扇区，ROTATE 还会清理测试区。

`waiting` 返回 `true` 时调用 `poll` 不会前进，可以让出 CPU。它内部会调用 `busy`，即访问一次 Flash。实例为空、测试没有运行、没有正在等待的操作、没有注册 `busy` 时，它返回 `false`。

## 读数接口

| 函数 | 读数 |
| --- | --- |
| `flash_stress_running(p)` | 是否仍需要继续调用 `poll` |
| `flash_stress_state(p)` | 当前状态 |
| `flash_stress_loop(p)` | 已完成的轮数 |
| `flash_stress_ok_cnt(p)` | 通过的轮数 |
| `flash_stress_err_cnt(p)` | 失败的轮数 |
| `flash_stress_last_err(p)` | 最近一次错误的详情 |
| `flash_stress_stat(p)` | 流量、次数和耗时统计 |
| `flash_stress_cycles(p)` | ENDURANCE 每个扇区累计的擦写次数 |

读数接口只读取实例中的数据，不访问 Flash，可以在显示任务中调用。32 位 CPU 上 64 位字节数可能读到更新一半的值，只影响一次显示。

`cycles` 包括以前运行累计的次数。`last_err` 保留到下一次出错或重新启动。

## 状态

`flash_stress_state_t` 的取值，前缀为 `FLASH_STRESS_ST_`：

| 状态 | 含义 | 每次 `poll` 的工作量 |
| --- | --- | --- |
| `IDLE` | 已初始化，未启动 | 无 |
| `CHECK` | 检查测试区是否可用 | 读一个扇区 |
| `ERASE` | 擦除本轮的扇区 | 发起一次扇区擦除 |
| `BLANK` | 检查擦除后是否全空白 | 读一个扇区并比较 |
| `PROG` | 写入测试数据 | 发起一次写入 |
| `VERIFY` | 读回比较 | 读一个扇区并比较 |
| `READ` | READ 模式的随机读 | 读一次并比较 |
| `CLEAN` | ROTATE 结束前擦回空白 | 发起一次扇区擦除 |
| `DONE` | 测试结束 | 无 |
| `ABORT` | 测试区不可用 | 无 |

等待擦除或写入完成期间，`poll` 只调用一次 `busy` 就返回。到达 `DONE` 时，ROTATE 的测试区已恢复空白，READ 和 ENDURANCE 保留数据。到达 `ABORT` 时没有擦除任何内容。

## 统计

`flash_stress_stat_t` 的字段：

| 字段 | 含义 |
| --- | --- |
| `rd_bytes` | 累计读取的字节数，64 位 |
| `wr_bytes` | 累计写入的字节数，64 位 |
| `er_bytes` | 累计擦除的字节数，64 位 |
| `cycle_base` | ENDURANCE 启动时读回的已有擦写次数 |
| `first_fail` | 首次失败的轮次或擦写次数，0 表示没有 |
| `er_time` | 扇区擦除耗时 |
| `pg_time` | 编程耗时 |

字节数只统计成功的底层调用。`first_fail` 从 1 起计。两项耗时用 `flash_stress_time_t` 表示，需要注册 `tick`，单位与 `tick` 相同。

它有 `cnt`（操作次数）、`last`（最近一次）、`min`（最短）、`max`（最长）和 `sum`（总和）五个字段，平均值为 `sum / cnt`。

非阻塞驱动的耗时量到 `poll` 发现不忙为止，`poll` 调用越密越准。

## 错误类型

`flash_stress_res_t` 的取值，前缀为 `FLASH_STRESS_`：

| 值 | 含义 |
| --- | --- |
| `OK` | 操作成功 |
| `ERR_PARAM` | 参数非法，或实例当前不能接受这次调用 |
| `ERR_NOT_BLANK` | 测试区里有非测试数据，测试中止 |
| `ERR_ERASE` | 擦除函数返回失败 |
| `ERR_PROG` | 写入函数返回失败 |
| `ERR_READ` | 读函数返回失败 |
| `ERR_ERASE_VERIFY` | 擦除后读回不是全 `0xFF` |
| `ERR_DATA_VERIFY` | 读回的数据与期望的不一致 |
| `ERR_READ_OVERRUN` | 读函数写出了请求的缓冲区范围 |

READ 模式的随机读和已知数据不一致时，也报 `ERR_DATA_VERIFY`。`ERR_READ_OVERRUN` 只在 READ 模式出现。

## 错误详情

`flash_stress_err_t` 的字段：

| 字段 | 含义 |
| --- | --- |
| `res` | 错误类型 |
| `loop` | 出错的轮次，从 0 起 |
| `addr` | 出错的 Flash 地址 |
| `expect` | 期望值 |
| `got` | 读回的值 |
| `bad_cnt` | 错误字节数 |
| `bad_bits` | 错误位数 |

`addr` 是第一个出错字节的地址。`expect`、`got` 和 `bad_bits` 只对比较类错误有效。越界错误时，`addr` 是那次读的起始地址，`bad_cnt` 是被改写的保护字节数。

传给 `on_error` 的 `err` 只在回调期间有效，需要保存时复制内容。错误位数少而分散，多是存储单元磨损；整字节或成片出错，多是传输问题。
