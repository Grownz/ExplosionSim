# miniz

Vendored from [miniz 3.0.2](https://github.com/richgel999/miniz) (MIT, see
`LICENSE`). Only the sources needed by the standalone update helper are kept.
`miniz_export.h` is a minimal static-build shim; the upstream project generates
it with CMake's `GenerateExportHeader`.
