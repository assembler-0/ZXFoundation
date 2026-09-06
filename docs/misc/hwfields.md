# Hardware Fields — Describing Registers, Records, and Memory-Mapped Structures

This guide documents the ZXFoundation hardware description DSL (`lib.bitfield` +
`lib.structure`). It is **architecture-agnostic**: it models the two shapes of
hardware-facing data that appear on every platform — *packed bit fields inside a
storage word*, and *byte-addressed aggregates over a memory region* — without
committing to any particular ISA, endianness, or bit-numbering convention.

> Language requirements: C++26 (modules, explicit object parameters / deducing
> `this`, structural NTTPs), freestanding, no exceptions, no RTTI. Error returns
> use `std::expected<T, lib::kernel_error>`; misuse is a `chk_assert` (UB-lite,
> always-on in debug, compiled out in release).

---

## 1. The mental model

Hardware data comes in two shapes, and the DSL has one layer for each:

| Layer | Domain | Addressable unit | Typical use |
|-------|--------|------------------|-------------|
| `record` | **bit** domain | a single storage integer (`u8`…`u32`/`u64`/`u128`) | CPU control/status words, packed flags, descriptor words |
| `structure` | **byte** domain | a byte range over memory | MMIO register files, page tables, bus/port frames, wire structs |

Both layers are declared with the **same** primitive — `field` — and differ only
by their *locator* (where the field lives) and the *attributes* attached to it.
This unification is the whole point: you learn one vocabulary and use it
everywhere.

Everything is **zero-overhead**: `record` is a single storage integer by value;
`structure_ref` is a view over an existing pointer; there are no vtables, no
runtime maps, no per-field objects. Field paths, offsets, and endian/shift
transformations are all resolved at compile time.

---

## 2. The `field` primitive

```cpp
template<lib::fixed_string Name, class Locator, class Type = void, class... Attr>
lib::field;
```

| Parameter | Meaning |
|-----------|---------|
| `Name`    | compile-time name, used for `get`/`set` path lookup |
| `Locator` | **where** the field lives (see §4) |
| `Type`    | the C++ value type of the field (defaults to `void`, i.e. "infer") |
| `Attr...` | access class, transformations, and constraints (§3) |

You rarely write `field` directly; instead you use the prefab aliases (§3.5),
which fix the access class and leave the locator/type generic:

```cpp
lib::rw<"enabled", lib::bits<3>>          // read/write bit 3
lib::ro<"slot",   lib::bits<4, 7>, u8>    // read-only 4-bit field, typed u8
```

A `field` on its own is a **description**, not an object. It becomes live only
when placed inside a `record` or `structure`, which provides storage and the
`get`/`set` machinery.

---

## 3. Attributes

Attributes are the closed set of type tags accepted in `field`'s variadic pack.
The first access-class attribute you supply selects the field's semantics.

### 3.1 Access classes

| Tag | Read | Write | Purpose |
|-----|------|-------|---------|
| `rw`  | value       | store value              | normal field |
| `ro`  | value       | *forbidden* (compile err) | read-only (status/identifier) |
| `wo`  | *forbidden* | store value              | write-only (command latches) |
| `w1c` | value       | clear bits written `1`   | write-1-to-clear (interrupt/status) |
| `w1s` | value       | set bits written `1`     | write-1-to-set |
| `w1t` | value       | toggle bits written `1`  | write-1-to-toggle |
| `w0c` | value       | clear bits written `0`   | write-0-to-clear |
| `rc`  | value, then clears | store value     | read-to-clear (error logs / counters) |
| `rz`  | `0`         | must be `0` (assert)      | reserved-zero |
| `raz` | `0`         | ignored                   | read-as-zero |
| `rao` | all-ones    | ignored                   | read-as-one |
| `wac` | value       | always clears             | write-any-to-clear |
| `woo` | value       | only while latched `0` (assert) | write-once-only |

Notes:

* `ro`/`wo` are enforced with `static_assert` (compile-time), so a
  read-only field on a non-const view simply does not compile.
* `w1c`/`w1s`/`w1t`/`w0c`/`wac`/`woo` perform a **read-modify-write** against the
  current hardware value; this is transparent for MMIO and constant-evaluated
  access.
* `rc` requires a non-const view (it mutates the source on read).

### 3.2 Transformations

Transformations change how the stored raw bits map to the logical C++ value.
They are pure, compile-time, and invertible.

| Tag | `from_bits(raw)` | `to_bits(val)` |
|-----|------------------|----------------|
| `aligned<N>` | `raw << log2(N)` | `val >> log2(N)` (asserts alignment) |
| `shifted_by<S>` | `raw << S` | `val >> S` (asserts low `S` bits are 0) |
| `inverted` | `~raw` (masked) | `~val` (masked) |

`aligned<N>`/`shifted_by<S>` are how you describe logically-byte-granular
registers stored in a denser bit stream (e.g. an alignment field encoded without
its trailing zeros). `inverted` models active-low signals.

`aligned_address<A>` / `shifted<S>` / `inverted` are the underlying transformers;
a custom transformer must expose `from_bits<Width>`/`to_bits<Width>` and
`select_transform<...>` picks it out of the attribute pack.

### 3.3 Endianness, granularity, layout

| Tag | Meaning |
|-----|---------|
| `endian_big` / `endian_little` | per-field endian override (fields default to the enclosing structure's endian) |
| `unaligned` | this field may sit at a non-naturally-aligned byte offset (byte-wise access) |
| `word<W>` | bus granularity. `W ∈ {u8,u16,u32,u64}`; the field is read/written as part of a wider `W` bus word |
| `overlay` | this field intentionally aliases the bytes of another field (disables overlap detection) |

### 3.4 Reset semantics

| Tag | Meaning |
|-----|---------|
| `init<V>` | default/reset value applied to `*_defaults()` / default construction |
| `fixed<V>` | architectural constant: reads return `V`, writes must equal `V` (assert) |

`fixed` is for fields that are architecturally pinned to a value (as opposed to
`ro`, which is "hardware wrote it, you may read it").

### 3.5 Prefab aliases

```cpp
lib::rw  <Name, Loc, T = void, Extra...>    // and ro, wo
lib::w1c <Name, Loc, T = void, Extra...>    // and w1s, w1t, w0c
lib::rc  <Name, Loc, T = void, Extra...>    // and rz, raz, rao, wac, woo

lib::overlay_rw <Name, Loc, T = void, Extra...>   // +ro, +wo
lib::aligned_rw <Name, Loc, Aln, Extra...>          // typed u64, +ro, +wo

lib::bits <A, B = A>     // == bit_slice<A,B>
lib::bytes<S, E>         // == byte_span<S,E>
```

`Extra...` lets you append `attr::word<...>`, `attr::unaligned`, `attr::init<...>`,
`attr::fixed<...>`, `endian_big`, a transformer, etc. to any prefab.

Compatibility shims (`rw_field`, `rw_byte`, `cross_*`, `bank_array`,
`*_byte_auto`, …) still exist and simply forward to the canonical aliases — new
code should use `lib::rw<...>`/`lib::bytes<...>`/etc.

---

## 4. Locators — the two domains

A locator is a tag type that tells the DSL where a field's bits live.

### 4.1 Bit domain (inside a `record`)

| Locator | Meaning |
|---------|---------|
| `bit_slice<A, B>` / `bits<A,B>` | contiguous bit range `A..B` within the record's storage word |
| `std::tuple<bs...>` | **disjoint/cross** bit ranges (scattered bits spliced into one value) |

Bit positions are interpreted by the record's **numbering** policy (§5.2):
`msb0_t` (bit `0` = most significant) or `lsb0_t` (bit `0` = least significant).

### 4.2 Byte domain (inside a `structure`)

| Locator | Meaning |
|---------|---------|
| `byte_span<S,E>` / `bytes<S,E>` | contiguous whole-byte range `S..E` (pinned) |
| `slice<Off, A, B, Num>` | a bit window `A..B` within the byte at offset `Off` (pinned) |
| `bank<Count, Stride>` | strided array: `Count` elements every `Stride` bytes |
| `auto_layout` | automatic layout: place me at the next free, aligned offset |
| `std::tuple<slices...>` | **cross**: scattered bit windows concatenated into one value |
| `std::tuple<byte_spans...>` | **disjoint**: scattered byte ranges concatenated into one value |

Hierarchical anchors let you build reusable offsets:

```cpp
lib::at_byte<N> / at_halfword<N> / at_word<N> / at_dword<N> / at_qword<N>
    .at_byte<K> / .at_bit<B> / .at_bits<A,B>
```

`at_word<2>` is `at_byte<8>`, etc.; anchors compose absolute offsets.

### 4.3 Field classification (structure domain)

A structure field is *exactly one* of:

* **pinned** — explicit `byte_span`/`slice` offset; must respect natural alignment
  unless `unaligned`.
* **cross** — concatenated bit windows across discontiguous bytes.
* **disjoint** — concatenated byte spans forming one larger value.
* **bank** — a strided, homogeneous array.
* **auto** — `auto_layout`, placed by the compiler with correct padding.

Pinned/cross/disjoint/bank are all implicitly *overlay-capable*; a plain
non-overlay overlap between **auto-laid** fields is a hard error, while
`overlay`/pinned aliases are explicit and allowed.

---

## 5. Records (`lib.bitfield`)

A `record` packs named fields into a single storage integer.

```cpp
template<lib::bit_storage T, class Numbering, class... FieldOrExts>
lib::record;

// views over existing storage:
lib::record_ref<T, Numbering, FieldOrExts...>      // borrowed, non-owning
lib::mmio_record_ref<T, Numbering, FieldOrExts...> // borrowed over volatile MMIO
```

### 5.1 Example

```cpp
using status_word = lib::record<u32, lib::msb0_t,
    lib::rw <"busy",       lib::bits<0>>,
    lib::ro <"level",      lib::bits<1, 3>, u8>,
    lib::w1c<"error",      lib::bits<4, 7>, u8>,
    lib::rw <"mode",       lib::bits<8, 9>, mode_t>,   // enum
    lib::rw <"!crc",       std::tuple<lib::bit_span<10,10>, lib::bit_span<15,15>>, u16>
>;

status_word s{0};
s.set<"busy">(true);
auto lvl = s.get<"level">();
```

Enums, integrals, `bool`, `lib::exact_uint<Bits>`, and nested `record`s are all
valid field value types. `Type = void` means "infer the value type from the bit
width" (an unsigned integer of minimal width).

### 5.2 Numbering

`lib::msb0_t` (alias `lib::msb0`) numbers bit `0` as the most-significant bit;
`lib::lsb0_t` (alias `lib::lsb0`) numbers bit `0` as the least-significant bit.
Pick whichever your register documentation uses.

### 5.3 Construction

```cpp
status_word a{};                      // zero-filled, then applies init<>/fixed<> defaults
status_word b(0x000F);                // from raw storage
status_word c{"busy"_f = true, "mode"_f = mode_t::x};   // field bindings
auto d = status_word::make<"busy", "mode">(true, mode_t::x);
```

`"_f"` produces a `field_tag`, whose `operator=` wraps a value into a
`field_binding`; the binding list is the DSL's counterpart to C's designated
initializers.

### 5.4 Access

```cpp
r.get<"name">()        r.set<"name">(v)      // by name (fixed_string)
r.get<0>()             r.set<0>(v)           // by index
r["name"_f]                                   // field_tag -> field_binding (pending set)
r.underlying()                                // raw storage integer
r.bits()                                      // bit_field<T,Numbering> (bit-level operator[])
r.ref()  /  r.mmio_ref()                      // record_ref / mmio_record_ref views
r.field_count,  r.storage_type,  r.numbering
```

Nested records use `\` in the path:

```cpp
outer.get<R"(inner\flag)">();   // reads inner.flag; also settable
```

**Compile-time overlap detection** verifies distinct (non-overlay) fields never
share bits. `record_ref` is a non-owning view over caller storage; use
`mmio_record_ref` for volatile/atomic access (`atomic_load`, `atomic_store`,
`atomic_bit_set`, `atomic_bit_clear`, `atomic_bit_toggle`).

---

## 6. Structures (`lib.structure`)

A `structure` is the byte-domain counterpart: fields laid out across a byte range.

```cpp
template<usize Extent, usize Alignment = 8, class StructEndian = lib::endian::native, class... FieldOrExts>
lib::structure;

lib::structure_ref<...>          // non-owning view over existing memory
lib::mmio_structure_ref<...>     // non-owning view over volatile MMIO
lib::structure_span<StructT, IsMMIO = false>  // array of structures
```

### 6.1 Example

```cpp
using pkt = lib::structure<512, 8, lib::endian::big,
    lib::rw<"magic",        lib::bytes<0, 3>, u32>,
    lib::ro<"version",      lib::bytes<4, 4>, u8>,
    lib::rw<"flags",        lib::bytes<5, 5>, lib::record<u8, lib::msb0_t,
        lib::rw<"urgent", lib::bits<0>>,
        lib::rw<"ack",    lib::bits<1>>
    >>,
    lib::rw<"checksum",     lib::auto_layout, u16>,
    lib::rw<"entries",      lib::bank<64, 8>, u64>          // 64 u64 entries, stride 8
>;

pkt p{};
p.set<"magic">(0xDEADBEEFu);
```

`endian::big`/`endian::little`/`endian::native` control byte swapping on
multibyte reads/writes; per-field `endian_big`/`endian_little` override it.
`struct_endian` is applied automatically, so a big-endian structure reads
correctly even on a little-endian host.

### 6.2 Construction & views

```cpp
pkt p;                            // zero-extent storage, applies init<>/fixed<>
pkt q{"magic"_f = 0xDEADBEEFu};   // field bindings
auto s = pkt::make<"magic">(0xDEADBEEFu);

pkt::ref_type  ref = p.ref();          // structure_ref over p.raw_ptr()
auto mref = p.mmio_ref();              // mmio_structure_ref over same memory

// borrow caller memory:
structure_ref<...> v(existing_ptr);
mmio_structure_ref<...> m(volatile_ptr);

// array of identical structures:
lib::structure_span<pkt> span(base_ptr, count);
span[3].set<"magic">(0x1234);          // or .try_at(3)
```

Non-owning refs are ideal for pointing at hardware, mapped regions, or packed
buffers with zero copying.

### 6.3 Access

```cpp
s.get<"name">()      s.set<"name">(v)    // path-based; `\` drills into nested records/structs
s["name"_f]                               // field_tag -> field_binding
s.as_bytes()                              // std::span<const u8, extent>
s.raw_read<T>(off)  s.raw_write<T>(off, v)
s.try_raw_read<T>(off) -> std::expected<T, kernel_error>
s.try_raw_write<T>(off, v)
s.offset_of<"name">()   s.size_of<"name">()   // consteval
s.apply_default_values()
s.field_count, s.layout_meta, s.extent, s.alignment
```

`mmio_structure_ref` additionally provides `atomic_load<"name">`,
`atomic_store<"name">`, `atomic_compare_exchange<"name">` for naturally-aligned,
trivially-copyable fields.

### 6.4 Layout guarantees (compile time)

* `Extent` is validated: the computed layout must not exceed it.
* Overlaps between distinct auto-laid fields are **rejected** unless marked
  `overlay` (or pinned/cross/disjoint).
* Pinned fields must respect natural alignment unless `attr::unaligned`.
* Cross fields must fit their declared `Type` in bits; disjoint fields must match
  `sizeof(Type)` exactly.

These are `static_assert`s — layout bugs do not survive compilation.

---

## 7. `attrs` cheat-sheet

```cpp
// access
rw ro wo w1c w1s w1t w0c rc rz raz rao wac woo
// transforms
aligned<N>  shifted_by<S>  inverted
// layout / storage
unaligned  word<W>  overlay  endian_big  endian_little
// reset
init<V>  fixed<V>
// mixins
extensions<M...>      // M = template<class Derived> struct ...
```

---

## 8. `extensions` (hardware mixins)

A record/structure may carry methods *derived from* its own type via deducing-`this`
mixins, so that common register logic lives at the field-definition site:

```cpp
template<class Self>
struct has_reset_bits {
    bool ready(this const Self& self) { return !self.template get<"busy">(); }
};

using status = lib::record<u32, lib::msb0_t,
    lib::extensions<has_reset_bits>,
    lib::rw<"busy", lib::bits<0>>
>;
```

`extensions` is excluded from field filtering automatically and adds its
mixin base(s) to the type. The mixin receives the concrete record/structure
type and can call `get`/`set` on it.

---

## 9. Use cases

This DSL is deliberately generic. Concrete patterns it expresses well:

1. **CPU control/status words** — a `record` per architectural register, with
   `init<>` reset values and `extensions<>` for derived queries.
2. **Interrupt/status registers** — `w1c`/`w0c`/`rc` fields give correct
   read-modify-write/ack semantics for free.
3. **MMIO register files** — `mmio_structure_ref` over a mapped pointer; `word<>`
   bus granularity, endian conversion, and `atomic_*` for shared/peripheral use.
4. **Strided peripheral arrays** — `bank<Count, Stride>` for registers that repeat
   every `Stride` bytes; `structure_span` for arrays of whole structures.
5. **Packed protocol / wire formats** — `record` + big/little endian + `slice`
   bit windows over on-the-wire bytes.
6. **Configuration blocks** — `structure` with `init<>`/`fixed<>` defaults that
   materialize a fully-initialized image via `*_defaults()`.
7. **Cross-field/split encodings** — `std::tuple<...>` locators for fields whose
   bits (or bytes) are deliberately scattered.

---

## 10. Design rules

* **Path names are structural** (`fixed_string` NTTP). `get`/`set` resolve at
  compile time; unknown names are `static_assert` failures.
* **No runtime overhead**: read a field and you get the same machine code as a
  hand-written shift/mask or typed memory access; refs are just pointers.
* **Constant-evaluation friendly**: field access works in `consteval`/`constexpr`
  contexts (bit-casting through byte arrays rather than alias-violating loads).
* **Errors**, when recoverable, surface as `std::expected<_, kernel_error>`; when
  they reflect a programming mistake, they are `chk_assert`/`static_assert`.
* **Access classes are primary**: choose `rw`/`ro`/`w1c`/… first; everything else
  (layout, endian) composes on top.

---

## 11. Minimal reference

```cpp
// bit domain
lib::record<T, Numbering, Fields...>                 // owning integer
lib::record_ref<T, Numbering, Fields...>             // borrowed view
lib::mmio_record_ref<T, Numbering, Fields...>        // volatile view
lib::bits<A, B = A>                                   // bit_slice

// byte domain
lib::structure<Extent, Align = 8, Endian = native, Fields...>
lib::structure_ref<...> / lib::mmio_structure_ref<...>
lib::structure_span<StructT, IsMMIO = false>
lib::bytes<S, E> / lib::bank<Count, Stride> / lib::auto_layout
lib::at_byte<N> / at_halfword / at_word / at_dword / at_qword

// value helpers
lib::exact_uint<Bits>
lib::field_type_fits
lib::record_value_type (concept)

// prefabs
lib::rw/ro/wo/w1c/w1s/w1t/w0c/rc/rz/raz/rao/wac/woo
lib::overlay_rw/ro/wo   lib::aligned_rw/ro/wo
```

Modules to import: `lib.bitfield` (records) and `lib.structure` (structures),
both re-exported by the umbrella `lib` module.