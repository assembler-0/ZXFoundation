# bin2rec

**Document Revision:** 26h1.0  
**Source:** `tools/bin2rec.c`

---

## 1. Purpose

`bin2rec` converts a flat binary into the DASD IPL record format required by the Hercules `dasdload` utility and the z/Architecture channel subsystem.

```sh
bin2rec <input.bin> <output.sys>
```

---

## 2. Background

The z/Architecture IPL mechanism reads the first physical record from the IPL device and loads it into memory at address `0x0`. The record must be in a specific format: each 80-byte card image contains a header identifying it as a text record (`TXT`) or end record (`END`), a load address, a byte count, and 56 bytes of data.

This format originates from the IBM card-punch era — the DASD IPL record format is a direct descendant of the punched-card object deck format.

---

## 3. Record Format

Each 80-byte record:

| Bytes | Content |
|-------|---------|
| 0 | `0x02` (record type marker) |
| 1–3 | `TXT` in EBCDIC (`0xE3 0xE7 0xE3`) or `END` (`0xC5 0xD5 0xC4`) |
| 4 | `0x00` |
| 5–7 | Load address (24-bit, big-endian) |
| 8–9 | `0x00 0x00` |
| 10–11 | Byte count (`0x0038` = 56, big-endian) |
| 12–15 | `0x00 0x00 0x00 0x00` |
| 16–71 | 56 bytes of binary data |
| 72–79 | `0x00` × 8 |

The tool reads 56 bytes at a time from the input binary, wraps each chunk in a `TXT` record, and writes an `END` record at the end.

---

## 4. Limitations

- Maximum input size: 32 KB (`MAX_REC_SIZE = 32768`). Stage 0 is well within this at ~5 KB.
- Load address is 24-bit — intentional. The IPL PSW is a 31-bit ESA/390 PSW; the channel subsystem loads the record into the low 16 MB.
