# tiny_zip

Tiny, single-header ZIP archive library written in pure C99. Designed for high-performance and minimal binary footprint.

<p align="center">
  <img src="https://img.shields.io/badge/C-99-blue.svg">
  <img src="https://img.shields.io/badge/dependencies-none-success.svg">
  <img src="https://img.shields.io/badge/license-MIT-green.svg">
  <img src="https://img.shields.io/badge/platform-windows%2Fposix-lightgrey.svg">
</p>

A lightweight, robust ZIP writer focused on binary stability and seamless integration.

- **No external dependencies** (zlib not required)
- **Portable** (pure C99, standard library only)
- **Memory efficient** (no massive allocations)
- **Robust** (CRC-32 verification, strict standard compliance)
- **Tiny** (single-header file)

---

# Features

- **Minimalist API:** Create archives and add files in just a few lines of code.
- **Buffer Support:** Add data directly from RAM without temporary file overhead.
- **File Streaming:** Optimized interface for adding existing files from disk.
- **Portable Architecture:** Explicit Little-Endian writing for cross-platform reliability.
- **Embedded-ready:** Zero hidden dynamic allocations in the critical path.

---

# Installation & Usage

Copy `tiny_zip.h` into your project and define `ZIP_IMPLEMENTATION` in exactly one source file before including it.

```c
#define ZIP_IMPLEMENTATION
#include "tiny_zip.h"

int main(void)
{
    // 1. Create the archive
    zip_archive_t *zip = zip_create("output.zip");
    if (!zip) return 1;

    // 2. Add files from disk
    zip_add_file(zip, "data.txt", "data.txt", 0);

    // 3. Add data directly from memory buffers
    char *data = "Hello, world!";
    zip_add_buffer(zip, data, 13, "hello.txt");

    // 4. Finalize and clean up
    zip_finalize(zip);

    return 0;
}
