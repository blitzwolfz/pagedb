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
| 4092 | 4 | crc32 of bytes 0..4091 |

Unused bytes are zero.

The file is rejected on open when the magic is wrong, the checksum does not
match, the format version is not 1, the page size is not 4096, or the file is
shorter than the page count.
