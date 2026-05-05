# ZXVL Verification

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/init/zxfl/common/zxvl_verify.c`

---

## 1. Overview

ZXVL (ZXVerifiedLoad) is the integrity verification layer embedded in the ZXFL bootloader. It prevents arbitrary payloads from being loaded as the kernel nucleus. Three mechanisms are applied in sequence after ELF loading, before DAT is enabled.

---

## 2. Structural Lock

The kernel must embed a `.zxfl_lock` section at fixed offsets from its physical load base (`load_min`):

| Offset from `load_min` | Content |
|------------------------|---------|
| `0x70000` | High 32 bits of lock key: `0xCCBBCC35` |
| `0x70004` | Sentinel: `0x5A58464C` ("ZXFL") |
| `0x71000` | Low 32 bits of lock key: `0xE5664311` |

The loader verifies:

$$(\texttt{key} \oplus \texttt{ZXVL\_LOCK\_MASK}) = \texttt{ZXVL\_LOCK\_EXPECTED}$$

where:
- $\texttt{key} = (\texttt{hi} \ll 32) \mid \texttt{lo}$
- $\texttt{ZXVL\_LOCK\_MASK} = \texttt{0x3C1E0F8704B2D596}$
- $\texttt{ZXVL\_LOCK\_EXPECTED} = \texttt{0xF0A5C3B2E1D49687}$

A missing sentinel or wrong key causes an immediate panic — the loader refuses to execute the image.

---

## 3. Handshake

The kernel must place a callable function stub at `load_min + 0x0` (the very first byte of the loaded image). The stub must implement:

$$f(\texttt{nonce}) = \text{rotl}_{17}(\texttt{nonce}) + \texttt{ZXVL\_HS\_RESPONSE}$$

where $\text{rotl}_{17}(x) = (x \ll 17) \mid (x \gg 47)$ and $\texttt{ZXVL\_HS\_RESPONSE} = \texttt{0xDEADBEEF0BADF00D}$.

The loader calls the stub with:

$$\texttt{nonce} = \texttt{ZXVL\_SEED} \oplus \texttt{binding\_token}$$

$$\texttt{binding\_token} = \texttt{ZXVL\_SEED} \oplus \texttt{stfle\_fac[0]} \oplus \texttt{schid}$$

This ties the handshake to the specific hardware and IPL device. A kernel image that passes on one machine will not pass on another with different STFLE facilities or a different subchannel ID.

---

## 4. SHA-256 Segment Checksums

After the handshake, `zxvl_verify_nucleus_checksums` reads the `zxvl_checksum_table_t` from `load_min + 0x80000` and verifies each entry:

$$\text{SHA-256}(\texttt{phys\_start}, \texttt{size}) = \texttt{entry.digest}$$

Any mismatch causes an immediate panic. The table is patched into the kernel ELF by `gen_checksums` at build time. Any modification to a `PT_LOAD` segment after the build — including by a malicious bootloader or storage attack — is detected here.

---

## 5. Binding Token

The binding token is stored in `proto->binding_token` and used in two places:

1. **Handshake nonce** (above).
2. **Stack frame canary**: `frame[1] = ZXVL_FRAME_MAGIC_B ^ binding_token`.

The canary value is unique per hardware configuration. A canary extracted from one system cannot be replayed on another.

The kernel must recompute the binding token on entry and compare it to `proto->binding_token`. See [Boot Protocol §14](../bootloader/protocol.md#14-binding-token).
