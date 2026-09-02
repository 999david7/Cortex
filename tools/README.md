# Toolchain

This folder is where a portable MinGW-w64 compiler can live. It is **not**
committed to git — a full `w64devkit` tree is hundreds of megabytes.

## Setup

1. Download a release from [skeeto/w64devkit](https://github.com/skeeto/w64devkit/releases).
2. Extract it so this path exists:

   ```
   tools/w64devkit/bin/g++.exe
   ```

3. Run `build.bat` from the repo root.

Any other MinGW-w64 `g++` on `PATH` works too: skip the extract step and run
`make` directly.
