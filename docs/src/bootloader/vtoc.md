# VTOC

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/init/zxfl/common/dasd_vtoc.c`

---

## 1. What Is the VTOC?

The Volume Table of Contents (VTOC) is the directory of a z/Architecture DASD volume. It is an IBM-defined on-disk structure that maps dataset names to their physical extents (cylinder/head ranges on ECKD, or block ranges on FBA).

The VTOC begins at a fixed location recorded in the DASD label (Format-4 DSCB at cylinder 0, head 0, record 3 on ECKD). ZXFL reads the VTOC to locate the kernel and loader datasets by name.

---

## 2. DSCB Types

The VTOC consists of Data Set Control Blocks (DSCBs), each 140 bytes. ZXFL uses two types:

| Type | Format | Purpose |
|------|--------|---------|
| Format-1 | F1DSCB | Dataset name, creation date, first extent |
| Format-3 | F3DSCB | Additional extents (overflow from F1) |
| Format-4 | F4DSCB | VTOC descriptor — location and size of VTOC itself |

---

## 3. Dataset Lookup

```
dasd_find_dataset(schid, name, &ext)
  │
  ├─ Read F4DSCB (C=0, H=0, R=3) → get VTOC start C/H and size
  ├─ For each DSCB in VTOC:
  │    ├─ Read record
  │    ├─ Check format byte
  │    ├─ If F1DSCB: compare DS1DSNAM (44-byte EBCDIC name) to target
  │    └─ If match: extract extent list from DS1EXT1..DS1EXT3
  └─ Return first extent (cylinder/head start + end)
```

Dataset names are stored in EBCDIC on disk. ZXFL converts the search name from ASCII to EBCDIC before comparison using `ebcdic_ascii_to_ebcdic()`.

---

## 4. Extent Structure

Each extent describes a contiguous range of tracks:

```
struct extent {
    uint16_t  cyl_start;   // starting cylinder
    uint16_t  head_start;  // starting head
    uint16_t  cyl_end;     // ending cylinder (inclusive)
    uint16_t  head_end;    // ending head (inclusive)
};
```

A dataset may span up to three extents in its F1DSCB, with additional extents in a chained F3DSCB. ZXFL follows the F3 chain if the dataset requires more than three extents.

---

## 5. Sequential Read

After locating a dataset's extents, `dasd_read_next()` reads tracks sequentially:

```
for each extent:
    for each track in [cyl_start/head_start .. cyl_end/head_end]:
        Seek → Search R=1 → Read all records on track → append to buffer
```

The read stops when the buffer is full or all extents are exhausted.
