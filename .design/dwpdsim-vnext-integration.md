# MQSim 面向 DWPDSim vNext 的改造设计

状态：已实现，并通过 DWPDSim vNext 单 SSD、单 scenario 联调。

本文定义 MQSim 为 DWPDSim vNext 提供物理 SSD 仿真的改造边界。实现基线是当前仓库
`c4c214f`，它已经具备基础 READ/WRITE/TRIM trace、NVMe DSM 和 page-level FTL TRIM。
`KVCache_Radix_MQSim_DWPD_online_v7/MQSim-master` 只作为正确性修复和实验方法的参考，
不整体替换当前 MQSim，也不保留其中的 adapter、Radix 或 policy 逻辑。

本设计不承诺兼容 v7 的 binary、XML 字段或生成目录。普通五列 trace 作为 MQSim 独立的通用
能力保留，不是 DWPDSim vNext 的 fallback 或兼容层。

配套的上层接口定义见
[DWPDSim vNext Policy 重构设计](../../DWPDSim/.design/vnext-policy-refactor.md)。

## 1. 目标和非目标

### 1.1 目标

- 在一次 MQSim 运行中回放 DWPDSim 产生的全部 SLC/TLC I/O；
- SLC/TLC 共享 NVMe host、controller、CMT、TSU 和全局时间轴，但使用显式且互不重叠的
  channel pool；
- 同一 pool 内的多个 stream 共享物理容量和 GC，不按 stream 静态切分 plane；
- MQSim 只接收 READ、WRITE、TRIM，正确模拟 relocation 的读、写和回收开销；
- 支持跨 flow 的 completion dependency，使 relocation 可以准确执行
  `READ completion -> WRITE completion -> TRIM`；
- 修复长时间 TRIM、LBA reuse、GC 并发下的映射和 block bookkeeping 正确性；
- 输出稳定的 host、FTL、pool、channel 和测量窗口统计，供 DWPDSim 计算 latency、写放大、
  DWPD 和寿命指标；
- 所有 DWPDSim policy 使用同一个 MQSim binary、SSD 配置和 GC 配置，保证物理层对比公平。

### 1.2 非目标

- MQSim 不理解 request、Radix、segment、Dump、placement、idle eviction 或 Algorithm2；
- 不在 MQSim 中实现 `MIGRATE` opcode；
- 不让物理 GC 回调或修改 DWPDSim 的 RadixTree/StorageState；
- 不复用 v7 的固定 1 ns/50 ms 间隔模拟 dependency；
- 不用 sentinel TRIM 延长 flow 或推动仿真结束；
- 不通过 `Queue_Fetch_Size=1` 偶然维持 relocation 顺序；
- 第一版不支持不同 pool 使用不同 page size、pages per block、dies 或 planes 等异构几何。

### 1.3 重构覆盖

重构前的 type-2 TRIM 基线存在以下缺口，本版本已逐项覆盖：

- 五列 trace 没有全局 request id 和跨 flow completion dependency；
- 相同 channel 被多个 stream 使用时，logical/physical capacity 会按 stream 数静态除分；
- `Flash_Parameter_Set` 是单一全局静态配置，一个 SSD 不能同时表达 SLC/TLC timing 和 PE limit；
- current TRIM 尚未完整覆盖 stale physical version、LBA reuse 和 GC 已读未写期间的竞争；
- GC 可能过早安装 physical-block barrier，且 active/default block 的候选边界不够严格；
- Data/Translation/GC frontier 在初始化时为每个 stream 预留完整 block；
- FTL 统计缺少 pool/channel measurement window，无法直接计算分层 DWPD；
- batch 结束仍等待 `cin.get()`。

实现没有复制 v7 MQSim，而是在当前 TRIM 基线上补齐正确性、资源模型、依赖调度和稳定统计接口。

## 2. 系统边界

```text
DWPDSim policy
    |
    | RelocateIntent / placement / eviction decision
    v
DWPDSim Simulator
    |
    | canonical READ/WRITE/TRIM + sequence + dependency
    v
DWPDSim MQSim converter
    |
    | one workload, one SSD config, N stream traces
    v
MQSim host dependency scheduler
    |
    | ordinary NVMe READ/WRITE/DSM
    v
shared controller/FTL
    |                         |
    v                         v
SLC channel pool         TLC channel pool
    |                         |
    +------ XML results ------+
```

DWPDSim 是逻辑状态和地址真值的唯一所有者。MQSim 只观察已经降低后的物理请求，并维护其
自身的 NVMe、FTL、NAND、GC 和 wear 状态。MQSim 的物理 GC 不产生新的 DWPDSim TRIM，也不
改变上层 residency；上层释放地址时产生的 TRIM 才表示逻辑失效。

## 3. DWPDSim 到 MQSim 的输入契约

### 3.1 Operation 集合

MQSim 输入操作固定为：

```text
0 = WRITE
1 = READ
2 = TRIM
```

Relocation 是 DWPDSim 的管理意图，不是设备命令。DWPDSim 在进入 converter 前已经把它降低
为：

```text
READ(source) -> WRITE(destination) -> TRIM(source)
```

access-triggered relocation 可以复用本次 storage hit 的 READ；background relocation 必须显式
产生 source READ。MQSim 不根据 LBA、时间间隔或相邻行猜测哪些请求属于 relocation。

### 3.2 扩展 trace 格式

普通 MQSim 五列 trace 无法表示跨 flow completion dependency。新增显式格式
`DWPDSIM_DEPENDENCY_V1`：

```text
arrival_time_ns device_id start_lba sector_count operation request_id depends_on_request_ids
```

约束：

- `request_id` 是 converter 为实际 MQSim command 分配的 scenario 内全局连续编号，不复用
  DWPDSim semantic trace sequence；
- `depends_on_request_ids = -1` 表示无前驱，否则为逗号分隔的一个或多个 request id；同一
  前驱重复出现时按一个依赖处理；
- dependency 可以跨 NVMe flow 和 channel pool；
- 一个请求可以等待多个直接前驱，manager 只在全部前驱完成后提交；多步顺序通过逐级
  request id 表达；
- `arrival_time_ns` 是最早可提交时间，实际提交时间还受 predecessor completion 限制；
- 每个 trace 文件内部 arrival time 非递减；
- `sector_count` 必须处于 NVMe command 可表达范围；较大的 semantic record 由 converter
  拆成 command 子链；首个 chunk 合并原 semantic predecessor 的末尾 chunk 和同 pool/LBA
  hazard 前驱，后续 chunk 依赖同一 record 的前一个 chunk；
- MQSim 不需要 `move_id`、reason、NodeId、source sequence 或 chunk 语义。converter manifest
  保存 `request_id -> source_sequence, chunk_index, chunk_count` 及原始地址范围，用于结果关联
  和 relocation range 校验。

普通五列 trace 仍作为 MQSim 的通用输入存在，但 DWPDSim vNext 的准确 relocation latency
实验必须使用 `DWPDSIM_DEPENDENCY_V1`，不能退化成时间间隔近似。

### 3.3 Converter 责任

DWPDSim converter 必须：

1. 保留一个全局纳秒时间原点，禁止按 tier 重置 timestamp；
2. 将 `(tier, tier_local_stream_id)` 确定性映射成 MQSim flow id；
3. 使用 DWPDSim 输出的 pool-local 地址，不再根据 node id 重新分配紧凑 LBA；
4. 校验 offset/length 的 512-byte 对齐并转换为 sector；
5. 为每条实际 command 分配全局连续 `request_id`；将超过单个 NVMe command 上限的
   semantic record 按连续地址切分，并按前述 chunk-chain 规则重建 dependency；
6. 在 manifest 中保留 source sequence、chunk index 和 command id 映射，不用固定 timestamp
   gap 代替 completion；
7. 生成一个 workload、一个 SSD config 和每个 configured flow 一个 trace；空 flow 也保留固定
   flow id、pool id 和空 trace；
8. 空 flow 不生成 sentinel I/O；
9. 输出 manifest，记录 DWPDSim tier/stream、MQSim flow/pool、容量、时间窗口和输入计数。

source 和 destination 可以使用不同 LBA；MQSim 不要求 relocation 前后保持相同数值地址。
READ 访问 source placement，WRITE/TRIM 分别访问 destination/source placement 即可。

## 4. Completion Dependency

### 4.1 Host 侧依赖调度器

在 Host System 增加 scenario 级 `RequestDependencyManager`，而不是把 dependency 实现到 FTL：

```cpp
struct ExternalRequestMetadata {
    std::uint64_t request_id;
    std::vector<std::uint64_t> predecessor_ids;
    sim_time_type earliest_submit_time;
};
```

每个 trace flow 解析请求后：

```text
arrival event
    |
    +-- no predecessors ---------------------> submit to NVMe
    |
    +-- all predecessors completed ----------> submit to NVMe
    |
    +-- any predecessor not completed -------> dependency wait queue
```

READ、WRITE 和 TRIM 都通过现有 host completion 路径通知 manager。一个请求完成时，manager
在当前模拟时间释放直接依赖者；依赖者的实际提交时间为：

```text
max(earliest_submit_time, max(predecessor_completion_times))
```

这样 relocation 顺序不依赖 flow 文件扫描顺序、NVMe queue fetch size 或人为时间间隔。

### 4.2 输入校验

仿真开始前必须校验整个 scenario：

- request id 不重复；
- 每个 predecessor 存在且不是自身；
- dependency graph 无环；
- READ/WRITE/TRIM 地址均落在所属 pool 的 logical namespace；
- 仿真结束时不存在 unresolved dependency。

MQSim 只校验 command 级图结构，不根据 sector 数量或地址推断 relocation。semantic
predecessor 的存在性、拆分前后 sector range 一致性以及 relocation READ/WRITE/TRIM 的 chunk
对应关系由 converter 在生成 manifest 时校验。

错误输入必须明确失败，不能把被阻塞请求静默丢弃。

### 4.3 仿真结束

删除 v7 的 sentinel TRIM 方案。正确的结束条件是：

- 所有 flow 已到 EOF；
- dependency wait queue 为空；
- NVMe submission/completion queue 已清空；
- data cache、FTL、TSU 和 NAND 中没有未完成事务；
- simulator event queue 已耗尽。

空 trace 合法：它不注册首个 arrival event，也不需要伪造请求。`finish` 时如果只剩 unresolved
dependency，则作为输入或实现错误退出。

## 5. 显式物理 Pool

### 5.1 Pool 配置

不沿用 v7 通过“多个 stream 的 channel list 相同”隐式推断共享 pool 的做法。SSD 配置显式定义：

```xml
<Flash_Pool_Parameter_Set>
    <Pool_ID>slc</Pool_ID>
    <Channel_IDs>0,1</Channel_IDs>
    <Logical_Capacity_In_Sectors>...</Logical_Capacity_In_Sectors>
    <Media_Profile_ID>slc_profile</Media_Profile_ID>
</Flash_Pool_Parameter_Set>

<Flash_Pool_Parameter_Set>
    <Pool_ID>tlc</Pool_ID>
    <Channel_IDs>2,3,4,5,6,7</Channel_IDs>
    <Logical_Capacity_In_Sectors>...</Logical_Capacity_In_Sectors>
    <Media_Profile_ID>tlc_profile</Media_Profile_ID>
</Flash_Pool_Parameter_Set>
```

workload flow 只声明 `Pool_ID`，不再重复维护一套可能不一致的 `Channel_IDs`。DWPDSim 场景要求：

- pool id 唯一；
- pool channel 非空且互不重叠；
- 每个 flow 只属于一个 pool；
- 每个被使用的 channel 只属于一个 pool；
- logical capacity 不超过该 pool 扣除 overprovisioning 后的物理容量；
- 同一 pool 的所有 stream 看到相同的 pool-local logical namespace。

本版本沿用 MQSim 现有的 8 个 NVMe I/O queue pair，单个 scenario 因此最多定义 8 个 flow，空
flow 也计入该上限。converter 在生成 workload 前必须拒绝超过该上限的固定 flow 拓扑；本次
重构不扩展 NVMe queue register ABI。

### 5.2 共享容量语义

当前 MQSim 会按引用同一 plane 的 stream 数量静态除分容量。目标行为是：

- pool 是容量和 GC 隔离单位；
- pool 内 stream 共享每个 plane 的 free block pool；
- stream 仍保留独立 mapping domain 和 Data/Translation/GC write frontier；
- write frontier 首次真正写入时才分配；写满后释放指针，下次写入再获取；
- 不为不会访问某个 pool/plane 的 stream 预留 block；
- GC 只在 victim 所属 plane/pool 内执行，不跨 pool 搬页。

每个 stream 拥有完整 pool-local LBA 范围不等于它独占相同容量。DWPDSim 负责保证所有 stream
的 aggregate live bytes 不超过 pool capacity，MQSim 负责模拟共享物理块竞争和 GC。

### 5.3 Preconditioning

DWPDSim 默认通过真实 WRITE trace 建立初始状态，关闭 MQSim 的 per-flow preconditioning。
如果以后需要 preconditioning，必须改成每个 pool 只预填一次，并按 pool 内 stream 分布写入；
不能让每个 stream 分别按完整 pool capacity 预填。

## 6. 异构 SLC/TLC Media Profile

v7 在一个 SSD 中仍只有一套 `Flash_Parameter_Set`，因此它的 SLC/TLC 只表示 channel/capacity/GC
pool，不能表示不同 NAND latency。目标版本增加显式 media profile：

```xml
<Flash_Media_Profile>
    <Media_Profile_ID>slc_profile</Media_Profile_ID>
    <Flash_Technology>SLC</Flash_Technology>
    <Page_Read_Latency_LSB>...</Page_Read_Latency_LSB>
    <Page_Program_Latency_LSB>...</Page_Program_Latency_LSB>
    <Block_Erase_Latency>...</Block_Erase_Latency>
    <Block_PE_Cycles_Limit>...</Block_PE_Cycles_Limit>
</Flash_Media_Profile>
```

每个 pool 引用一个 profile，`SSD_Device` 按 channel 所属 profile 构造 `Flash_Chip`。第一阶段只
允许以下参数按 pool 不同：

- flash technology；
- page read/program latency；
- block erase latency；
- suspend program/erase latency；
- PE cycle limit。

以下几何参数保持全设备一致：

- chips per channel；
- dies per chip；
- planes per die；
- blocks per plane；
- pages per block；
- page/metadata capacity。

这是为了避免一次重构同时改写 PPA 编码、plane manager、FTL page bitmap 和 TSU topology。
SLC/TLC 容量差异第一版通过 channel 数和 pool logical capacity 表达。后续若要支持异构几何，
必须另立设计，不能在 profile 中接受参数却继续按全局几何寻址。

`Host_Parameter_Set`、`Device_Parameter_Set` 和公共 `Flash_Parameter_Set` 均为 scenario 实例
状态。公共 flash 配置只包含几何；technology、timing、suspension 和 PE limit 只存在于 media
profile。Flash chip、FTL preconditioning latency、TSU suspension 和 block PE 上限均按目标
flow/channel 的 pool/profile 选择。erase histogram 按所有 profile 的最大 PE limit 分配，block
寿命检查使用其自身 channel 的 limit。

## 7. TRIM、LBA Reuse 与 GC 正确性

当前 `c4c214f` 的基础 TRIM 保留，包括 host 的独立 trim request/byte 统计。以下 v7 修复需要
以独立提交迁移并补测试。

### 7.1 Sector-granular validity

- mapping entry 保留 `WrittenStateBitmap`；
- partial TRIM 只清除请求覆盖的 sector bits；
- 只有 bitmap 为空时才使物理 page invalid；
- repeated TRIM 是 no-op，但仍可计入 received/requested command；
- effective trimmed sectors 只统计本次从 valid 变 invalid 的 sectors。

READ、WRITE、TRIM 都必须使用 stream-local LSA 计算 page boundary，避免 host range 起点未对齐
时三类操作落入不同 LPA。

### 7.2 Stale TRIM 和 LBA reuse

TRIM 在修改 block bookkeeping 前必须验证当前 mapping 指向的物理版本仍属于同一
`(stream_id, LPA)`。已经擦除、重分配或被更新 mapping 替代的旧物理页不能再次减少 valid
page count。

`Invalidate_page_in_block` 必须幂等：

- block owner 不匹配时不修改计数；
- invalid bitmap 已置位时不重复扣减；
- valid/invalid/free counter 始终保持守恒。

GC 扫描 programmed page 时，如果 metadata 是 `NO_LPA`、越界，或 mapping 已指向新 PPA，
应修复旧页 bookkeeping 并跳过 copy，不能访问非法 GMT entry，也不能复制 stale data。

### 7.3 TRIM/GC race

需要覆盖：

```text
GC reads old physical page
        |
        v
host TRIM invalidates its LPA
        |
        v
cancel pending GC write
release LPA barrier
allow victim erase to finish
```

GC 选择 victim 后，必须等待所有指向该 block 的 in-flight user program 完成，再安装 physical
block barrier 和扫描 page metadata。否则可能看到已分配但尚未提交 LPA metadata 的 page。

这些属于 FTL 正确性修改，对所有 GC policy 生效，不得仅在 DWPDSim workload 下打开。

## 8. GC Policy 与 Write Frontier

v7 的三层 greedy 不直接覆盖现有 `GREEDY`，而是增加显式策略，例如：

```text
KV_THREE_GREEDY:
    1. invalid page count 最大
    2. 相同时 Last_write_time 最早
    3. 再相同时 Erase_count 最小
```

候选必须同时满足：block 已写满、存在垃圾页、不是活动 write frontier、没有危险的 in-flight
事务并通过 `is_safe_gc_wl_candidate`。找不到合法 victim 时返回，不允许从 block 0 或任意默认
位置开始 GC。

RR、Algorithm1、Algorithm2 对比时固定使用相同 GC policy。GC policy 是物理实验参数，不由
DWPDSim placement policy 动态切换。

Data、Translation 和 GC frontier 都采用 lazy allocation，解决小规模 DWPD 配置下每个 stream
预留多个完整 block 导致的容量和 GC 偏差。

## 9. 统计 ABI

### 9.1 Host flow

每个 flow 至少输出：

```text
Generated_Request_Count
Completed_Request_Count
Read_Request_Count
Write_Request_Count
Trim_Request_Count
Bytes_Transferred_Read
Bytes_Transferred_Write
Bytes_Trimmed_Requested
Average/Min/Max_Response_Time_Read
Average/Min/Max_Response_Time_Write
Average/Min/Max_Response_Time_Trim
Dependency_Wait_Time_Total
Dependency_Wait_Time_Max
```

TRIM 不能落入 WRITE completion 分支。生成数、完成数和字节数必须与 converter manifest 对账。

### 9.2 FTL 和 pool

稳定输出名称，不复制 v7 与当前 fork 之间互不兼容的临时字段：

```text
Received_Trim_Command_Count
Requested_Trim_Sector_Count
Effective_Trimmed_Sector_Count
Pages_Invalidated_By_Trim
GC_Execution_Count
GC_Page_Read_Count
GC_Page_Program_Count
```

每个 pool 和 channel 输出：

```text
Host_Read_Bytes
Host_Write_Bytes
Requested_Trim_Bytes
Effective_Trimmed_Bytes
Flash_Read_Command_Count
Flash_Program_Command_Count
Flash_Erase_Command_Count
Total_Block_Erase_Count
Max_Block_Erase_Count
Measurement_Total_Block_Erase_Count
Measurement_Max_Block_Erase_Count
Logical_Capacity_Bytes
Physical_Capacity_Bytes
PE_Cycle_Limit
```

结果 XML 必须记录 simulator 版本、配置 hash、pool/channel 映射、时间单位和 measurement window，
使 DWPDSim summary 可追溯。

稳定节点路径如下；flow、pool 和 channel 均按显式 ID 关联，不按 XML 顺序关联：

```text
MQSim_Results/Host/Host.IO_Flow
    Flow_ID, Pool_ID, Time_Unit, request counts, integer byte counts, latency

MQSim_Results/SSDDevice/SSDDevice.Configuration
    Statistics_ABI_Version="1"
    Configuration_Hash=<SSD XML 原始字节的十进制 FNV-1a 64>
    Configuration_Hash_Algorithm="fnv1a64-raw-xml"
    Time_Unit="nanosecond"
    Measurement_Start_Time_Ns, Measurement_End_Time_Ns

MQSim_Results/SSDDevice/SSDDevice.FTL
    Received_Trim_Command_Count, Requested_Trim_Sector_Count,
    Effective_Trimmed_Sector_Count, Pages_Invalidated_By_Trim,
    GC_Execution_Count, GC_Page_Read_Count, GC_Page_Program_Count

MQSim_Results/SSDDevice/SSDDevice.Pool[@ID]
    Media_Profile_ID, Channel_IDs, capacity, PE limit, host/trim/flash/GC counts,
    Measurement_Host_Write_Bytes, Measurement_Flash_Programmed_Bytes

MQSim_Results/SSDDevice/SSDDevice.Channel[@ID]
    Pool_ID, Media_Profile_ID, capacity, PE limit, host/trim/flash/erase counts,
    Measurement_Flash_Programmed_Bytes
```

`SSDDevice.FTL` 的 TRIM 与 GC 整数计数严格等于全部 `SSDDevice.Pool` 对应字段之和。

### 9.3 Measurement window 与公式

measurement window 进入 SSD XML 配置，不使用 v7 的进程环境变量：

```xml
<Measurement_Start_Time_Ns>...</Measurement_Start_Time_Ns>
<Measurement_End_Time_Ns>...</Measurement_End_Time_Ns>
```

按 erase completion timestamp 计入半开区间 `[start, end)`。输出原始计数，派生指标由
DWPDSim converter 明确计算：

```text
measurement_days = (end_ns - start_ns) / 86400e9

host_DWPD(pool) =
    host_write_bytes_in_window
    / logical_capacity_bytes
    / measurement_days

nand_DWPD(pool) =
    flash_programmed_bytes_in_window
    / logical_capacity_bytes
    / measurement_days

write_amplification(pool) =
    flash_programmed_bytes_in_window
    / host_write_bytes_in_window

max_block_PE_per_day(pool) =
    measurement_max_block_erase_count
    / measurement_days
```

当分母为零时输出 null/undefined，不输出无穷或 NaN。v7 的 `120:12` endurance budget 是上层
policy/实验参数，不硬编码到 MQSim；MQSim 只输出实际计数和 media profile 的 PE limit。

## 10. 批处理和可观测性

- 删除正常结束和 fatal path 中的 `cin.get()`；
- 正常 batch 执行不等待终端输入；
- 删除 v7 的无条件 `Dump_kv_debug_state()`；
- 不逐 I/O 打印日志；
- 可选 `Enable_Request_Completion_Log` 输出
  `request_id,flow_id,operation,arrival_time_ns,dependency_release_time_ns,submit_time_ns,completion_time_ns,dependency_wait_ns`，
  默认关闭；`dependency_release_time_ns` 是全部前驱完成后进入 flow 的时刻，`submit_time_ns`
  是实际进入 NVMe/SATA queue 的时刻；
- fatal error 返回非零 exit code，并在 stderr 中给出 scenario、flow、request id 和原因；
- XML 只包含结构化统计，不混入依赖 stdout 文本解析的 debug 数据。

## 11. 主要源码改造面

| 模块 | 目标修改 |
|---|---|
| `src/host/ASCII_Trace_Definition.*` | 定义并解析 dependency trace schema |
| `src/host/IO_Flow_Trace_Based.*` | 空 trace、request id、predecessor、全场景预检 |
| `src/host/IO_Flow_Base.*` | 三类请求独立完成/字节/延迟统计 |
| `src/host/Host_IO_Request.*` | 携带 opaque request id 和 dependency metadata |
| `src/exec/Host_System.*` | scenario 级 dependency manager 和 drain 校验 |
| `src/exec/IO_Flow_Parameter_Set.*` | `Trace_Format`、`Pool_ID`、completion log 配置 |
| `src/exec/Device_Parameter_Set.*` | flash pool、media profile、measurement window 配置 |
| `src/exec/SSD_Device.*` | 按 channel profile 构造 chip，建立 pool 映射 |
| `src/utils/Logical_Address_Partitioning_Unit.*` | pool-local namespace，不按 stream 静态除分容量 |
| `src/ssd/Host_Interface_NVMe.*` | stream-local segmentation 和正确 DSM completion |
| `src/ssd/Address_Mapping_Unit_Page_Level.*` | partial/stale TRIM、LBA reuse 和 GC mapping 修复 |
| `src/ssd/Flash_Block_Manager*` | 幂等 invalidation、lazy frontier、pool/channel wear 统计 |
| `src/ssd/GC_and_WL_Unit*` | deferred barrier、TRIM/GC race、三层 greedy |
| `src/ssd/FTL.*`、`src/ssd/Stats.*` | 稳定 FTL/pool measurement 统计 ABI |
| `src/nvm_chip/flash_memory/Flash_Chip.*` | channel-specific media profile |
| `src/main.cpp` | 非交互 batch 退出和结构化错误码 |

## 12. 实现阶段

### 阶段一：锁定当前基线

- 保留 `c4c214f` 的 READ/WRITE/TRIM 行为和现有 trim functional test；
- 增加输入/host/FTL 计数对账测试；
- 建立一个可快速执行的小型 page-level、cache-off SSD 配置。

### 阶段二：TRIM/GC 正确性

- 迁移 sector validity、stale TRIM、LBA reuse 修复；
- 实现 idempotent invalidation；
- 实现 GC stale-page repair、TRIM/GC cancellation 和 deferred barrier；
- 不在这一阶段改变 pool、GC selection 或统计 schema，以便定位回归。

### 阶段三：共享 Pool

- 引入显式 Pool_ID 和 pool-local capacity；
- 移除 per-stream plane quota；
- 实现 lazy write frontiers；
- 增加 `KV_THREE_GREEDY`，不覆盖原有 `GREEDY`；
- 验证 pool 内共享和 pool 间隔离。

### 阶段四：异构 Media Profile

- 将全局 Flash_Parameter_Set 拆成公共 geometry 和 pool media profile；
- 按 channel 创建 SLC/TLC Flash_Chip；
- 将 PE limit 和 timing 查询改为 channel/pool aware；
- 验证同负载下配置差异确实反映到 NAND latency 和 wear 输出。

### 阶段五：Dependency Replay

- 实现扩展 trace parser 和 scenario 级依赖图；
- 完成跨 flow completion release；
- 修复 empty flow 和自然 drain；
- 删除 sentinel、固定 gap 和 Queue_Fetch_Size 排序依赖；
- 增加可选 per-request completion log。

### 阶段六：统计和 DWPDSim 集成

- 固化 host/FTL/pool/channel XML ABI；
- 实现 measurement window 和 DWPD 所需原始计数；
- 更新 DWPDSim converter 为单次 MQSim 运行；
- 对相同 canonical trace 校验 converter manifest 与 MQSim 完成统计；
- 用 baseline、RR、Algorithm1、Algorithm2 验证物理配置完全相同，只有输入 trace 不同。

## 13. 测试矩阵

至少覆盖以下小规模确定性场景：

1. partial-page TRIM 后未裁剪 sector 仍可读；
2. full-page TRIM 只 invalid 一次；
3. repeated TRIM 不破坏 valid/invalid/free counter；
4. TRIM 后重写同一 LBA，延迟到达的旧 invalidation 不删除新版本；
5. GC read 与 host TRIM 竞争时取消 GC write 且最终 erase 完成；
6. GC 等待 candidate block 上的 in-flight program 后再安装 barrier；
7. 两个 stream 共享一个 pool，不再各自获得静态一半 plane；
8. 未使用 stream 不预留 Data/Translation/GC frontier；
9. SLC/TLC pool 的 channel、LBA 和 GC 不互相越界；
10. 不同 media profile 对相同命令产生不同配置期望的 latency；
11. 跨 flow READ completion 后才提交 WRITE，WRITE completion 后才提交 TRIM；
12. access READ 复用时不生成第二条 READ；
13. 空 flow 无 sentinel 也能正常结束；
14. unresolved、duplicate、missing 和 cyclic dependency 明确失败；
15. host READ/WRITE/TRIM counts 和 bytes 与输入 manifest 完全一致；
16. measurement window 边界分别覆盖 start-inclusive、end-exclusive；
17. 相同输入、seed 和配置得到完全相同的结构化结果。

每个阶段先运行针对性测试，再运行现有 `tests/trim/test_trim.sh`。最终集成必须使用一个包含
SLC/TLC relocation、后台 idle eviction 和 GC 的小型端到端 workload。

## 14. 不从 v7 迁移的内容

- v7 adapter、online replay、storage action CSV 和任何 Radix 数据结构；
- `MIGRATE` action 及其 MQSim 特殊识别；
- 1 ns/50 ms controller gap；
- `Queue_Fetch_Size=1` 作为顺序保证；
- 空 stream 和测量窗口末尾的 sentinel TRIM；
- 无条件 FTL/Radix debug dump；
- 用 `else` 把 TRIM completion 计入 WRITE 的统计行为；
- 仅通过一套全局 `Flash_Technology` 把 channel group 命名为 SLC/TLC；
- 通过环境变量隐式配置 measurement window；
- 任何 `120:12` policy budget、placement 或后台 eviction 逻辑。

## 15. 验收条件

改造完成至少满足：

1. DWPDSim 的所有 tier/stream 在一个 MQSim scenario 和全局时间轴中运行；
2. MQSim 输入 operation 只有 READ、WRITE、TRIM；
3. background relocation 的 source READ、destination WRITE、source TRIM 依 completion 运行；
4. access-triggered relocation 可以复用已有 READ，不重复计费；
5. dependency 正确性不依赖 timestamp gap、flow 顺序或 Queue_Fetch_Size；
6. 同 pool stream 共享物理容量，不存在静态 per-stream plane quota；
7. 不活跃 stream 不预留 write-frontier block；
8. SLC/TLC pool 可以使用不同 latency 和 PE limit，且仍共享 host/controller；
9. partial/repeated/stale TRIM 和 TRIM/GC race 均通过确定性测试；
10. host/FTL/pool/channel 统计名称稳定且与输入 manifest 对账；
11. 空 flow 和普通 flow 都无需 sentinel 即可自然 drain；
12. measurement window、容量、时间单位和公式在结果中可追溯；
13. MQSim 中不存在 DWPDSim policy、Radix 或 placement 状态；
14. v7 只作为测试和算法参考，不进入最终运行链路。
