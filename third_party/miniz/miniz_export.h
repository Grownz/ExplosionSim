#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

// Minimal static-build shim for the vendored miniz sources. The upstream CMake
// project generates this header with GenerateExportHeader; the project links
// miniz statically, so the export annotations are empty.
#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#define MINIZ_DEPRECATED

#endif // MINIZ_EXPORT_H
