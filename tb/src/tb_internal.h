#pragma once

// Windows likes it's secure functions, i kinda do too
// but only sometimes and this isn't one of them
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "tb.h"
#include "tb_formats.h"

#include <limits.h>
#include <time.h>
#include <stdalign.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <immintrin.h>
#define thread_local __declspec(thread)
#define alignas(x) __declspec(align(x))
#else
#define thread_local _Thread_local
#endif

#ifndef _WIN32
// NOTE(NeGate): I love how we assume that if it's not windows
// its just posix, these are the only options i guess
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "tb_platform.h"
#include "bigint/BigInt.h"
#include "dyn_array.h"
#include "builtins.h"
#include "pool.h"

#define NL_HASH_MAP_INLINE
#include <hash_map.h>

#include <hash_set.h>
#include <perf.h>

#define FOREACH_N(it, start, end) \
for (ptrdiff_t it = (start), end__ = (end); it < end__; ++it)

#define FOREACH_REVERSE_N(it, start, end) \
for (ptrdiff_t it = (end), start__ = (start); (it--) > start__;)

#include <arena.h>
#include "set.h"

#include <threads.h>
#include <stdatomic.h>

// ***********************************
// Constraints
// ***********************************
// TODO: get rid of all these
#ifndef TB_MAX_THREADS
#define TB_MAX_THREADS 64
#endif

// Per-thread
#ifndef TB_TEMPORARY_STORAGE_SIZE
#define TB_TEMPORARY_STORAGE_SIZE (1 << 20)
#endif

// ***********************************
// Atomics
// ***********************************
// We use C11 atomics, fuck you if you can't. we don't serve Microsofts.
int tb_atomic_int_load(int* dst);
int tb_atomic_int_add(int* dst, int src);
int tb_atomic_int_store(int* dst, int src);
bool tb_atomic_int_cmpxchg(int* address, int old_value, int new_value);

size_t tb_atomic_size_load(size_t* dst);
size_t tb_atomic_size_add(size_t* dst, size_t src);
size_t tb_atomic_size_sub(size_t* dst, size_t src);
size_t tb_atomic_size_store(size_t* dst, size_t src);

#define CODE_REGION_BUFFER_SIZE (128 * 1024 * 1024)

typedef struct TB_Emitter {
    size_t capacity, count;
    uint8_t* data;
} TB_Emitter;

#define TB_DATA_TYPE_EQUALS(a, b) ((a).raw == (b).raw)

#undef TB_FOR_BASIC_BLOCK
#define TB_FOR_BASIC_BLOCK(it, f) for (TB_Label it = 0; it < f->bb_count; it++)

#undef TB_FOR_FUNCTIONS
#define TB_FOR_FUNCTIONS(it, m) for (TB_Function* it = (TB_Function*) m->first_symbol_of_tag[TB_SYMBOL_FUNCTION]; it != NULL; it = (TB_Function*) it->super.next)

#undef TB_FOR_GLOBALS
#define TB_FOR_GLOBALS(it, m) for (TB_Global* it = (TB_Global*) m->first_symbol_of_tag[TB_SYMBOL_GLOBAL]; it != NULL; it = (TB_Global*) it->super.next)

#undef TB_FOR_EXTERNALS
#define TB_FOR_EXTERNALS(it, m) for (TB_External* it = (TB_External*) m->first_symbol_of_tag[TB_SYMBOL_EXTERNAL]; it != NULL; it = (TB_External*) it->super.next)

// i love my linked lists don't i?
typedef struct TB_SymbolPatch TB_SymbolPatch;
struct TB_SymbolPatch {
    TB_SymbolPatch* prev;

    TB_Function* source;
    uint32_t pos;  // relative to the start of the function body
    bool internal; // handled already by the code gen's emit_call_patches
    const TB_Symbol* target;
};

typedef struct TB_File {
    char* path;
} TB_File;

struct TB_External {
    TB_Symbol super;
    TB_ExternalType type;

    void* thunk; // JIT will cache a thunk here because it's helpful
};

typedef struct TB_InitObj {
    enum {
        TB_INIT_OBJ_REGION,
        TB_INIT_OBJ_RELOC,
    } type;
    TB_CharUnits offset;
    union {
        struct {
            TB_CharUnits size;
            const void* ptr;
        } region;

        const TB_Symbol* reloc;
    };
} TB_InitObj;

struct TB_Global {
    TB_Symbol super;
    TB_Linkage linkage;

    TB_ModuleSection* parent;

    // layout stuff
    void* address; // JIT-only
    uint32_t pos;
    TB_CharUnits size, align;

    // debug info
    TB_DebugType* dbg_type;

    // contents
    uint32_t obj_count, obj_capacity;
    TB_InitObj* objects;
};

struct TB_DebugType {
    enum {
        TB_DEBUG_TYPE_VOID,
        TB_DEBUG_TYPE_BOOL,

        TB_DEBUG_TYPE_UINT,
        TB_DEBUG_TYPE_INT,
        TB_DEBUG_TYPE_FLOAT,

        TB_DEBUG_TYPE_ARRAY,
        TB_DEBUG_TYPE_POINTER,

        // special types
        TB_DEBUG_TYPE_ALIAS,
        TB_DEBUG_TYPE_FIELD,

        // aggregates
        // TODO(NeGate): apparently codeview has cool vector and matrix types... yea
        TB_DEBUG_TYPE_STRUCT,
        TB_DEBUG_TYPE_UNION,

        TB_DEBUG_TYPE_FUNCTION,
    } tag;

    // debug-info target specific data
    union {
        struct {
            uint16_t cv_type_id;
            uint16_t cv_type_id_fwd; // used by records to manage forward decls
        };
    };

    // tag specific
    union {
        int int_bits;
        TB_FloatFormat float_fmt;
        TB_DebugType* ptr_to;
        struct {
            TB_DebugType* base;
            size_t count;
        } array;
        struct {
            char* name;
            TB_DebugType* type;
        } alias;
        struct {
            char* name;
            TB_CharUnits offset;
            TB_DebugType* type;
        } field;
        struct TB_DebugTypeRecord {
            char* tag;
            TB_CharUnits size, align;

            size_t count;
            TB_DebugType** members;
        } record;
        struct TB_DebugTypeFunc {
            TB_CallingConv cc;
            bool has_varargs;

            size_t param_count, return_count;
            TB_DebugType** params;
            TB_DebugType** returns;
        } func;
    };
};

typedef struct TB_Line {
    TB_FileID file;
    int line;
    uint32_t pos;
} TB_Line;

typedef enum {
    TB_ATTRIB_NONE,
    TB_ATTRIB_VARIABLE,
    TB_ATTRIB_LOCATION,
} TB_AttribType;

struct TB_Attrib {
    TB_Attrib* next;
    TB_AttribType type;

    union {
        struct {
            TB_Attrib* parent;
        } scope;
        struct {
            char* name;
            TB_DebugType* storage;
        } var;
        struct {
            TB_FileID file;
            int line;
        } loc;
    };
};

typedef struct TB_StackSlot {
    // TODO(NeGate): support complex variable descriptions
    // currently we only support stack relative
    int32_t position;
    const char* name;
    TB_DebugType* storage_type;
} TB_StackSlot;

typedef struct TB_Comdat {
    TB_ComdatType type;
    uint32_t reloc_count;
} TB_Comdat;

typedef struct {
    uint32_t ip; // relative to the function body.
    TB_Safepoint* sp;
} TB_SafepointKey;

typedef struct TB_CodeRegion TB_CodeRegion;
struct TB_CodeRegion {
    size_t capacity, size;
    TB_CodeRegion* prev;

    uint8_t data[];
};

typedef struct TB_FunctionOutput {
    TB_Function* parent;
    TB_Linkage linkage;

    uint8_t prologue_length;
    uint8_t epilogue_length;

    // NOTE(NeGate): This data is actually specific to the
    // architecture run but generically can be thought of as
    // 64bits which keep track of which registers to save.
    uint64_t prologue_epilogue_metadata;
    uint64_t stack_usage;

    TB_CodeRegion* code_region;
    uint8_t* code;

    size_t code_pos; // relative to the export-specific text section
    size_t code_size;

    // export-specific
    uint32_t unwind_info;
    uint32_t unwind_size;

    // windows COMDAT specific
    uint32_t comdat_id;

    DynArray(TB_StackSlot) stack_slots;

    // Part of the debug info
    DynArray(TB_Line) lines;

    // safepoints are stored into a binary tree to allow
    // for scanning neighbors really quickly
    TB_SafepointKey* safepoints;

    // Relocations
    uint32_t patch_pos;
    uint32_t patch_count;
    TB_SymbolPatch* last_patch;
} TB_FunctionOutput;

struct TB_Function {
    TB_Symbol super;
    TB_Linkage linkage;

    TB_DebugType* dbg_type;
    TB_FunctionPrototype* prototype;
    TB_Comdat comdat;

    TB_Node* start_node;
    TB_Node* active_control_node;

    size_t safepoint_count;
    size_t control_node_count;
    size_t node_count;

    // IR allocation
    TB_Arena* arena;

    // IR building
    TB_Attrib* line_attrib;

    // Compilation output
    union {
        void* compiled_pos;
        size_t compiled_symbol_id;
    };

    TB_FunctionOutput* output;
};

typedef enum {
    // stores globals
    TB_MODULE_SECTION_DATA,

    // data but it's thread local
    TB_MODULE_SECTION_TLS,

    // holds all the code (no globals)
    TB_MODULE_SECTION_TEXT,
} TB_ModuleSectionKind;

struct TB_ModuleSection {
    char* name;
    TB_LinkerSectionPiece* piece;

    int section_num;
    TB_ModuleSectionKind kind;

    // export-specific
    uint32_t flags;
    uint32_t name_pos;

    // this isn't computed until export time
    uint32_t raw_data_pos;
    uint32_t total_size;
    uint32_t reloc_count;
    uint32_t reloc_pos;

    uint32_t total_comdat_relocs;
    uint32_t total_comdat;

    bool laid_out;

    // this is all the globals within the section
    DynArray(TB_Global*) globals;
};

struct TB_Module {
    int max_threads;
    bool is_jit;

    atomic_flag is_tls_defined;

    // we have a global lock since the arena can be accessed
    // from any thread.
    mtx_t lock;

    TB_ABI target_abi;
    TB_Arch target_arch;
    TB_System target_system;
    TB_FeatureSet features;

    // This is a hack for windows since they've got this idea
    // of a _tls_index
    TB_Symbol* tls_index_extern;

    size_t comdat_function_count; // compiled function count
    _Atomic size_t compiled_function_count;

    // symbol table
    _Atomic size_t symbol_count[TB_SYMBOL_MAX];
    _Atomic(TB_Symbol*) first_symbol_of_tag[TB_SYMBOL_MAX];

    alignas(64) struct {
        Pool(TB_Global) globals;
        Pool(TB_External) externals;
    } thread_info[TB_MAX_THREADS];

    DynArray(TB_File) files;

    // Common sections
    // TODO(NeGate): custom sections
    TB_ModuleSection text, data, rdata, tls;

    // windows specific lol
    TB_LinkerSectionPiece* xdata;

    // The code is stored into giant buffers
    // there's on per code gen thread so that
    // each can work at the same time without
    // making any allocations within the code
    // gen.
    TB_CodeRegion* code_regions[TB_MAX_THREADS];
};

typedef struct {
    size_t length;
    TB_ObjectSection* data;
} TB_SectionGroup;

typedef struct {
    uint32_t used;
    uint8_t data[];
} TB_TemporaryStorage;

// the maximum size the prologue and epilogue can be for any machine code impl
enum { PROEPI_BUFFER = 256 };
typedef struct {
    // what does CHAR_BIT mean on said platform
    int minimum_addressable_size, pointer_size;

    void (*get_data_type_size)(TB_DataType dt, size_t* out_size, size_t* out_align);

    // return the number of patches resolved
    size_t (*emit_call_patches)(TB_Module* restrict m);

    size_t (*emit_prologue)(uint8_t* out, uint64_t saved, uint64_t stack_usage);
    size_t (*emit_epilogue)(uint8_t* out, uint64_t saved, uint64_t stack_usage);

    // NULLable if doesn't apply
    void (*emit_win64eh_unwind_info)(TB_Emitter* e, TB_FunctionOutput* out_f, uint64_t saved, uint64_t stack_usage);

    void (*fast_path)(TB_Function* restrict f, TB_FunctionOutput* restrict func_out, const TB_FeatureSet* features, uint8_t* out, size_t out_capacity);
    void (*complex_path)(TB_Function* restrict f, TB_FunctionOutput* restrict func_out, const TB_FeatureSet* features, uint8_t* out, size_t out_capacity);
} ICodeGen;

// All debug formats i know of boil down to adding some extra sections to the object file
typedef struct {
    const char* name;

    bool (*supported_target)(TB_Module* m);
    int (*number_of_debug_sections)(TB_Module* m);

    // functions are laid out linearly based on their function IDs and
    // thus function_sym_start tells you what the starting point is in the symbol table
    TB_SectionGroup (*generate_debug_info)(TB_Module* m, TB_TemporaryStorage* tls);
} IDebugFormat;

#define TB_FITS_INTO(T,x) ((x) == (T)(x))

// tb_todo means it's something we fill in later
// tb_unreachable means it's logically impossible to reach
// tb_assume means we assume some expression cannot be false
//
// in debug builds these are all checked and tb_todo is some sort of trap
#if defined(_MSC_VER) && !defined(__clang__)
#if TB_DEBUG_BUILD
#define tb_todo()            (assert(0 && "TODO"), __assume(0))
#define tb_unreachable()     (assert(0), __assume(0), 0)
#else
#define tb_todo()            abort()
#define tb_unreachable()     (__assume(0), 0)
#endif
#else
#if TB_DEBUG_BUILD
#define tb_todo()            __builtin_trap()
#define tb_unreachable()     (assert(0), 0)
#else
#define tb_todo()            __builtin_trap()
#define tb_unreachable()     (__builtin_unreachable(), 0)
#endif
#endif

#ifndef NDEBUG
#define tb_assert(condition, ...) ((condition) ? 0 : (fprintf(stderr, __FILE__ ": " STR(__LINE__) ": assertion failed: " #condition "\n  "), fprintf(stderr, __VA_ARGS__), abort(), 0))
#else
#define tb_assert(condition, ...) (0)
#endif

#if defined(_WIN32) && !defined(__GNUC__)
#define tb_panic(...)                     \
do {                                      \
    printf(__VA_ARGS__);                  \
    __fastfail(FAST_FAIL_FATAL_APP_EXIT); \
} while (0)
#else
#define tb_panic(...)                     \
do {                                      \
    printf(__VA_ARGS__);                  \
    abort();                              \
} while (0)
#endif

#ifndef COUNTOF
#define COUNTOF(...) (sizeof(__VA_ARGS__) / sizeof((__VA_ARGS__)[0]))
#endif

#ifndef CONCAT
#define CONCAT_(x, y) x ## y
#define CONCAT(x, y) CONCAT_(x, y)
#endif

// NOTE(NeGate): if you steal it you should restore the used amount back to what it was before
TB_TemporaryStorage* tb_tls_steal(void);
TB_TemporaryStorage* tb_tls_allocate(void);
void* tb_tls_push(TB_TemporaryStorage* store, size_t size);
void* tb_tls_try_push(TB_TemporaryStorage* store, size_t size);
void tb_tls_restore(TB_TemporaryStorage* store, void* ptr);
void* tb_tls_pop(TB_TemporaryStorage* store, size_t size);
void* tb_tls_peek(TB_TemporaryStorage* store, size_t distance);
bool tb_tls_can_fit(TB_TemporaryStorage* store, size_t size);

ICodeGen* tb__find_code_generator(TB_Module* m);

void* tb_out_reserve(TB_Emitter* o, size_t count);
void tb_out_commit(TB_Emitter* o, size_t count);

// reserves & commits
void* tb_out_grab(TB_Emitter* o, size_t count);
size_t tb_out_grab_i(TB_Emitter* o, size_t count);
size_t tb_out_get_pos(TB_Emitter* o, void* p);

// Adds null terminator onto the end and returns the starting position of the string
size_t tb_outstr_nul_UNSAFE(TB_Emitter* o, const char* str);
size_t tb_outstr_nul(TB_Emitter* o, const char* str);

void tb_out1b_UNSAFE(TB_Emitter* o, uint8_t i);
void tb_out4b_UNSAFE(TB_Emitter* o, uint32_t i);
void tb_outstr_UNSAFE(TB_Emitter* o, const char* str);
void tb_outs_UNSAFE(TB_Emitter* o, size_t len, const void* str);
size_t tb_outs(TB_Emitter* o, size_t len, const void* str);
void* tb_out_get(TB_Emitter* o, size_t pos);

// fills region with zeros
void tb_out_zero(TB_Emitter* o, size_t len);

void tb_out1b(TB_Emitter* o, uint8_t i);
void tb_out2b(TB_Emitter* o, uint16_t i);
void tb_out4b(TB_Emitter* o, uint32_t i);
void tb_out8b(TB_Emitter* o, uint64_t i);
void tb_patch1b(TB_Emitter* o, uint32_t pos, uint8_t i);
void tb_patch2b(TB_Emitter* o, uint32_t pos, uint16_t i);
void tb_patch4b(TB_Emitter* o, uint32_t pos, uint32_t i);
void tb_patch8b(TB_Emitter* o, uint32_t pos, uint64_t i);

uint8_t  tb_get1b(TB_Emitter* o, uint32_t pos);
uint16_t tb_get2b(TB_Emitter* o, uint32_t pos);
uint32_t tb_get4b(TB_Emitter* o, uint32_t pos);

////////////////////////////////
// CFG analysis
////////////////////////////////
typedef NL_Map(TB_Node*, TB_Node*) TB_Dominators;
typedef NL_HashSet TB_FrontierSet;
typedef NL_Map(TB_Node*, TB_FrontierSet) TB_DominanceFrontiers;

typedef struct {
    size_t count;
    TB_Node** traversal;

    NL_Map(TB_Node*, char) visited;
} TB_PostorderWalk;

////////////////////////////////
// IR ANALYSIS
////////////////////////////////
TB_API TB_Dominators tb_get_dominators(TB_Function* f, TB_PostorderWalk order);

// Allocates from the heap and requires freeing with tb_function_free_postorder
TB_API TB_PostorderWalk tb_function_get_postorder(TB_Function* f);
TB_API void tb_function_free_postorder(TB_PostorderWalk* walk);

inline static uint64_t align_up(uint64_t a, uint64_t b) {
    return a + (b - (a % b)) % b;
}

// NOTE(NeGate): Considers 0 as a power of two
inline static bool tb_is_power_of_two(uint64_t x) {
    return (x & (x - 1)) == 0;
}

////////////////////////////////
// HELPER FUNCTIONS
////////////////////////////////
#ifdef _MSC_VER
#define TB_LIKELY(x)   (!!(x))
#define TB_UNLIKELY(x) (!!(x))
#else
#define TB_LIKELY(x)   __builtin_expect(!!(x), 1)
#define TB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

TB_Node* tb_alloc_node(TB_Function* f, int type, TB_DataType dt, int input_count, size_t extra);

////////////////////////////////
// EXPORTER HELPER
////////////////////////////////
size_t tb_helper_write_text_section(size_t write_pos, TB_Module* m, uint8_t* output, uint32_t pos);
size_t tb_helper_write_section(TB_Module* m, size_t write_pos, TB_ModuleSection* section, uint8_t* output, uint32_t pos);
size_t tb_helper_get_text_section_layout(TB_Module* m, size_t symbol_id_start);

size_t tb__layout_relocations(TB_Module* m, DynArray(TB_ModuleSection*) sections, const ICodeGen* restrict code_gen, size_t output_size, size_t reloc_size, bool sizing);

TB_ExportChunk* tb_export_make_chunk(size_t size);
void tb_export_append_chunk(TB_ExportBuffer* buffer, TB_ExportChunk* c);

////////////////////////////////
// ANALYSIS
////////////////////////////////
int tb__get_local_tid(void);
TB_Symbol* tb_symbol_alloc(TB_Module* m, enum TB_SymbolTag tag, ptrdiff_t len, const char* name, size_t size);
void tb_symbol_append(TB_Module* m, TB_Symbol* s);

void tb_emit_symbol_patch(TB_FunctionOutput* func_out, const TB_Symbol* target, size_t pos);

// trusty lil hash functions
uint32_t tb__crc32(uint32_t crc, size_t length, const void* data);

// out_bytes needs at least 16 bytes
void tb__md5sum(uint8_t* out_bytes, uint8_t* initial_msg, size_t initial_len);

uint64_t tb__sxt(uint64_t src, uint64_t src_bits, uint64_t dst_bits);

char* tb__arena_strdup(TB_Module* m, ptrdiff_t len, const char* src);

// temporary arena
extern thread_local Arena tb__arena;

// NOTE(NeGate): Place all the codegen interfaces down here
extern ICodeGen tb__x64_codegen;
extern ICodeGen tb__aarch64_codegen;
extern ICodeGen tb__wasm32_codegen;

// And all debug formats here
//extern IDebugFormat dwarf_debug_format;
extern IDebugFormat tb__codeview_debug_format;