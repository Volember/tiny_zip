# tiny_zip

Tiny single-header ZIP archive library written in pure C99.

<p align="center">
  <img src="https://img.shields.io/badge/C-99-blue.svg">
  <img src="https://img.shields.io/badge/dependencies-none-success.svg">
  <img src="https://img.shields.io/badge/license-MIT-green.svg">
  <img src="https://img.shields.io/badge/platform-cross--platform-lightgrey.svg">
</p>

A lightweight ZIP writer with built-in:

- CRC-32
- LZ77 compression
- Fixed Huffman Deflate
- ZIP archive generation

No external dependencies.  
No zlib.  
Pure portable C99.

---

# Features

- Single-header library
- Tiny and embeddable
- Pure C99
- Cross-platform
- No dynamic allocations inside compression loops
- Fixed Huffman Deflate implementation
- Supports directories
- Custom archive filenames
- Minimal API

---

# Installation & Usage

Just copy `tiny_zip.h` into your project.

```c
#define NZ_IMPLEMENTATION
#include "tiny_zip.h"

int main(void)
{
    nz_zip zip;

    nz_zip_create(&zip, "assets.zip");

    nz_zip_add_file(&zip, "image.png");
    nz_zip_add_file(&zip, "config.json");
    nz_zip_add_directory(&zip, "data");

    nz_zip_close(&zip);

    return 0;
}
```
---

# Contributing

Issues and pull requests are welcome.
