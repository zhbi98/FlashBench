# 接口参考

本篇列出框架的全部接口、配置项、状态和错误类型，声明见 [flash_stress.h](../src/flash_stress.h)。接入步骤见[平台移植](porting.md)，读数的用法见[使用说明](usage.md)。

## 底层函数（`flash_stress_ops_t`）

| 回调 | 原型 | 要求 |
| --- | --- | --- |
| `read` | `bool (*)(uint32_t addr, uint8_t * buf, uint32_t len)` | 读出 `len` 字节，成功返回 `true`。ROTATE/ENDURANCE 每次读一个扇区；READ 的长度、地址和 `buf` 对齐都是随机的 |
| `prog` | `bool (*)(uint32_t addr, const uint8_t * buf, uint32_t len)` | 向已擦除区域写 `len` 字节（不超过 `prog_size`），发出指令后即可返回，但返回前必须已经发送或拷贝完 `buf` |
| `erase` | `bool (*)(uint32_t addr)` | 擦除 `addr` 起始的一个扇区，发出指令后即可返回 |
| `busy` | `bool (*)(void)` | 上一次擦除或写入仍在进行时返回 `true`。可选 |
| `on_error` | `void (*)(const flash_stress_err_t * err)` | 每检测到一次错误调用一次。可选 |
| `on_loop` | `void (*)(uint32_t loop, uint32_t ok_cnt, uint32_t err_cnt)` | 每轮结束调用一次。可选 |
| `tick` | `uint32_t (*)(void)` | 自由运行的计数器，单位任意，用于擦除和编程计时。可选 |

两种驱动都能接：

- **非阻塞驱动**：`prog` / `erase` 发完指令就返回，同时提供 `busy`。框架在 `busy` 返回 `true` 期间不会再访问 Flash。
- **阻塞驱动**：`prog` / `erase` 内部等到操作完成才返回，`busy` 填 `NULL`。此时单次 `poll` 的耗时取决于驱动。

非阻塞驱动的耗时量到 `poll` 发现不忙为止，`poll` 调用越密越准。

## 配置项（`flash_stress_cfg_t`）

| 字段 | 含义 | 约束 |
| --- | --- | --- |
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

## 控制接口

| 函数 | 作用 |
| --- | --- |
| `flash_stress_init(p, ops, cfg)` | 初始化实例并登记接口和配置，不访问 Flash |
| `flash_stress_start(p, loops)` | 启动一次全新的测试，计数和流量清零。`loops` 为 0 表示跑到 `stop` 为止 |
| `flash_stress_poll(p)` | 推进一小步。运行中返回 `true`，结束、中止或未启动返回 `false` |
| `flash_stress_waiting(p)` | 是否正在等待擦除或写入完成，此时调用 `poll` 不会前进，可以让出 CPU |
| `flash_stress_stop(p)` | 请求停止：跑完当前这一轮（READ 为当前扇区），ROTATE 还会清理测试区 |

这些接口必须在同一个执行上下文中调用，不能放进中断。

`flash_stress_waiting()` 内部会调用 `busy`，即访问一次 Flash。以下情况返回 `false`：实例为空、测试没有运行、没有正在等待的操作、没有注册 `busy`。

## 读数接口

| 函数 | 读数 |
| --- | --- |
| `flash_stress_running(p)` | 是否仍需要继续调用 `poll` |
| `flash_stress_state(p)` | 当前状态，见[状态](#状态flash_stress_state_t) |
| `flash_stress_loop(p)` | 已完成的轮数 |
| `flash_stress_ok_cnt(p)` | 通过的轮数 |
| `flash_stress_err_cnt(p)` | 失败的轮数 |
| `flash_stress_cycles(p)` | ENDURANCE 每个扇区累计的擦写次数，包括以前运行的次数 |
| `flash_stress_last_err(p)` | 最近一次错误的详情，保留到下一次出错或重新启动 |
| `flash_stress_stat(p)` | 流量、次数和耗时统计，见[统计](#统计flash_stress_stat_t) |

读数接口只读取实例中的数据，不访问 Flash，可以在显示任务中调用；32 位 CPU 上 64 位字节数可能读到更新一半的值，只影响一次显示。

## 状态（`flash_stress_state_t`）

| 状态 | 含义 | 每次 `poll` 的工作量 |
| --- | --- | --- |
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

## 统计（`flash_stress_stat_t`）

| 字段 | 含义 |
| --- | --- |
| `rd_bytes` / `wr_bytes` / `er_bytes` | 累计读、写、擦字节数，64 位，只统计成功的底层调用 |
| `cycle_base` | ENDURANCE 启动时从扇区头读回的已有擦写次数 |
| `first_fail` | 首次失败的轮次或擦写次数，从 1 起，0 表示没有失败 |
| `er_time` / `pg_time` | 擦除和编程耗时：`cnt`、`last`、`min`、`max`、`sum`，平均值为 `sum / cnt`；需要注册 `tick` |

## 错误类型（`flash_stress_res_t`）

| 值 | 含义 |
| --- | --- |
| `FLASH_STRESS_OK` | 操作成功 |
| `FLASH_STRESS_ERR_PARAM` | 参数非法，或实例当前不能接受这次调用 |
| `FLASH_STRESS_ERR_NOT_BLANK` | 测试区里有非测试数据，测试中止 |
| `FLASH_STRESS_ERR_ERASE` / `ERR_PROG` / `ERR_READ` | 对应的底层函数返回失败 |
| `FLASH_STRESS_ERR_ERASE_VERIFY` | 擦除后读回不是全 `0xFF` |
| `FLASH_STRESS_ERR_DATA_VERIFY` | 写入后读回与写入的数据不一致；READ 模式随机读和已知数据不一致也报这一项 |
| `FLASH_STRESS_ERR_READ_OVERRUN` | READ 模式：读函数写出了请求的缓冲区范围 |

## 错误详情（`flash_stress_err_t`）

| 字段 | 含义 |
| --- | --- |
| `res` | 错误类型 |
| `loop` | 出错的轮次，从 0 起 |
| `addr` | Flash 地址：第一个出错字节，或越界那次读的起始地址 |
| `expect` / `got` | 期望值和读回的值，只对比较类错误有效 |
| `bad_cnt` | 比较范围内的错误字节数，或被改写的保护字节数 |
| `bad_bits` | 比较范围内的错误位数，只对比较类错误有效 |

位数少而分散多是存储单元磨损，整字节或成片出错多是传输问题。传给 `on_error` 的 `err` 只在回调期间有效，需要保存时复制内容。
