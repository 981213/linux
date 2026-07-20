# TMU：流量管理单元

## 功能与关系

TMU 接收前级 met frame（MF），决定丢包、复制到多个输出端口、排队、调度和整形。报文数据仍在 DATA_BUF 中；TMU 保存 MF、packet info 和 buffer pointer。某个输出副本得到调度后，后级 ARP_INTF、EVLAN、EACL 和 MODIFY 读取并发送；丢包或最后一个副本发送完成后，buffer pointer 交 BMU 回收。

每个 TMU port 有 8 个 queue、2 个 scheduler 和 6 个 shaper。当前驱动按第一版 RTL 使用 10 个 port；寄存器地址空间定义了 36 个 port。硬件的两级调度拓扑为：

```text
Q0..Q3 ─► scheduler 0 ─┐
                       ├─► scheduler 1 ─► 最终 port output
Q4..Q7 ────────────────┘

6 个 shaper 可通过 position 接在 queue、scheduler 分支或最终输出上。
```

本文寄存器字段来自用户提供的 SF21 寄存器表截图；重复项与当前 `sf_dpns_tmu.h`、`sf_dpns_tmu.c` 交叉核对。截图与当前头文件不一致的地方单独列出，不能静默选择其中一种。

## 地址约定

下文偏移均相对 DPNS 基址 `B = 0x11000000`。例如 `TMU_CTRL` 的偏移是 `0x148004`，物理地址为 `0x11148004`。

```text
PORT(p) = p * 0x2000
QUEUE(p, q) = PORT(p) + 0x100000 + q * 0x20      q = 0..7
SCHED(p, s) = PORT(p) + 0x101000 + s * 0x40     s = 0..1
SHAPER(p, h) = PORT(p) + 0x101080 + h * 0x20    h = 0..5
TDQ(p) = PORT(p) + 0x101140
MD2TM(p, q) = PORT(p) + 0x101148 + q * 4
```

## 全局控制与 LLM FIFO

| 偏移 | 寄存器 | 位 | 访问 | 复位值 | 功能 |
|---:|---|---:|---|---:|---|
| `0x148000` | `TMU_VERSION` | 15:0 | RO | `0x0001` | TMU ID |
|  |  | 23:16 | RO | `0x01` | version |
|  |  | 31:24 | RO | `0x00` | revision |
| `0x148004` | `TMU_CTRL` | 1 | RW | 1 | `MF_IN_CNT_EN`；MF buffer 计数是否计入 MF 本身 |
|  |  | 2 | RW | 1 | `MD_RDY_EN`；是否把 MODIFY ready 纳入可发送条件 |
|  |  | 7:3 | RW | 0 | `DEBUG_SEL`；选择 26 路 LA debug signal，有效值 0～25 |
| `0x148008` | `LLM_FIFO_CTRL0` | 11:0 | RW | `0x7ff` | 2K-depth LLM FIFO programmable-full assert 水位 |
|  |  | 27:16 | RW | `0x7fe` | programmable-full negate 水位 |
| `0x14800c` | `LLM_FIFO_CTRL1` | 11:0 | RW | `0x024` | programmable-empty assert 水位 |
|  |  | 27:16 | RW | `0x028` | programmable-empty negate 水位 |

当前驱动写入：

```text
TMU_CTRL          = 0x00000006
LLM_FIFO_CTRL0    = 0x07fe07ff
LLM_FIFO_CTRL1    = 0x00280024
```

这与截图中的复位值一致。`MD_RDY_EN=1` 很关键：MODIFY backpressure 会阻止 TMU 继续放行。如果压力下 TMU 队列不为空但端口不再出包，应同时检查 MODIFY flow-backpressure latch。

注意：截图把 `DEBUG_SEL` 定义为 bits 7:3（5 bit），当前头文件却使用 `GENMASK(8,3)`（6 bit）。在确认 RTL 版本前，debug selector 应限制为 0～25，不要写 bit 8。

## Reclaim 与 flow-control 诊断

### Read-clear 控制

`TMU_RECLAIM_FULL_RD_CLR = 0x148010` 的低 4 bit 分别控制以下寄存器的 read-clear 行为，截图复位值均为 1：

| 位 | 字段 | 控制对象 |
|---:|---|---|
| 0 | `full_max_cnt_clr_en` | `TMU_RECLAIM_FULL_MAX_CNT` |
| 1 | `full_posedge_cnt_clr_en` | `TMU_RECLAIM_FULL_POSEDGE_CNT` |
| 2 | `tmu_reclaim_full_clr_en` | `TMU_FLOW_CONTROL_LATCH` |
| 3 | `llm_fifo_ctrl_clr_en` | `TMU_LLM_FIFO_LATCH` |

bit 为 1 时，读取对应 RC 寄存器会清零；为 0 时读取不清。需要无损快照时应先清对应 enable bit，再读取；要清除旧 latch，可重新置位后读一次。不要用循环监控程序直接读取默认 RC counter，否则采样程序本身会改变状态。

这里的全局 `TMU_RECLAIM_FULL_RD_CLR@0x148010` 不要和当前头文件中的 `TMU_RD_CLR_EN@0x2800c0` 混淆：后者位于 BMU/MODIFY 邻近区域，已知 bit 12 控制 TMU enqueue release，是另一组控制寄存器。

### Counter 与 latch

| 偏移 | 寄存器 | 位 | 访问 | 含义 |
|---:|---|---:|---|---|
| `0x148014` | `TMU_RECLAIM_FULL_MAX_CNT` | 31:0 | RC | reclaim-full 连续拉高的最大周期数；更大的新值才覆盖旧最大值 |
| `0x148018` | `TMU_RECLAIM_FULL_POSEDGE_CNT` | 31:0 | RC | reclaim-full 从低到高的次数 |
| `0x14801c` | `TMU_FLOW_CONTROL_LATCH` | 0 | RC | `tmu_reclaim_full` 曾经拉高 |
|  |  | 1 | RC | `arp_flow_bp` 曾经拉高 |
|  |  | 2 | RC | buffer-control write backpressure 曾经拉高 |
|  |  | 3 | RC | buffer-control read backpressure 曾经拉高 |
|  |  | 30:4 | RC | 27 路 MODIFY flow-backpressure latch |
| `0x148020` | `TMU_LLM_FIFO_LATCH` | 0 | RC | programmable-full 曾经拉高 |
|  |  | 1 | RC | full 曾经拉高 |
|  |  | 2 | RC | programmable-empty 曾经拉高 |
|  |  | 3 | RC | empty 曾经拉高 |

截图给出的 reset 值中，`arp_flow_bp`、`llm_fifo_prog_empty_latch` 和 `llm_fifo_empty_latch` 为 1，其余为 0。因此冷启动后的第一次读数不一定代表 Linux 启动后出现过异常；应先清一次，再开始测试窗口。

诊断解释：

- `reclaim_full_posedge` 快速增长：回收通路频繁抖动。
- `reclaim_full_max_cnt` 很大：出现过持续阻塞，比短促脉冲更危险。
- `modify_flow_bp` 某一位置位：对应逻辑输出端口的后级来不及消费。
- LLM full latch 置位：queue linked-list/pointer 资源耗尽，不等同于某个 queue 的 packet threshold 命中。

## vport 到 iport 映射

TMU 有 27 个逻辑入口 vport，每个映射到一个 5-bit iport。映射值 0～26 有效，27～31 无效。

| 偏移 | 映射内容 |
|---:|---|
| `0x148030` | vport 0～5，每项依次占 bits 4:0、9:5、14:10、19:15、24:20、29:25 |
| `0x148034` | vport 6～11，打包格式相同 |
| `0x148038` | vport 12～17，打包格式相同 |
| `0x14803c` | vport 18～23，打包格式相同 |
| `0x148040` | vport 24 bits 4:0、25 bits 9:5、26 bits 14:10；bit 15 为 `ivport_map_enable` |

`ivport_map_enable=1` 时，TMU 用映射后的 iport 释放入口报文缓存；为 0 时使用报文携带的原始 iport。复位时 mapping 和 enable 都为 0，因此当前精简驱动不编程这组寄存器也能依靠报文 metadata 工作。接入 external vport 7～26 时，必须把 mapping 五个 word 全部准备好，最后再置 enable，避免把正在转发的包释放到错误端口账户。

## BMU 丢包准入策略

当共享 buffer 紧张时，BMU 给 TMU 一个 2-bit `drop_policy_indication`。`TMU_BMU_PRIORITY = 0x148044` 决定哪些 priority/port 仍可入队：

| 位 | 字段 | 访问 | 复位值 | 含义 |
|---:|---|---|---:|---|
| 25:24 | `drop_policy_indication` | RO | 0 | BMU 当前压力等级：00 全通过，11 全丢弃，01/10 进入选择性准入 |
| 17:16 | `drop_scheme_sel` | RW | 0 | 00/11 按 priority；01 按 port；10 按 port 与 priority 组合 |
| 15:8 | `priority_bitmap_level2` | RW | `0xff` | indication=10 时允许通过的 priority 0～7 |
| 7:0 | `priority_bitmap_level1` | RW | `0xff` | indication=01 时允许通过的 priority 0～7 |

对应 port bitmap：

| 偏移 | 位 | 复位值 | 用途 |
|---:|---:|---:|---|
| `0x148048` | 26:0 | `0x7ffffff` | `port_bitmap_level1`，indication=01 |
| `0x14804c` | 26:0 | `0x7ffffff` | `port_bitmap_level2`，indication=10 |

bitmap 中 1 表示允许对应 priority/port 继续通过。复位值全 1，因此 buffer 有余量或进入选择性压力等级时不会无意偏置某个端口。这里的 BMU admission 是共享缓冲保护，不是普通用户 QoS；配置错误可能在高负载下只丢特定 port/priority，低负载测试却完全正常。

## 每端口 Queue

第 `q` 个 queue 的基址为 `QUEUE(p,q)`，下面是相对该 queue 的偏移：

| 子偏移 | 寄存器 | 位 | 访问 | 截图复位值 | 功能 |
|---:|---|---:|---|---:|---|
| `0x00` | `QUEUE_CFG0` | 1:0 | RW | 0 | drop type：0 mixed tail drop，1 tail drop，2 WRED，3 buffer-cell tail drop |
|  |  | 18:8 | RW | `0x0f` | packet/queue max threshold |
|  |  | 30:20 | RW | 0 | WRED min threshold |
| `0x04` | `QUEUE_CFG1` | 4:0...29:25 | RW | 0 | WRED stage 0～5 probability，各 5 bit |
| `0x08` | `QUEUE_CFG2` | 4:0、9:5 | RW | 0 | WRED stage 6、7 probability |
| `0x0c` | `QUEUE_STS0` | 10:0、26:16 | R | 0 | head pointer、tail pointer |
| `0x10` | `QUEUE_STS1` | 11:0 | R | 0 | 当前 packet count |
| `0x14` | `QUEUE_STS2` | 11:0 | R | 0 | 当前占用的 96-byte buffer-cell 数 |
| `0x18` | `QUEUE_CFG3` | 11:0 | RW | `0x01f` | 该 queue 可使用的最大 buffer-cell 数 |

截图中 `QUEUE_CFG2` 的英文说明误写成 scheduler queue weight；从字段名、位置和当前头文件可以确认它们是 WRED stage 6/7 probability。

当前驱动没有使用截图复位阈值，而是对 10 个 TMU port 的 8 个 queue 全部写：

```text
QUEUE_CFG0 = 0x00011f00    # drop type 0，QUEUE_MAX=0x11f
QUEUE_CFG1 = 0
QUEUE_CFG2 = 0
QUEUE_CFG3 = 0x000005ee    # 最多 1518 个 buffer cell
```

驱动还向截图标为只读的 `STS0/1/2` 写 0。当前硬件接受这套初始化且已通过转发测试，但这与寄存器表的 R 属性冲突；在其他 RTL revision 上不要照搬“写状态寄存器清零”的假设。

`QUEUE_MAX` 是 packet/FIFO 项阈值，`QUEUE_BUF_MAX` 是 buffer-cell 阈值。大包可能先触发 cell 限制，小包可能先触发 packet 限制，排查丢包时必须同时读取 `STS1` 和 `STS2`。

## Scheduler

每个 port 有 scheduler 0 和 1，stride 为 `0x40`：

| 子偏移 | 寄存器 | 位 | 功能 |
|---:|---|---:|---|
| `0x00` | `SCH_CTRL` | 3:0 | 算法：0 PQ、1 WFQ、2 DWRR、3 RR、4 WRR |
| `0x10 + 4*q` | `SCH_Q_WEIGHT(q)` | 31:0 | scheduler 输入 q 的 weight |
| `0x30` | `SCH_Q_ALLOC0` | 3:0、11:8、19:16、27:24 | queue/input 0～3 接到 scheduler 的哪个 input |
| `0x34` | `SCH_Q_ALLOC1` | 同上 | queue/input 4～7 的连接；值 8 表示不连接 |
| `0x38` | `SCH_BIT_RATE` | 0 | 0 按 packet length 调度，1 按 packet count 调度 |
| `0x3c` | `SCH0_POS` | 3:0 | 仅 scheduler 0：其输出连接到 scheduler 1 的哪个 input |

截图把算法字段定义为 bits 3:0、bit-rate 只使用 bit 0；当前头文件分别写成 `GENMASK(6,0)` 和 32-bit mask。当前初始化只写 0，行为没有歧义；将来开放高级调度时应按截图限制保留位，并先读回 version/revision。

当前驱动建立的两级连接是：

```text
scheduler 0:
  ALLOC0 = 0x03020100   # Q0..Q3 -> input 0..3
  ALLOC1 = 0x08080808   # Q4..Q7 不连接
  POS    = 0            # scheduler 0 output -> scheduler 1 input 0

scheduler 1:
  ALLOC0 = 0x06050400   # sch0 output、Q4、Q5、Q6 -> input 0、4、5、6
  ALLOC1 = 0x08080807   # Q7 -> input 7，其余不连接
```

两个 scheduler 当前都选择 PQ，weight 全 0，按 packet length 调度。PQ 中编号较大的 input 优先级更高；例如 scheduler 1 同时积压 input 0 和 input 7 时，input 7 会一直优先出队。

切换到 WFQ、DWRR 或 WRR 前，必须先给该 scheduler 的每个已连接 input 写入非零 weight，再切换 `SCH_CTRL`。已连接 input 保持 weight 0 时，DWRR 可能停止整个 scheduler 的出队。当前资料仍不足以确定各 weighted 算法的 weight 单位及比例换算，不能仅把 Linux ETS quantum 原样写入该寄存器。

物理入口收到的 802.1Q PCP 直接选择同编号的出口 TMU queue：PCP 0～7 分别进入 Q0～Q7。未携带 VLAN tag 的帧进入 Q0；仅设置 IPv4 DSCP 不会改变 queue。由于 PQ 的高编号 input 优先，所以 PCP 7 是最高优先级，PCP 0 是最低优先级。若要从 DSCP 获得硬件队列选择，必须在 parser/IACL/MODIFY 路径中另行配置 DSCP 到内部优先级或 PCP 的映射。

## Shaper

每个 port 有 6 个 shaper，stride 为 `0x20`：

| 子偏移 | 寄存器 | 位 | 访问 | 截图复位值 | 功能 |
|---:|---|---:|---|---:|---|
| `0x00` | `SHP_CTRL` | 0 | RW | 0 | enable |
|  |  | 31:1 | RW | 0 | clock divider，控制 credit 添加周期 |
| `0x04` | `SHP_WEIGHT` | 11:0 | RW | 0 | 8.12 格式的小数部分 |
|  |  | 19:12 | RW | 0 | 8.12 格式的整数部分 |
| `0x08` | `SHP_MAX_CREDIT` | 21:0 | RW | `0x3fffff` | credit 上限 |
| `0x0c` | `SHP_CTRL2` | 0 | RW | 0 | 0 按 packet length，1 按 packet count |
|  |  | 5:1 | RW | shaper position |
|  |  | 6 | RW | 0 空闲时保留 credit，1 当前 queue 无包时清 credit |
| `0x10` | `SHP_MIN_CREDIT` | 21:0 | RW | `0x3fffff` | credit 下限 |
| `0x14` | `SHP_STATUS` | 0 | R | 1 | shaper 是否可工作；未 enable 或 credit 为正时为 1 |
|  |  | 23:1 | R | 0 | 当前 credit counter |

当前驱动初始化每个 shaper 为 disabled，weight 0，raw `MIN_CREDIT=0x3ff00`、`MAX_CREDIT=0x400`，position 初始等于 shaper index。这里存在一个需要保留在文档中的版本/位域差异：截图明确把 `MAX_CREDIT` 定义为 bits 21:0，当前头文件却定义为 `GENMASK(21,10)`，root TBF 也通过这个 mask 把 `max_size` 左移 10 bit。现有 20/100 Mbps 测试通过并不能证明 credit ceiling 的所有边界值都正确；修改 burst/max-size 语义前应做 raw register 读回、长包和突发流量测试。

position 的实际连接点如下。一个连接点只能由一个 shaper 占用；多个 shaper 使用相同 position 时不能形成串联关系，低编号 shaper 可能遮蔽高编号 shaper。

| Position | 连接点 |
|---:|---|
| 0 | scheduler 1 输出，即最终 port output |
| 1 | scheduler 0 输出 |
| 2 | queue 4 |
| 3 | queue 5 |
| 4 | queue 6 |
| 5 | queue 7 |
| 6 | queue 3 |
| 7 | queue 2 |
| 8 | queue 1 |
| 9 | queue 0 |

与 MD2TM 默认 bitmap 相配套的原生布局是 shaper 0 位于 position 0、shaper 1 位于 position 1、shaper 2～5 分别位于 position 2～5。端口总出口整形应启用 shaper 0，不能把 shaper 5 移到 position 0；否则 shaper 0 和 shaper 5 会争用同一连接点。

当前 rate 换算代码使用真实 NPU clock：

```text
weight_8_12 = round(rate_bytes_per_sec * 2^(clk_div + 13) / npu_clock_hz)
```

它从 divider 15 向下选择能装入 20-bit weight 的最大 divider，以提高精度。不要使用头文件里关于 MPW 400 MHz/fullmask 600 MHz 的旧注释做固定换算。

## TDQ 与 MODIFY 长度反馈

`TDQ(p)` 的已知寄存器：

| 子偏移 | 寄存器 | 位 | 功能 |
|---:|---|---:|---|
| `0x00` | `TDQ_IFG` | 7:0 | packet-length mode 下额外计入的线缆开销 |
| `0x04` | `TDQ_CTRL` | 0 | shaper clock counter enable |
|  |  | 1 | TDQ hardware enable |
|  |  | 2 | scheduler 0 enable |
|  |  | 3 | scheduler 1 enable |
|  |  | 4 | RO，当前是否允许配置 |
|  |  | 5 | 忽略残留 packet |

当前驱动把 `TDQ_IFG` 写成 `0x18`，即把 preamble、FCS 和 inter-packet gap 合计 24 Byte 纳入调度/整形，并在允许配置时向 `TDQ_CTRL` 写 `0x2f`。

MODIFY 会把其计算的 header length 返回 TMU，以修正 shaper 的 packet-length credit。每个 queue 有一项 `MD2TM_Qn_SHPVLD`：

| 偏移公式 | 位 | 功能 |
|---:|---:|---|
| `PORT(p)+0x101148+4*q` | 5:0 | shaper 0～5 bitmap；bit 为 1 表示该 queue 的 MODIFY header length 应计入对应 shaper |
| `PORT(p)+0x101148`（Q0） | 8 | `PKT_LEN_BYPASS`；1 表示 shaper credit 不包含 MODIFY 返回的这部分长度 |

截图给出的默认 bitmap：

| Queue | 默认值 | 经过的 shaper |
|---:|---:|---|
| 0～3 | `0x03` | 0、1 |
| 4 | `0x05` | 0、2 |
| 5 | `0x09` | 0、3 |
| 6 | `0x11` | 0、4 |
| 7 | `0x21` | 0、5 |

这组 bitmap 只控制 MODIFY 返回的 header length 是否参与相应 shaper 的 packet-length credit 计算，并不控制 queue 是否经过该 shaper。默认 bitmap 与上述原生 shaper position 一致：所有 queue 都把修正长度反馈给根 shaper 0；Q0～Q3 还反馈给 scheduler-0 分支的 shaper 1；Q4～Q7 分别反馈给各自的 shaper 2～5。以后改变 shaper position 时必须同步审计 `MD2TM_Qn_SHPVLD`，否则 VLAN、PPPoE 或 NAT header rewrite 会造成按线速计费的系统性偏差。

## Linux root TBF 配置

当前主线化实现只暴露物理端口 root TBF，使用原生根 shaper 0、position 0：

1. 先 disable shaper 0，避免半更新参数立即生效。
2. 写 weight、max credit、min credit 和 position 0。
3. 最后写 `SHP_CTRL`，作为新 rate 的 commit 点。
4. 删除 qdisc 时先 disable，再把 weight 清零。

```sh
tc qdisc replace dev eth0 root tbf rate 100mbit burst 2k latency 50ms
tc qdisc del dev eth0 root
```

当前实现接受的 `max_size` 上限为 4095 Byte；更大的 burst 会使硬件 offload 被拒绝并由 qdisc 回退到软件路径。当前不支持 ATM linklayer、额外 overhead/mpu，也没有暴露 queue、WFQ/DWRR/WRR/WRED。

## Linux strict-priority 配置

物理端口可以通过 Linux `prio` qdisc 启用两级 PQ。Linux band 0 是最高优先级，而 TMU Q7 是最高优先级，因此只支持 8 个 band 以及如下反向 priomap：

```sh
tc qdisc replace dev eth0 root handle 1: prio bands 8 \
  priomap 7 6 5 4 3 2 1 0 7 6 5 4 3 2 1 0
tc qdisc del dev eth0 root
```

该配置使 Linux priority 0～7 与硬件 PCP/Q0～Q7 保持同一优先级含义，同时把 qdisc band 0～7 对应到 Q7～Q0。其他 band 数或 priomap 不能由现有硬件分类路径准确表达，驱动会拒绝 offload。给 band graft 子 qdisc 也会撤销父 prio 的 offload，因为目前尚未提供 per-queue qdisc offload。

Linux ETS 的 strict-only 子集也由同一 PQ 表达，配置同样必须有 8 个 strict band 和上述反向 priomap：

```sh
tc qdisc replace dev eth0 root handle 1: ets bands 8 strict 8 \
  priomap 7 6 5 4 3 2 1 0 7 6 5 4 3 2 1 0
```

Weighted ETS 需要可信的 quantum-to-weight 换算。当前寄存器资料没有定义 WFQ/DWRR/WRR weight 单位，各 weighted 模式也尚未表现出可重复的比例关系，因此任何带非零 quantum 的 ETS 配置都不会 offload。

## 推荐调试顺序

1. 读取 `TMU_VERSION`，记录 ID/version/revision。
2. 清一次 reclaim/flow/LLM RC latch，建立干净测试窗口。
3. 同时采集 queue `PKT_CNT`、`BUF_CNT`、head/tail pointer。
4. 查看 reclaim-full 最大持续时间与上升沿次数。
5. 查看 27-bit MODIFY backpressure latch，确认阻塞属于哪个逻辑输出端口。
6. 检查 BMU pressure indication、scheme 及两级 priority/port bitmap。
7. 最后检查 scheduler allocation、shaper position、MD2TM bitmap 和 credit counter。

Linux fq_codel 只能管理 CPU→EDMA 的 skb；命中硬件 L2/NAT 的流量没有 skb，不经过 qdisc。所有 offloaded 流的最终物理出口拥塞仍必须由 TMU queue、scheduler、WRED 和 shaper 管理。
