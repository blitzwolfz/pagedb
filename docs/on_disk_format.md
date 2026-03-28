# On disk format

One database is one file. The file is a list of pages. Every page is 4096
bytes. The page id is the index of the page, so the byte offset of a page is
`page_id * 4096`. Page 0 is the meta page. All numbers are little endian and
are written field by field, never as a C++ struct.

## Meta page (page 0)

| offset | size | field |
|---|---|---|
| 0 | 8 | magic `PAGEDB01` |
| 8 | 4 | format version |
| 12 | 4 | page size |
| 16 | 4 | root page id of the tree, 0 if the tree is empty |
| 20 | 4 | number of pages in the file |
| 24 | 4 | first page of the free list, 0 if empty |
| 28 | 4 | number of pages in the free list |
| 4092 | 4 | crc32 of bytes 0..4091 |

Unused bytes are zero.

The file is rejected on open when the magic is wrong, the checksum does not
match, the format version is not 1, the page size is not 4096, or the file is
shorter than the page count.

## Node page

Leaf and internal nodes of the B+ tree use the same header.

| offset | size | field |
|---|---|---|
| 0 | 1 | page type: 1 meta, 2 internal, 3 leaf, 4 free |
| 1 | 1 | unused |
| 2 | 2 | number of cells |
| 4 | 2 | start of the cell area |
| 6 | 2 | bytes lost by removed cells |
| 8 | 4 | internal: right most child. leaf: next leaf id, 0 at the end |
| 12 | 8 | page lsn |
| 20 | 4 | unused |

After the header comes one 2 byte slot per cell. A slot is the offset of the
cell inside the page. Slots are sorted by key. Cells are placed from the end
of the page downwards, so the free space sits between the slots and the cells.

Leaf cell: key length (2), value length (2), key bytes, value bytes.

Internal cell: key length (2), child page id (4), key bytes.

An internal node with n cells has n + 1 children. The child of cell i holds
keys smaller than key i. The right most child in the header holds keys that
are greater or equal to the last key.

## Free page

A page on the free list has page type 4 and the id of the next free page at
offset 8. The meta page points at the first free page.
