# Foreword
This explanation is intentionally written at an accessible, beginner-friendly level rather than as a formal, production-grade README. The project exists primarily for self-education, and the documentation reflects the kind of clear, step-by-step guidance I wish I had when I first explored lock-free, low-latency programming.

This project does not provide a makefile (yet?), nor it is distributed in the form of library.

## Prerequisites
Before reading the internals of this queue, you should already understand the following topics:

0. **C++ programming language**
1. **Multithreading basics** — how threads run concurrently.  
2. **Mutexes / synchronization primitives** — what mutual exclusion is.  
3. **Atomics** — what atomic operations are and why they matter.  
4. **Memory ordering** — especially `relaxed`, `acquire`, and `release`.  
5. **Lock‑free programming** — basic understanding of non‑blocking algorithms.  
6. **Ring buffers (circular buffers)** — fixed‑size circular queues.  
7. **Ring buffers in lock‑free scenarios** — using wrap‑around indices.

Recommended reading:
- (1) https://www.geeksforgeeks.org/cpp/multithreading-in-cpp/
- (2) https://www.geeksforgeeks.org/cpp/std-mutex-in-cpp/
- (3) https://www.geeksforgeeks.org/cpp/cpp-11-atomic-header/
- (4) https://gcc.gnu.org/wiki/Atomic/GCCMM/AtomicSync
- (5) https://preshing.com/20120612/an-introduction-to-lock-free-programming/
- (6) https://www.geeksforgeeks.org/dsa/how-to-manage-full-circular-queue-event/
- (7) https://rigtorp.se/ringbuffer/

---
## High‑Level Overview
This is a **Multiple Producer, Single Consumer (MPSC)** lock‑free queue implemented as a **ring buffer**.

There are two major ways to implement MPSC queues:
1. **Dynamic linked‑node queues** (Michael–Scott queue) — flexible size but requires memory allocation.  
2. **Ring‑buffer queues** — fixed capacity, but extremely fast and cache‑friendly.

For high‑performance systems, dynamic allocation can sometimes be critical overhead. A ring buffer is usually preferred because:
- No allocation, no deallocation.  
- Predictable memory layout.  
- Excellent cache behavior.  
- Very small atomic footprint.

A fixed capacity is **not a limitation** in most high‑throughput designs — if the queue overflows, your system is already lagging behind, and you must redesign your producers or pipeline. In such case, other container should be preferred (e.g. Multiple Producer Multiple Consumer Queue (MPMCQueue)).

---

## Why Relaxed Atomics Are Used
In many places we use `std::memory_order_relaxed` because the algorithm cares only about the *atomicity* of index updates — not their ordering relative to other loads/stores.

Where ordering *does* matter (consumer must not read an unwritten element), we use `release` / `acquire` pairing.

---

# Push Algorithm (Multiple Producers)
Below is the conceptual algorithm for `push()`.

```
loop forever:
    load rw = reserveWriteIndex
    load r = readIndex

    if buffer full : rw - r == capacity -> return false

    if CAS(reserveWriteIndex, rw -> rw+1) succeeds:
        write element into buf[rw & maxIndexMask]
        advance commitWriteIndex : CAS(release)
        return true
```
---

## Step‑by‑Step Explanation
### **1. Load current write index `rw` and read index `r`**
We read the indices using relaxed loads. 
We only need their current numeric values.

### **2. Check if the buffer is full**
To check if the buffer is full (`rw - r == capacity`), indexes must grow infinitely.
Cheap wrapping occurs automatically on SIZE_T_MAX overflow.
Writing uses `buf[rw & maxIndexMask]` with `maxIndexMask = capacity - 1`.

Current approach might be not as intuitive as the [modulo wrapping approach](#why-not-use-modulo-wrap-on-indexes), however, the former does not face several minor cons the latter possesses, with the single con of the provided solution being the `capacity` limitation of the power of 2. 

### **3. Compete for the write index via CAS**
Each producer tries:
```
CAS(reserveWriteIndex: rw -> rw+1)
```
If it fails, it means **another producer advanced the index first**, so we retry.

### **4. Write the element**
After a successful CAS, each producer writes into its reserved slot:
```
buf[rw & maxIndexMask] = msg
```
At this moment, the slot contains valid data — but it is guaranteed only to the producer thread, which has commited this write just now.
**The consumer might not see it yet**.

### Why we need two write indices
Imagine we have only one write index.
For simplicity, only one producer is featured in the example.

0. The queue is empty. Both `writeIndex` and `readIndex` indexes are set to `0`.
1. Producer reserves an index `0` and advances it to `1`, but sleeps instantly - just *before writing*.
2. Consumer sees `readIndex == 0` and `writeIndex == 1`, passing non-empty buffer check.
3. Consumer reads `buf[readIndex]` which still contains garbage.

Note: We can't advance the write index after the write, as we have multiple producers, who need to syncrhonize writes between each other.

To solve this we use **two separate counters**:
- `reserveWriteIndex` — used by producers to reserve positions.  
- `commitWriteIndex` — used by the consumer to know *which elements are definitely written*.

Only after writing, the producer runs:
```
CAS(commitWriteIndex: rw -> rw+1, memory_order_release)
```
This CAS enforces **release semantics**, ensuring the consumer will see the correct data.

If multiple producers finish writes out of order, only one at a time may advance `commitWriteIndex`, so some must spin until their turn arrives. 
Fail during CAS (and the spin itself more than once) can happen only when the other CAS has succeeded, making it algorithmically impossible to cause any kind of locks. Hence "lock-free".

---

# Pop Algorithm (Single Consumer)
The pop logic is simple because only one thread ever performs it.

```
r = readIndex
cw = commitWriteIndex (memory_order_acquire)

if r == cw -> empty

readIndex = r + 1
return value by non-advanced index: return buf[r & maxIndexMask]
```

`commitWriteIndex` is loaded with **acquire semantics** because the consumer must observe the writes that were released by producers.

---

# ABA Situation (Benign in This Algorithm)
ABA example:
- Producer A stores slot 5.  
- Producer B also stores slot 5. 
- Producer A goes sleeping.
- Producer B takes control, and performs a push SIZE_T_MAX times, overflowing the type and advancing the slot to 5 again.
- Producer A wakes up and writes at the *new* slot 5, without knowing that this is not the *initially stored* slot at all.

However, this is *not* harmful because:
- slots are reused cyclically by design,  
- correctness does not depend on which thread writes a given slot.

Thus this is an **ABA situation**, not an ABA **problem**.

---

### Why not use modulo wrap on indexes
Of course, the index for write/read is most intuitively calculated as `(index + 1 ) % capacity`. 
However, there are a few minor cons for this approach: 
1)  **Full/Empty Detection**

Initially, when the buffer is empty, both read and write indexes are zero, so `readIndex` == `writeIndex` clearly indicates emptiness.
However, when the buffer wraps around, the `writeIndex` could again equal the `readIndex` — this is the same condition used for emptiness.
To distinguish fullness, one common approach is to check whether `writeIndex+1 == readIndex`.

The downside of this method is that the write operation can never fill the slot immediately before the read index, effectively leaving one slot unused at all times. 
While this may seem inelegant, it’s actually the cleanest and simplest way to distinguish between full and empty states.
Alternative approaches, like maintaining a fullFlag or adding extra bookkeeping, introduce additional logic and overhead.

Finally, there is no significant difference between `capacity` vs `capacity-1`, provided `capacity >= 2`, so nothing is affected.

*This is a minor to no con.*

2) **[ABA](#aba-situation-benign-in-this-algorithm) probability**

Chances for this situation increase from 1/SIZE_T_MAX to 1/CAPACITY, which is mathematically WAY more possible, however, still extremely unlikely. 

However, once again, we do not care about ABA at all (explained below).

*This is a minor to no con.*

3) **Performance Overhead**

Modulo operations are tens of times slower than bitwise AND (&).
Lock free structs are used when performance is the number 1 priority, hence each operation must happen as fast as possible. 

*This is a **real** con.*

