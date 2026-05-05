# Synchronization Primitives

**Document Revision:** 26h1.0  
**Source:** `zxfoundation/sync/`, `include/zxfoundation/spinlock.h`, `include/zxfoundation/atomic.h`

---

## 1. Atomic Operations

`include/zxfoundation/atomic.h` provides `atomic_t` (32-bit) and `atomic64_t` (64-bit) types with the standard load/store/add/sub/cmpxchg operations, implemented using z/Architecture's `CS` (Compare and Swap) and `CSG` (Compare and Swap, 64-bit) instructions.

---

## 2. Spinlock

`include/zxfoundation/spinlock.h` provides a ticket spinlock. Ticket spinlocks guarantee FIFO ordering, preventing starvation on highly contended locks.

| Function | Description |
|----------|-------------|
| `spin_lock(lock)` | Acquire; busy-wait with `DIAG 44` (yield hint) |
| `spin_unlock(lock)` | Release |
| `spin_lock_irqsave(lock, flags)` | Acquire + disable interrupts, save PSW mask |
| `spin_unlock_irqrestore(lock, flags)` | Release + restore PSW mask |

`irqsave`/`irqrestore` variants are required whenever a lock may be acquired from both process context and interrupt context.

---

## 3. Mutex

`zxfoundation/sync/mutex.c` — a sleeping mutex backed by a wait queue. Suitable for contexts where sleeping is permitted (not interrupt handlers).

| Function | Description |
|----------|-------------|
| `mutex_lock(m)` | Acquire; sleep if contended |
| `mutex_trylock(m)` | Non-blocking acquire; returns 0 on failure |
| `mutex_unlock(m)` | Release; wake one waiter |

---

## 4. Reader-Writer Lock

`zxfoundation/sync/rwlock.c` — allows multiple concurrent readers or one exclusive writer.

| Function | Description |
|----------|-------------|
| `rwlock_read_lock(rw)` | Acquire shared read access |
| `rwlock_read_unlock(rw)` | Release read access |
| `rwlock_write_lock(rw)` | Acquire exclusive write access |
| `rwlock_write_unlock(rw)` | Release write access |

---

## 5. Semaphore

`zxfoundation/sync/semaphore.c` — counting semaphore.

| Function | Description |
|----------|-------------|
| `sem_init(s, count)` | Initialize with initial count |
| `sem_wait(s)` | Decrement; sleep if count is 0 |
| `sem_post(s)` | Increment; wake one waiter |

---

## 6. Wait Queue

`zxfoundation/sync/waitqueue.c` — a list of sleeping tasks waiting for a condition.

| Function | Description |
|----------|-------------|
| `waitqueue_init(wq)` | Initialize |
| `waitqueue_wait(wq, condition)` | Sleep until `condition` is true |
| `waitqueue_wake_one(wq)` | Wake the first waiter |
| `waitqueue_wake_all(wq)` | Wake all waiters |

---

## 7. RCU

`zxfoundation/sync/rcu.c` — Read-Copy-Update. Currently a stub; `rcu_read_lock`/`rcu_read_unlock` are no-ops and `synchronize_rcu` returns immediately.
