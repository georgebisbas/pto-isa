# TNOTIFY

## Introduction

Send flag notification to remote NPU. Used for lightweight synchronization between NPUs without transferring bulk data.

## Math Interpretation

For `NotifyOp::Set`:

$$ \mathrm{signal}^{\mathrm{remote}} = \mathrm{value} $$

For `NotifyOp::AtomicAdd`:

$$ \mathrm{signal}^{\mathrm{remote}} \mathrel{+}= \mathrm{value} \quad (\text{atomic}) $$

## Assembly Syntax

```text
tnotify %signal_remote, %value {op = #pto.notify_op<Set>} : (!pto.memref<i32>, i32)
tnotify %signal_remote, %value {op = #pto.notify_op<AtomicAdd>} : (!pto.memref<i32>, i32)
```

## C++ Intrinsic

Declared in `include/pto/comm/pto_comm_inst.hpp`:

```cpp
template <typename GlobalSignalData, typename... WaitEvents>
PTO_INST void TNOTIFY(GlobalSignalData &dstSignalData, int32_t value, NotifyOp op, WaitEvents&... events);
```

## Constraints

- **Type constraints**:
    - `GlobalSignalData::DType` must be `int32_t` (32-bit signal).
- **Memory constraints**:
    - `dstSignalData` must point to remote address (on target NPU).
    - `dstSignalData` should be 4-byte aligned.
    - `dstSignalData` must be **readable** by the issuing rank: `NotifyOp::Set` loads the
      current word before publishing it (see semantics below), so a write-only remote
      mapping is not supported.
- **Operation semantics**:
    - `NotifyOp::Set`: Word-published store of `value`: on return the target word holds exactly `value`, and other words sharing its 64 B cache line are left untouched (a whole-line `dcci` write-back would clobber them). On a2a3 this is `st_atomic` SUM of the wrapping `(value - current)` because `st_atomic` only accepts `ATOMIC_SUM`.
    - **One writer per word (concurrency)**: concurrent `Set`s to the *same* word are not last-writer-wins. Every writer re-reads the word and re-publishes, so the accumulating deltas can leave a value no rank ever wrote - two writers racing with `1000` and `1001` can leave `2001`. Give each publishing rank its own word (e.g. one slot per rank).
    - `NotifyOp::AtomicAdd`: Hardware atomic add using `st_atomic` instruction.

## Examples

### Basic Set Notification

```cpp
#include <pto/comm/pto_comm_inst.hpp>

using namespace pto;

void notify_set(__gm__ int32_t* remote_signal) {
    comm::Signal sig(remote_signal);

    // Set remote signal to 1
    comm::TNOTIFY(sig, 1, comm::NotifyOp::Set);
}
```

### Atomic Counter Increment

```cpp
#include <pto/comm/pto_comm_inst.hpp>

using namespace pto;

void atomic_increment(__gm__ int32_t* remote_counter) {
    comm::Signal counter(remote_counter);

    // Atomically add 1 to remote counter
    comm::TNOTIFY(counter, 1, comm::NotifyOp::AtomicAdd);
}
```

### Producer-Consumer Pattern

```cpp
#include <pto/comm/pto_comm_inst.hpp>

using namespace pto;

// Producer: notify when data is ready
void producer(__gm__ int32_t* remote_flag) {
    // ... produce data ...

    comm::Signal flag(remote_flag);
    comm::TNOTIFY(flag, 1, comm::NotifyOp::Set);
}

// Consumer: wait for data
void consumer(__gm__ int32_t* local_flag) {
    comm::Signal flag(local_flag);
    comm::TWAIT(flag, 1, comm::WaitCmp::EQ);

    // ... consume data ...
}
```

## Notes

- **Word-published `Set` is covered by tests**: `tests/npu/a2a3/comm/st/testcase/tnotify_set_false_share`
  checks that a `Set` does not disturb the neighbouring words sharing its 64 B cache line, and
  `tnotify` (`RuntimeOp_Set`) checks one slot per publishing rank.
- **This header is vendored into CANN.** CANN also ships a flattened copy at
  `$ASCEND_HOME_PATH/aarch64-linux/include/pto/comm/TNotify.hpp` (no `a2a3/` path segment).
  CANN 9.0.0's copy still contains the pre-fix `Set` (cache-line `dcci` invalidate followed by a
  plain word store), so a build whose include path resolves CANN ahead of this repository - or any
  distribution that copies CANN's tree - keeps the old behaviour. Re-vendor this header downstream
  when changing it.
