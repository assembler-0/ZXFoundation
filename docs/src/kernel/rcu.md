# RCU and SRCU

**Document Revision:** 26h1.1  
**Source:** `zxfoundation/sync/rcu.c`, `zxfoundation/sync/srcu.c`

---

## 1. RCU

Read-Copy-Update for a non-preemptive kernel. A quiescent state (QS) occurs whenever a CPU is not inside an `rcu_read_lock()` section.

### Read Side

| Function | Description |
|---|---|
| `rcu_read_lock()` | Enter read-side critical section (compiler barrier only) |
| `rcu_read_unlock()` | Exit read-side critical section |
| `rcu_dereference(p)` | Safely read an RCU-protected pointer |
| `rcu_assign_pointer(p, v)` | Safely publish a new pointer |

### Write Side

| Function | Description |
|---|---|
| `call_rcu(head, fn)` | Register a callback for after the next grace period |
| `synchronize_rcu()` | Block until all pre-existing readers have completed, then drain callbacks |
| `rcu_report_qs()` | Report a quiescent state for the current CPU |

### Grace Period Mechanism

```
synchronize_rcu():
  1. Increment gp_seq
  2. Broadcast new gp_seq to all per-CPU rcu_gp_seq fields
  3. Spin until every CPU's rcu_qs_seq == gp_seq
  4. Drain callback list
```

`rcu_report_qs()` must be called from the idle loop and any long-running non-read-side context.

---

## 2. SRCU

Sleepable RCU — allows read-side critical sections to sleep. Each SRCU domain (`srcu_struct_t`) is independent.

### Read Side

| Function | Description |
|---|---|
| `srcu_read_lock(s)` | Enter SRCU read section; returns slot index |
| `srcu_read_unlock(s, idx)` | Exit SRCU read section |

### Write Side

| Function | Description |
|---|---|
| `synchronize_srcu(s)` | Wait for all pre-existing readers; may spin |
| `call_srcu(s, head, fn)` | Synchronize then invoke callback |

### Two-Slot Mechanism

```
Active slot: s->idx (0 or 1)

srcu_read_lock:   increment pcpu[cpu].c[s->idx]
srcu_read_unlock: decrement pcpu[cpu].c[idx]

synchronize_srcu:
  1. Flip s->idx (new readers use new slot)
  2. Wait until sum of pcpu[*].c[old_idx] == 0
  3. Increment gp_seq
```

### Initialization

```c
DEFINE_SRCU(my_domain);          // static
srcu_init(&my_domain);           // runtime
```
