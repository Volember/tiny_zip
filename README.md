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
- **Dynamic Capacity:** Automatically resizes archive entries buffer.
- **Robust** (CRC-32 verification, strict standard compliance)
- **Tiny** (single-header file)

---

# Features

- **Minimalist API:** Create archives and add files in just a few lines.
- **Buffer & Disk Support:** Add data directly from RAM or stream from disk files.
- **Fileless In-Memory Creation:** Build archives entirely in-memory without touching physical storage, preventing SSD wear.
- **Auto-Scaling:** Safely handles adding an unlimited number of files via dynamic reallocations.
- **Portable Architecture:** Explicit Little-Endian writing for cross-platform reliability.

---

# Installation & Usage

Copy `tiny_zip.h` into your project and define `ZIP_IMPLEMENTATION` in exactly one source file before including it.

### File-Based Archive Creation

```c
#define ZIP_IMPLEMENTATION
#include "tiny_zip.h"

int main(void)
{
    // 1. Create the archive on disk
    zip_archive_t *zip = zip_create("output.zip");
    if (!zip) return 1;

    // 2. Add files from disk
    zip_add_file(zip, "test.txt", "data.txt", 0);

    // 3. Add data directly from memory
    char *data = "Hello, world!";
    zip_add_buffer(zip, data, 13, "hello.txt");

    // 4. Finalize and clean up
    zip_finalize(zip);

    return 0;
}
```

### In-Memory (Fileless) Archive Creation

```c
#define ZIP_IMPLEMENTATION
#include "tiny_zip.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // 1. Create the archive in memory
    zip_archive_t *zip = zip_create_mem();
    if (!zip) return 1;

    // 2. Add data directly from memory
    char *data = "Hello, world!";
    zip_add_buffer(zip, data, 13, "hello.txt");

    // 3. Finalize and get the in-memory ZIP payload
    size_t zip_size = 0;
    void *zip_buf = zip_finalize_mem(zip, &zip_size);
    if (!zip_buf) return 1;

    // Use zip_buf (e.g. transmit over network)...
    printf("In-memory ZIP created. Size: %zu bytes\n", zip_size);

    // 4. Free the finalized buffer
    free(zip_buf);

    return 0;
}
```
