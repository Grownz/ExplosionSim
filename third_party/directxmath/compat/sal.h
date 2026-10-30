#pragma once

// Minimal SAL annotation shim so Microsoft's DirectXMath headers compile with
// GCC and Clang outside Windows. Only the annotations DirectXMath uses are
// defined; they are compile-time hints and expand to nothing.

#define _In_
#define _In_reads_(n)
#define _In_reads_bytes_(n)
#define _Out_
#define _Out_opt_
#define _Out_writes_(n)
#define _Out_writes_bytes_(n)
#define _Success_(expr)
#define _Use_decl_annotations_
#define _Analysis_assume_(expr) ((void)0)
