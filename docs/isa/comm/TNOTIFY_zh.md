# TNOTIFY

## 简介

向远端NPU发送标志通知。用于NPU之间的轻量级同步，无需传输大量数据。

## 数学语义

`NotifyOp::Set` 时：

$$\mathrm{signal}^{\mathrm{remote}} = \mathrm{value}$$

`NotifyOp::AtomicAdd` 时：

$$\mathrm{signal}^{\mathrm{remote}} \mathrel{+}= \mathrm{value} \quad (\text{原子操作})$$

## 汇编语法

```text
tnotify %signal_remote, %value {op = #pto.notify_op<Set>} : (!pto.memref<i32>, i32)
tnotify %signal_remote, %value {op = #pto.notify_op<AtomicAdd>} : (!pto.memref<i32>, i32)
```

## C++内建接口

声明于 `include/pto/comm/pto_comm_inst.hpp`：

```cpp
template <typename GlobalSignalData, typename... WaitEvents>
PTO_INST void TNOTIFY(GlobalSignalData &dstSignalData, int32_t value, NotifyOp op, WaitEvents&... events);
```

## 约束

- **类型约束**：
    - `GlobalSignalData::DType` 必须为 `int32_t`（32位信号）。
- **内存约束**：
    - `dstSignalData` 必须指向远端地址（目标NPU）。
    - `dstSignalData` 应4字节对齐。
    - `dstSignalData` 必须对该 rank **可读**：`NotifyOp::Set` 发布前会先读取当前值
      （见下方语义），因此不支持只写的远端映射。
- **操作语义**：
    - `NotifyOp::Set`：按字发布 `value`：返回时目标字恰好等于 `value`，与该字共享 64 B cache line 的其它字不受影响（整行 `dcci` 回写会破坏它们）。a2a3 上 `st_atomic` 只接受 `ATOMIC_SUM`，因此实现为对回绕差值 `(value - current)` 的原子加。
    - **每个地址只能有一个写入者（并发）**：同一字上的并发 `Set` 不保证“最后写入者胜出”。每个写入者都会重新读取并重新发布，因此各 delta 的累加可能落在任何 rank 都未写入过的值上——两个分别写 `1000` 和 `1001` 的 rank 竞争后可能留下 `2001`。请为每个发布 rank 分配独立的字（例如每 rank 一个槽位）。
    - `NotifyOp::AtomicAdd`：使用 `st_atomic` 指令执行硬件原子加。

## 并发与 cache line 粒度

GM 写入以整条 64 字节 cache line 到达 DDR，因此若发布操作弄脏了目标行，会把同一行中的相邻字一并写回。在 A2/A3 上，行归属由调用方负责：由不同 AIV 或 NPU 并发更新的信号必须各自独占 **64 字节 cache line**——64 字节对齐、步长不小于 64 字节；由同一个 AIV 串行执行的多次通知不受此限制（规则见 hw-native-sys/pto-isa#322；cache line 长度见 Ascend 标量单元文档）。软 SYNCALL 工作区已经编码了该规则（`pto/common/type.hpp` 中的 `SYNCALL_SOFT_WORKSPACE_INT32 = 16`），pypto 也以同样的规则提示并发标量写入。

`NotifyOp::Set` 不需要这种填充，因为它**不会弄脏目标行**（见上方语义）：紧密排列的 `int32` 信号是安全的——在 P=2 与 P=4 下以默认步长实测 0/50 失败，而以 cache line 为步长（16 × `int32` = 64 B）同样保持 0/50。由此有两点：

- 遵循 64 字节约定的调用方不受影响；对所有**基于普通存储**的发布路径（普通标量存储、SYNCALL、`pl.write` 风格的写入），该规则仍然是必需的，因为整行回写依然可能破坏相邻字。
- `Set` 只对其**自身**的存储保持行安全：若某个 AIV 还向承载其所通知信号的那一行写入数据，仍可能清理该行并破坏对端已发布的值。请把数据写入放在独立的行上。

## 示例

### 基础Set通知

```cpp
#include <pto/comm/pto_comm_inst.hpp>

using namespace pto;

void notify_set(__gm__ int32_t* remote_signal) {
    comm::Signal sig(remote_signal);

    // 将远端信号置为 1
    comm::TNOTIFY(sig, 1, comm::NotifyOp::Set);
}
```

### 原子计数器自增

```cpp
#include <pto/comm/pto_comm_inst.hpp>

using namespace pto;

void atomic_increment(__gm__ int32_t* remote_counter) {
    comm::Signal counter(remote_counter);

    // 对远端计数器原子加 1
    comm::TNOTIFY(counter, 1, comm::NotifyOp::AtomicAdd);
}
```

### 生产者-消费者模式

```cpp
#include <pto/comm/pto_comm_inst.hpp>

using namespace pto;

// 生产者：数据就绪后发送通知
void producer(__gm__ int32_t* remote_flag) {
    // ... 生产数据 ...

    comm::Signal flag(remote_flag);
    comm::TNOTIFY(flag, 1, comm::NotifyOp::Set);
}

// 消费者：等待数据就绪
void consumer(__gm__ int32_t* local_flag) {
    comm::Signal flag(local_flag);
    comm::TWAIT(flag, 1, comm::WaitCmp::EQ);

    // ... 消费数据 ...
}
```

## 说明

- **按字发布的 `Set` 有测试覆盖**：`tests/npu/a2a3/comm/st/testcase/tnotify_set_false_share`
  校验 `Set` 不会影响与其共享 64 B cache line 的相邻字；`tnotify`（`RuntimeOp_Set`）
  校验每个发布 rank 使用独立槽位。
- **本头文件在 CANN 中另有一份副本。** CANN 还以扁平路径提供该副本：
  `$ASCEND_HOME_PATH/aarch64-linux/include/pto/comm/TNotify.hpp`（无 `a2a3/` 路径层级）。
  CANN 9.0.0 中的副本仍是修复前的 `Set`（先对整行 `dcci` 失效，再做普通按字存储），
  因此若 include 顺序先命中 CANN，或下游直接分发 CANN 的目录，则会继续保留旧行为。
  修改本头文件后需在下游重新带入（re-vendor）。
