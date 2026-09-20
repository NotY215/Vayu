
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <direct.h>
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#endif

typedef struct { int64_t len; char data[]; } VayuStr;
typedef struct {
    int64_t len;
    int64_t cap;
    int64_t* items;
    int8_t*  tags;   /* per-element kind: 0=int 1=bool 2=str 3=list 4=map 5=tuple 6=set */
} VayuList;
typedef struct { VayuStr* key; int64_t value; uint8_t used; } VayuMapEntry;
typedef struct { int64_t len; int64_t cap; VayuMapEntry* entries; } VayuMap;
typedef struct { VayuStr* typeName; VayuStr* message; } VayuExc;

void vayu_raise_str(VayuStr* typeName, VayuStr* msg);
int64_t vayu_str_to_int(VayuStr* s);

/* Forward declarations for list helpers used by the new string methods
   (partition, splitlines, rsplit) that are defined before the list block. */
VayuList* vayu_list_new(void);
void vayu_list_push(VayuList* l, int64_t v);
void vayu_list_push_tagged(VayuList* l, int64_t v, int64_t tag);

/* Phase 15.2d: forward decls for vayu_map_slot, which is defined before
   the map implementation block. */
static VayuStr* vayu_concat_c(const char* prefix, VayuStr* s);
VayuMap* vayu_map_new(void);
void     vayu_map_put(VayuMap* m, VayuStr* k, int64_t v);
static uint64_t hash_str(VayuStr* s);
static VayuMapEntry* map_find(VayuMap* m, VayuStr* k);
static void map_grow(VayuMap* m);

static int    g_argc = 0;
static char** g_argv = NULL;

void* vayu_alloc(int64_t size) {
    void* p = malloc((size_t)size);
    if (!p) { fprintf(stderr, "vayu: oom\n"); exit(1); }
    return p;
}

static VayuStr* vayu_mkstr(const char* cstr, int64_t n) {
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)n + 1);
    s->len = n;
    if (n > 0) memcpy(s->data, cstr, (size_t)n);
    s->data[n] = 0;
    return s;
}
static VayuStr* vayu_mkstr_c(const char* cstr) {
    return vayu_mkstr(cstr, (int64_t)strlen(cstr));
}

void vayu_print_int(long long v) { printf("%lld\n", v); }
void vayu_print_bool(long long v) { printf("%s\n", v ? "true" : "false"); }
void vayu_print_int_noln(long long v) { printf("%lld", v); }
void vayu_print_bool_noln(long long v) { printf("%s", v ? "true" : "false"); }
void vayu_print_space(void) { putchar(' '); }
void vayu_print_ln(void) { putchar('\n'); }
void vayu_print_str(VayuStr* s) {
    fwrite(s->data, 1, (size_t)s->len, stdout); putchar('\n');
}
void vayu_print_str_noln(VayuStr* s) {
    fwrite(s->data, 1, (size_t)s->len, stdout);
}

void vayu_print_raw(VayuStr* s) {
    fwrite(s->data, 1, (size_t)s->len, stdout);
    fflush(stdout);
}

/* ---- Phase 15.1: C FFI string bridging ---- */
char* vayu_str_cstr(VayuStr* s) { return s->data; }
VayuStr* vayu_cstr_to_str(const char* p) {
    if (!p) return vayu_mkstr("", 0);
    return vayu_mkstr_c(p);
}

/* ===========================================================================
 * Phase 16.0–16.2: CPython dynamic-loading shim.
 *
 *  16.0  dynamic discovery + py.init / py.version / py.run
 *  16.1  value bridging: py.int / py.str / py.bool + py.from_*
 *  16.2  calling:        py.import / py.getattr / py.call / py.decref
 *
 * Loads python3.dll (or libpython3.so) at first use via:
 *   1) VAYU_PYTHON_DLL / VAYU_PYTHON_SO env override
 *   2) auto-discovery by shelling out to `where python`
 *   3) bare-name candidates on PATH
 * ========================================================================= */
#ifdef _WIN32
#  define VAYU_PY_DLL_T HMODULE
#  define VAYU_PY_OPEN(n)  LoadLibraryA(n)
#  define VAYU_PY_SYM(h,n) GetProcAddress(h,n)
#else
#  include <dlfcn.h>
#  define VAYU_PY_DLL_T void*
#  define VAYU_PY_OPEN(n)  dlopen(n, RTLD_LAZY | RTLD_GLOBAL)
#  define VAYU_PY_SYM(h,n) dlsym(h,n)
#endif

static VAYU_PY_DLL_T g_py_dll = NULL;
static int           g_py_inited = 0;

typedef int         (*vayu_py_init_t)(int);
typedef int         (*vayu_py_final_t)(void);
typedef int         (*vayu_py_isinit_t)(void);
typedef const char* (*vayu_py_ver_t)(void);
typedef int         (*vayu_py_runstr_t)(const char*);
typedef void        (*vayu_py_errprint_t)(void);

typedef int64_t     (*vayu_py_long_from_t)(int64_t);
typedef int64_t     (*vayu_py_long_as_t)(int64_t);
typedef int64_t     (*vayu_py_unicode_from_t)(const char*, int64_t);
typedef const char* (*vayu_py_unicode_as_t)(int64_t);
typedef int64_t     (*vayu_py_bool_from_t)(int64_t);
typedef int         (*vayu_py_is_true_t)(int64_t);

typedef int64_t     (*vayu_py_import_t)(const char*);
typedef int64_t     (*vayu_py_getattr_t)(int64_t, const char*);
typedef int64_t     (*vayu_py_call_t)(int64_t, int64_t, int64_t);
typedef int64_t     (*vayu_py_tuple_new_t)(int64_t);
typedef int         (*vayu_py_tuple_set_t)(int64_t, int64_t, int64_t);
typedef void        (*vayu_py_decref_t)(int64_t);
typedef void        (*vayu_py_incref_t)(int64_t);

static vayu_py_init_t      p_Py_InitializeEx           = NULL;
static vayu_py_final_t     p_Py_FinalizeEx             = NULL;
static vayu_py_isinit_t    p_Py_IsInitialized          = NULL;
static vayu_py_ver_t       p_Py_GetVersion             = NULL;
static vayu_py_runstr_t    p_PyRun_SimpleString        = NULL;
static vayu_py_errprint_t  p_PyErr_Print               = NULL;

static vayu_py_long_from_t    p_PyLong_FromLongLong         = NULL;
static vayu_py_long_as_t      p_PyLong_AsLongLong           = NULL;
static vayu_py_unicode_from_t p_PyUnicode_FromStringAndSize = NULL;
static vayu_py_unicode_as_t   p_PyUnicode_AsUTF8            = NULL;
static vayu_py_bool_from_t    p_PyBool_FromLong             = NULL;
static vayu_py_is_true_t      p_PyObject_IsTrue             = NULL;

static vayu_py_import_t     p_PyImport_ImportModule    = NULL;
static vayu_py_getattr_t    p_PyObject_GetAttrString   = NULL;
static vayu_py_call_t       p_PyObject_Call            = NULL;
static vayu_py_tuple_new_t  p_PyTuple_New              = NULL;
static vayu_py_tuple_set_t  p_PyTuple_SetItem          = NULL;
static vayu_py_decref_t     p_Py_DecRef                = NULL;
static vayu_py_incref_t     p_Py_IncRef                = NULL;

/* ---- DLL discovery ---- */

/* Upgrade the loader: use LOAD_WITH_ALTERED_SEARCH_PATH so that when we
   load tools\python313.dll, its vcruntime*.dll and DLLs\*.pyd siblings
   are resolved from tools\ first (not from the exe's directory). */
#ifdef _WIN32
#  undef VAYU_PY_OPEN
static VAYU_PY_DLL_T vayu_py_open_ex(const char* n) {
    if (strchr(n, '\\') || strchr(n, '/'))
        return LoadLibraryExA(n, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    return LoadLibraryA(n);
}
#  define VAYU_PY_OPEN(n)  vayu_py_open_ex(n)
#endif

/* Versioned runtime DLLs only.  We deliberately exclude the stable-ABI
   forwarder `python3.dll` — on this system it does not export
   PyRun_SimpleString, and using it silently loses py.run(). */
#ifdef _WIN32
static const char* const vayu_py_dll_names[] = {
    "python313.dll", "python312.dll", "python311.dll",
    "python310.dll", "python39.dll",  "python38.dll",
    "python37.dll",  "python36.dll",
    NULL
};
#else
static const char* const vayu_py_dll_names[] = {
    "libpython3.13.so", "libpython3.12.so", "libpython3.11.so",
    "libpython3.10.so", "libpython3.9.so",  "libpython3.8.so",
    "libpython3.so",
    NULL
};
#endif

static int vayu_py_try_dir(const char* dir, char* out, size_t outsz) {
    if (!dir || !*dir) return 0;
    for (int i = 0; vayu_py_dll_names[i]; ++i) {
        int n = snprintf(out, outsz,
#ifdef _WIN32
                         "%s\\%s",
#else
                         "%s/%s",
#endif
                         dir, vayu_py_dll_names[i]);
        if (n <= 0 || (size_t)n >= outsz) continue;
        FILE* g = fopen(out, "rb");
        if (!g) continue;
        fclose(g);
        return 1;
    }
    return 0;
}

/* Search <cwd>\tools\python3XX.dll and walk up a few levels, so an exe
   run from build\x64-debug\bin still finds the DLLs the user dropped in
   E:\Vayu\tools\. */
static int vayu_py_search_upward(char* out, size_t outsz, int maxUp) {
    char buf[2048];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf)) == NULL) return 0;
#else
    if (getcwd(buf, sizeof(buf)) == NULL) return 0;
#endif
    for (int i = 0; i <= maxUp; ++i) {
        char cand[2048];
#ifdef _WIN32
        snprintf(cand, sizeof(cand), "%s\\tools", buf);
#else
        snprintf(cand, sizeof(cand), "%s/tools", buf);
#endif
        if (vayu_py_try_dir(cand, out, outsz)) return 1;
        if (vayu_py_try_dir(buf, out, outsz)) return 1;

        size_t blen = strlen(buf);
        while (blen > 0 && (buf[blen-1] == '\\' || buf[blen-1] == '/'))
            buf[--blen] = 0;
#ifdef _WIN32
        char* slash = strrchr(buf, '\\');
#else
        char* slash = strrchr(buf, '/');
#endif
        if (!slash) break;
        if (slash == buf) { buf[1] = 0; }
        else { *slash = 0; }
    }
    return 0;
}

static int vayu_py_discover(char* out, size_t outsz) {
#ifdef _WIN32
    FILE* f = _popen("where python 2>nul", "r");
#else
    FILE* f = popen("which python3 2>/dev/null", "r");
#endif
    if (!f) return 0;
    char line[2048];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        size_t start = 0;
        while (start < n && (line[start] == ' ' || line[start] == '\t')) ++start;
        if (start > 0) { memmove(line, line + start, n - start + 1); n -= start; }
        if (n < 2) continue;
#ifdef _WIN32
        char* slash = strrchr(line, '\\');
#else
        char* slash = strrchr(line, '/');
#endif
        if (!slash) continue;
        *slash = 0;
        if (vayu_py_try_dir(line, out, outsz)) { found = 1; break; }
    }
#ifdef _WIN32
    _pclose(f);
#else
    pclose(f);
#endif
    return found;
}

static int vayu_py_load_dll(void) {
    if (g_py_dll) return 1;
    const int dbg = (getenv("VAYU_PY_DEBUG") != NULL);

    /* 1) Env override. */
    const char* envp =
#ifdef _WIN32
        getenv("VAYU_PYTHON_DLL");
#else
        getenv("VAYU_PYTHON_SO");
#endif
    if (envp && *envp) {
        if (dbg) fprintf(stderr, "[py] env override: %s\n", envp);
        g_py_dll = VAYU_PY_OPEN(envp);
        if (dbg && !g_py_dll)
            fprintf(stderr, "[py] env override load failed\n");
    }

    /* 2) tools\python3XX.dll next to or above the cwd. */
    if (!g_py_dll) {
        char local[2048];
        if (vayu_py_search_upward(local, sizeof(local), 6)) {
            if (dbg) fprintf(stderr, "[py] upward search: %s\n", local);
            g_py_dll = VAYU_PY_OPEN(local);
            if (dbg && !g_py_dll)
                fprintf(stderr, "[py] LoadLibrary failed on %s\n", local);
        } else if (dbg) {
            fprintf(stderr, "[py] upward search found nothing "
                            "(looked for tools\\python3XX.dll)\n");
        }
    }

    /* 3) `where python` discovery — parent dir of python.exe. */
    if (!g_py_dll) {
        char discovered[2048];
        if (vayu_py_discover(discovered, sizeof(discovered))) {
            if (dbg) fprintf(stderr, "[py] where python: %s\n", discovered);
            g_py_dll = VAYU_PY_OPEN(discovered);
            if (dbg && !g_py_dll)
                fprintf(stderr, "[py] LoadLibrary failed on %s\n", discovered);
        } else if (dbg) {
            fprintf(stderr, "[py] `where python` discovery failed\n");
        }
    }

    /* 4) Bare-name candidates — let the OS search PATH. */
    if (!g_py_dll) {
        for (int i = 0; vayu_py_dll_names[i]; ++i) {
            g_py_dll = VAYU_PY_OPEN(vayu_py_dll_names[i]);
            if (g_py_dll) {
                if (dbg) fprintf(stderr, "[py] bare-name load: %s\n",
                                 vayu_py_dll_names[i]);
                break;
            }
        }
    }
    if (!g_py_dll) {
        if (dbg) fprintf(stderr, "[py] all load paths failed\n");
        return 0;
    }

    if (dbg) {
        char modpath[2048];
        DWORD got = GetModuleFileNameA(g_py_dll, modpath, sizeof(modpath));
        if (got > 0) modpath[got] = 0;
        else         modpath[0] = 0;
        fprintf(stderr, "[py] loaded: %s\n", modpath);
    }

    p_Py_InitializeEx    = (vayu_py_init_t)   VAYU_PY_SYM(g_py_dll, "Py_InitializeEx");
    p_Py_FinalizeEx      = (vayu_py_final_t)  VAYU_PY_SYM(g_py_dll, "Py_FinalizeEx");
    p_Py_IsInitialized   = (vayu_py_isinit_t) VAYU_PY_SYM(g_py_dll, "Py_IsInitialized");
    p_Py_GetVersion      = (vayu_py_ver_t)    VAYU_PY_SYM(g_py_dll, "Py_GetVersion");
    p_PyRun_SimpleString = (vayu_py_runstr_t) VAYU_PY_SYM(g_py_dll, "PyRun_SimpleString");
    p_PyErr_Print        = (vayu_py_errprint_t)VAYU_PY_SYM(g_py_dll, "PyErr_Print");

    p_PyLong_FromLongLong         = (vayu_py_long_from_t)
        VAYU_PY_SYM(g_py_dll, "PyLong_FromLongLong");
    p_PyLong_AsLongLong           = (vayu_py_long_as_t)
        VAYU_PY_SYM(g_py_dll, "PyLong_AsLongLong");
    p_PyUnicode_FromStringAndSize = (vayu_py_unicode_from_t)
        VAYU_PY_SYM(g_py_dll, "PyUnicode_FromStringAndSize");
    p_PyUnicode_AsUTF8            = (vayu_py_unicode_as_t)
        VAYU_PY_SYM(g_py_dll, "PyUnicode_AsUTF8");
    p_PyBool_FromLong             = (vayu_py_bool_from_t)
        VAYU_PY_SYM(g_py_dll, "PyBool_FromLong");
    p_PyObject_IsTrue             = (vayu_py_is_true_t)
        VAYU_PY_SYM(g_py_dll, "PyObject_IsTrue");

    p_PyImport_ImportModule  = (vayu_py_import_t)
        VAYU_PY_SYM(g_py_dll, "PyImport_ImportModule");
    p_PyObject_GetAttrString = (vayu_py_getattr_t)
        VAYU_PY_SYM(g_py_dll, "PyObject_GetAttrString");
    p_PyObject_Call          = (vayu_py_call_t)
        VAYU_PY_SYM(g_py_dll, "PyObject_Call");
    p_PyTuple_New            = (vayu_py_tuple_new_t)
        VAYU_PY_SYM(g_py_dll, "PyTuple_New");
    p_PyTuple_SetItem        = (vayu_py_tuple_set_t)
        VAYU_PY_SYM(g_py_dll, "PyTuple_SetItem");
    p_Py_DecRef              = (vayu_py_decref_t)
        VAYU_PY_SYM(g_py_dll, "Py_DecRef");
    p_Py_IncRef              = (vayu_py_incref_t)
        VAYU_PY_SYM(g_py_dll, "Py_IncRef");

    if (!p_Py_InitializeEx || !p_Py_GetVersion || !p_PyRun_SimpleString) {
        if (dbg) fprintf(stderr,
            "[py] required symbols missing after load: "
            "InitializeEx=%p GetVersion=%p Run_SimpleString=%p\n",
            (void*)p_Py_InitializeEx, (void*)p_Py_GetVersion,
            (void*)p_PyRun_SimpleString);
        g_py_dll = NULL;
        return 0;
    }
    return 1;
}

/* ---- 16.0: init / version / run ---- */
int64_t vayu_py_init(void) {
    if (g_py_inited) return 1;
    if (!vayu_py_load_dll()) return 0;
    p_Py_InitializeEx(0);
    g_py_inited = 1;
    return 1;
}

VayuStr* vayu_py_version(void) {
    if (!vayu_py_init()) return vayu_mkstr_c("(no python runtime found)");
    const char* v = p_Py_GetVersion();
    if (!v) return vayu_mkstr_c("(unknown)");
    const char* nl = strchr(v, '\n');
    if (nl) return vayu_mkstr(v, (int64_t)(nl - v));
    return vayu_mkstr_c(v);
}

void vayu_py_run(const char* src) {
    if (!vayu_py_init()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    int rc = p_PyRun_SimpleString(src);
    if (rc != 0 && p_PyErr_Print) p_PyErr_Print();
}

/* ---- 16.1: value bridging ---- */
int64_t vayu_py_int(int64_t v) {
    if (!vayu_py_init() || !p_PyLong_FromLongLong) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyLong_FromLongLong(v);
}

int64_t vayu_py_bool(int64_t v) {
    if (!vayu_py_init() || !p_PyBool_FromLong) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyBool_FromLong(v ? 1 : 0);
}

int64_t vayu_py_str(int64_t sp) {
    if (!vayu_py_init() || !p_PyUnicode_FromStringAndSize) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuStr* s = (VayuStr*)sp;
    return p_PyUnicode_FromStringAndSize(s->data, s->len);
}

int64_t vayu_py_from_int(int64_t p) {
    if (!vayu_py_init() || !p_PyLong_AsLongLong) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyLong_AsLongLong(p);
}

int64_t vayu_py_from_str(int64_t p) {
    if (!vayu_py_init() || !p_PyUnicode_AsUTF8) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    const char* s = p_PyUnicode_AsUTF8(p);
    if (!s) return (int64_t)vayu_mkstr("", 0);
    return (int64_t)vayu_mkstr_c(s);
}

int64_t vayu_py_from_bool(int64_t p) {
    if (!vayu_py_init() || !p_PyObject_IsTrue) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return (int64_t)(p_PyObject_IsTrue(p) ? 1 : 0);
}

/* ---- 16.2: calling Python ---- */
int64_t vayu_py_import(const char* name) {
    if (!vayu_py_init() || !p_PyImport_ImportModule) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyImport_ImportModule(name);
}

int64_t vayu_py_getattr(int64_t obj, const char* name) {
    if (!vayu_py_init() || !p_PyObject_GetAttrString) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!obj) return 0;
    return p_PyObject_GetAttrString(obj, name);
}

void vayu_py_decref(int64_t obj) {
    if (!vayu_py_init()) return;
    if (obj && p_Py_DecRef) p_Py_DecRef(obj);
}

void vayu_py_incref(int64_t obj) {
    if (!vayu_py_init()) return;
    if (obj && p_Py_IncRef) p_Py_IncRef(obj);
}

static int64_t vayu_py_arg_to_obj(int64_t v, int8_t tag) {
    if (tag == 0 || tag == 1) {
        return p_PyLong_FromLongLong(v);
    }
    if (tag == 2) {
        VayuStr* s = (VayuStr*)v;
        return p_PyUnicode_FromStringAndSize(s->data, s->len);
    }
    return 0;
}

int64_t vayu_py_call(int64_t fn, int64_t args_list) {
    if (!vayu_py_init() || !p_PyObject_Call || !p_PyTuple_New) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!fn) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.call: null function handle"));
    }
    VayuList* lst = (VayuList*)args_list;
    int64_t n = lst->len;
    int64_t tup = p_PyTuple_New(n);
    if (!tup) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.call: PyTuple_New failed"));
    }
    for (int64_t i = 0; i < n; ++i) {
        int64_t arg = vayu_py_arg_to_obj(lst->items[i], lst->tags[i]);
        if (!arg) {
            if (p_Py_DecRef) p_Py_DecRef(tup);
            vayu_raise_str(vayu_mkstr_c("TypeError"),
                           vayu_mkstr_c("py.call: unsupported argument type"));
        }
        p_PyTuple_SetItem(tup, i, arg);
    }
    int64_t result = p_PyObject_Call(fn, tup, 0);
    if (p_Py_DecRef) p_Py_DecRef(tup);
    return result;
}

/* ---- Phase 16.3 + 16.4: bidirectional ---- */

/* Additional symbols needed for eval / exec_file / callback / attrs. */
typedef int64_t (*vayu_py_runstring_t)(const char*, int, int64_t, int64_t);
typedef int64_t (*vayu_py_addmodule_t)(const char*);
typedef int64_t (*vayu_py_getdict_t)(int64_t);
typedef int64_t (*vayu_py_cfunc_new_t)(void*, int64_t, int64_t);
typedef int64_t (*vayu_py_capsule_new_t)(void*, const char*, void*);
typedef void*   (*vayu_py_capsule_get_t)(int64_t, const char*);
typedef int64_t (*vayu_py_tuple_size_t)(int64_t);
typedef int64_t (*vayu_py_tuple_getitem_t)(int64_t, int64_t);
typedef void    (*vayu_py_err_clear_t)(void);
typedef int64_t (*vayu_py_setattr_t)(int64_t, const char*, int64_t);
typedef int64_t (*vayu_py_repr_t)(int64_t);
typedef int64_t (*vayu_py_type_t)(int64_t);
typedef int     (*vayu_py_gil_ensure_t)(void);
typedef void    (*vayu_py_gil_release_t)(int);
typedef int64_t (*vayu_py_dict_new_t)(void);
typedef int     (*vayu_py_dict_setstr_t)(int64_t, const char*, int64_t);
typedef int64_t (*vayu_py_dict_size_t)(int64_t);
typedef int64_t (*vayu_py_dict_keys_t)(int64_t);
typedef int64_t (*vayu_py_dict_getitem_t)(int64_t, int64_t);
typedef int64_t (*vayu_py_list_size_t)(int64_t);
typedef int64_t (*vayu_py_list_getitem_t)(int64_t, int64_t);
typedef int64_t (*vayu_py_obj_str_t)(int64_t);
typedef int64_t (*vayu_py_err_occurred_t)(void);
typedef void    (*vayu_py_err_fetch_t)(int64_t*, int64_t*, int64_t*);
typedef void    (*vayu_py_err_norm_t)(int64_t*, int64_t*, int64_t*);

typedef struct {
    int64_t (*fn)(int64_t);
    int      ret_kind;   /* 0=int 1=bool 2=str */
} VayuPyCallback;

static vayu_py_runstring_t     p_PyRun_String          = NULL;
static vayu_py_addmodule_t     p_PyImport_AddModule    = NULL;
static vayu_py_getdict_t       p_PyModule_GetDict      = NULL;
static vayu_py_cfunc_new_t     p_PyCFunction_NewEx     = NULL;
static vayu_py_capsule_new_t   p_PyCapsule_New         = NULL;
static vayu_py_capsule_get_t   p_PyCapsule_GetPointer  = NULL;
static vayu_py_tuple_size_t    p_PyTuple_Size          = NULL;
static vayu_py_tuple_getitem_t p_PyTuple_GetItem       = NULL;
static vayu_py_err_clear_t     p_PyErr_Clear           = NULL;
static vayu_py_setattr_t       p_PyObject_SetAttrString = NULL;
static vayu_py_repr_t          p_PyObject_Repr          = NULL;
static vayu_py_type_t          p_PyObject_Type          = NULL;
static vayu_py_gil_ensure_t    p_PyGILState_Ensure      = NULL;
static vayu_py_gil_release_t   p_PyGILState_Release     = NULL;
static vayu_py_dict_new_t      p_PyDict_New             = NULL;
static vayu_py_dict_setstr_t   p_PyDict_SetItemString   = NULL;
static vayu_py_dict_size_t     p_PyDict_Size            = NULL;
static vayu_py_dict_keys_t     p_PyDict_Keys            = NULL;
static vayu_py_dict_getitem_t  p_PyDict_GetItem         = NULL;
static vayu_py_list_size_t     p_PyList_Size            = NULL;
static vayu_py_list_getitem_t  p_PyList_GetItem         = NULL;
static vayu_py_obj_str_t       p_PyObject_Str           = NULL;
static vayu_py_err_occurred_t  p_PyErr_Occurred         = NULL;
static vayu_py_err_fetch_t     p_PyErr_Fetch            = NULL;
static vayu_py_err_norm_t      p_PyErr_NormalizeException = NULL;

static void* p_PyLong_Type    = NULL;
static void* p_PyBool_Type    = NULL;
static void* p_PyUnicode_Type = NULL;

static int vayu_py_load_bidi(void) {
    if (!vayu_py_load_dll()) return 0;
    if (!p_PyRun_String) {
        p_PyRun_String          = (vayu_py_runstring_t)
            VAYU_PY_SYM(g_py_dll, "PyRun_String");
        p_PyImport_AddModule    = (vayu_py_addmodule_t)
            VAYU_PY_SYM(g_py_dll, "PyImport_AddModule");
        p_PyModule_GetDict      = (vayu_py_getdict_t)
            VAYU_PY_SYM(g_py_dll, "PyModule_GetDict");
        p_PyCFunction_NewEx     = (vayu_py_cfunc_new_t)
            VAYU_PY_SYM(g_py_dll, "PyCFunction_NewEx");
        p_PyCapsule_New         = (vayu_py_capsule_new_t)
            VAYU_PY_SYM(g_py_dll, "PyCapsule_New");
        p_PyCapsule_GetPointer  = (vayu_py_capsule_get_t)
            VAYU_PY_SYM(g_py_dll, "PyCapsule_GetPointer");
        p_PyTuple_Size          = (vayu_py_tuple_size_t)
            VAYU_PY_SYM(g_py_dll, "PyTuple_Size");
        p_PyTuple_GetItem       = (vayu_py_tuple_getitem_t)
            VAYU_PY_SYM(g_py_dll, "PyTuple_GetItem");
        p_PyErr_Clear           = (vayu_py_err_clear_t)
            VAYU_PY_SYM(g_py_dll, "PyErr_Clear");
        p_PyObject_SetAttrString = (vayu_py_setattr_t)
            VAYU_PY_SYM(g_py_dll, "PyObject_SetAttrString");
        p_PyObject_Repr          = (vayu_py_repr_t)
            VAYU_PY_SYM(g_py_dll, "PyObject_Repr");
        p_PyObject_Type          = (vayu_py_type_t)
            VAYU_PY_SYM(g_py_dll, "PyObject_Type");
        p_PyGILState_Ensure      = (vayu_py_gil_ensure_t)
            VAYU_PY_SYM(g_py_dll, "PyGILState_Ensure");
        p_PyGILState_Release     = (vayu_py_gil_release_t)
            VAYU_PY_SYM(g_py_dll, "PyGILState_Release");
        p_PyLong_Type    = VAYU_PY_SYM(g_py_dll, "PyLong_Type");
        p_PyBool_Type    = VAYU_PY_SYM(g_py_dll, "PyBool_Type");
        p_PyUnicode_Type = VAYU_PY_SYM(g_py_dll, "PyUnicode_Type");
                p_PyDict_New              = (vayu_py_dict_new_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_New");
        p_PyDict_SetItemString    = (vayu_py_dict_setstr_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_SetItemString");
        p_PyDict_Size             = (vayu_py_dict_size_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_Size");
        p_PyDict_Keys             = (vayu_py_dict_keys_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_Keys");
        p_PyDict_GetItem          = (vayu_py_dict_getitem_t)
            VAYU_PY_SYM(g_py_dll, "PyDict_GetItem");
        p_PyList_Size             = (vayu_py_list_size_t)
            VAYU_PY_SYM(g_py_dll, "PyList_Size");
        p_PyList_GetItem          = (vayu_py_list_getitem_t)
            VAYU_PY_SYM(g_py_dll, "PyList_GetItem");
        p_PyObject_Str            = (vayu_py_obj_str_t)
            VAYU_PY_SYM(g_py_dll, "PyObject_Str");
        p_PyErr_Occurred          = (vayu_py_err_occurred_t)
            VAYU_PY_SYM(g_py_dll, "PyErr_Occurred");
        p_PyErr_Fetch             = (vayu_py_err_fetch_t)
            VAYU_PY_SYM(g_py_dll, "PyErr_Fetch");
        p_PyErr_NormalizeException = (vayu_py_err_norm_t)
            VAYU_PY_SYM(g_py_dll, "PyErr_NormalizeException");
    }
    return p_PyRun_String && p_PyImport_AddModule && p_PyModule_GetDict &&
           p_PyCFunction_NewEx && p_PyCapsule_New && p_PyCapsule_GetPointer &&
           p_PyTuple_Size && p_PyTuple_GetItem &&
           p_PyLong_Type && p_PyBool_Type && p_PyUnicode_Type;
}

/* Py_eval_input is 258 (CPython stable value). */
int64_t vayu_py_eval(int64_t expr_str) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuStr* s = (VayuStr*)expr_str;
    int64_t main_mod = p_PyImport_AddModule("__main__");
    int64_t d        = p_PyModule_GetDict(main_mod);
    int64_t r        = p_PyRun_String(s->data, 258, d, d);
    /* leave error pending for py.last_error() */
    if (!r && p_PyErr_Clear && 0) p_PyErr_Clear();
    return r;
}

void vayu_py_exec_file(int64_t path_str) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuStr* path = (VayuStr*)path_str;
    char pathbuf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(pathbuf, path->data, (size_t)n);
    pathbuf[n] = 0;

    FILE* f = fopen(pathbuf, "rb");
    if (!f) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_concat_c("py.exec_file: cannot open ", path));
    }
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char* buf = (char*)malloc((size_t)sz + 1);
    if (sz > 0) fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);

    if (!vayu_py_init()) {
        free(buf);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    int rc = p_PyRun_SimpleString(buf);
    free(buf);
    if (rc != 0 && p_PyErr_Print) p_PyErr_Print();
}

/* ---- Vayu callbacks callable from Python ---- */

/* PyObject header on 64-bit: ob_refcnt then ob_type. */
static void* vayu_py_type_of(void* obj) {
    return *(void**)((char*)obj + sizeof(void*));
}

static int64_t vayu_py_obj_to_i64(int64_t obj) {
    if (!obj) return 0;
    void* t = vayu_py_type_of((void*)obj);
    if (t == p_PyBool_Type) {
        return (int64_t)(p_PyObject_IsTrue(obj) ? 1 : 0);
    }
    if (t == p_PyLong_Type) {
        return p_PyLong_AsLongLong(obj);
    }
    if (t == p_PyUnicode_Type) {
        const char* s = p_PyUnicode_AsUTF8(obj);
        if (!s) return 0;
        return (int64_t)vayu_mkstr_c(s);
    }
    return 0;
}

static int64_t vayu_py_i64_to_obj(int64_t v, int ret_kind) {
    if (ret_kind == 2) {  /* str */
        VayuStr* s = (VayuStr*)v;
        return p_PyUnicode_FromStringAndSize(s->data, s->len);
    }
    if (ret_kind == 1) {  /* bool */
        return p_PyBool_FromLong(v ? 1 : 0);
    }
    return p_PyLong_FromLongLong(v);
}

static int64_t vayu_py_cb_call(int64_t self, int64_t args);

static void vayu_py_cb_destructor(int64_t cap) {
    VayuPyCallback* cb = (VayuPyCallback*)p_PyCapsule_GetPointer(
        cap, "vayu_py_callback");
    if (cb) free(cb);
}

int64_t vayu_py_callback(int64_t fn_ptr, int64_t ret_kind_str) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!fn_ptr) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("py.callback: null function pointer"));
    }
    VayuStr* rk = (VayuStr*)ret_kind_str;
    int kind = 0;
    if (rk->len == 4 && memcmp(rk->data, "bool", 4) == 0) kind = 1;
    else if (rk->len == 3 && memcmp(rk->data, "str", 3) == 0) kind = 2;

    VayuPyCallback* cb = (VayuPyCallback*)malloc(sizeof(VayuPyCallback));
    cb->fn = (int64_t (*)(int64_t))fn_ptr;
    cb->ret_kind = kind;

    int64_t cap = p_PyCapsule_New(cb, "vayu_py_callback",
                                  (void*)&vayu_py_cb_destructor);
    if (!cap) {
        free(cb);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.callback: PyCapsule_New failed"));
    }

    /* PyMethodDef must outlive the PyCFunction object — leak one. */
    struct VayuMethDef {
        const char* ml_name;
        void*       ml_meth;
        int         ml_flags;
        const char* ml_doc;
    };
    struct VayuMethDef* md =
        (struct VayuMethDef*)malloc(sizeof(struct VayuMethDef));
    md->ml_name  = "vayu_cb";
    md->ml_meth  = (void*)&vayu_py_cb_call;
    md->ml_flags = 0x0001;   /* METH_VARARGS */
    md->ml_doc   = "Vayu callback";

    int64_t fn_obj = p_PyCFunction_NewEx(md, cap, 0);
    if (!fn_obj) {
        free(md);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.callback: PyCFunction_NewEx failed"));
    }
    if (p_Py_DecRef) p_Py_DecRef(cap);
    return fn_obj;
}

static int64_t vayu_py_cb_call(int64_t self, int64_t args) {
    VayuPyCallback* cb = (VayuPyCallback*)p_PyCapsule_GetPointer(
        self, "vayu_py_callback");
    if (!cb) return 0;
    /* 16.4: GIL guard so Vayu callbacks invoked from worker threads work. */
    int gil = 0;
    if (p_PyGILState_Ensure) gil = p_PyGILState_Ensure();
    int64_t n = p_PyTuple_Size(args);
    int64_t a0 = 0;
    if (n >= 1) {
        int64_t o = p_PyTuple_GetItem(args, 0);
        a0 = vayu_py_obj_to_i64(o);
    }
    int64_t r = cb->fn(a0);
    int64_t out = vayu_py_i64_to_obj(r, cb->ret_kind);
    if (p_PyGILState_Release) p_PyGILState_Release(gil);
    return out;
}

/* ---- Phase 16.4: attributes / repr / type_name ---- */

int64_t vayu_py_setattr(int64_t obj, int64_t name_str, int64_t value_obj) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuStr* n = (VayuStr*)name_str;
    char namebuf[512];
    int64_t ln = n->len < 511 ? n->len : 511;
    memcpy(namebuf, n->data, (size_t)ln);
    namebuf[ln] = 0;
    int rc = (int)p_PyObject_SetAttrString(obj, namebuf, value_obj);
    if (rc != 0) {
        if (p_PyErr_Print) p_PyErr_Print();
        return 0;
    }
    return 1;
}

int64_t vayu_py_repr(int64_t obj) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    return p_PyObject_Repr(obj);
}

int64_t vayu_py_type_name(int64_t obj) {
    if (!vayu_py_load_bidi() || !p_PyObject_GetAttrString) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!obj) return (int64_t)vayu_mkstr_c("NoneType");
    int64_t tp = p_PyObject_Type(obj);
    if (!tp) return (int64_t)vayu_mkstr_c("?");
    int64_t nm = p_PyObject_GetAttrString(tp, "__name__");
    if (!nm) {
        if (p_Py_DecRef) p_Py_DecRef(tp);
        return (int64_t)vayu_mkstr_c("?");
    }
    const char* s = p_PyUnicode_AsUTF8(nm);
    VayuStr* out = vayu_mkstr_c(s ? s : "?");
    if (p_Py_DecRef) { p_Py_DecRef(nm); p_Py_DecRef(tp); }
    return (int64_t)out;
}

/* ---- Phase 16.5: kwargs, list/dict bridging, last_error ---- */

int64_t vayu_py_call_kw(int64_t fn, int64_t kwargs_map) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    if (!fn) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.call_kw: null function handle"));
    }
    VayuMap* m = (VayuMap*)kwargs_map;
    int64_t d = p_PyDict_New();
    if (!d) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("py.call_kw: PyDict_New failed"));
    }
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        VayuStr* k = m->entries[i].key;
        char kbuf[512];
        int64_t kn = k->len < 511 ? k->len : 511;
        memcpy(kbuf, k->data, (size_t)kn);
        kbuf[kn] = 0;
        int rc = p_PyDict_SetItemString(d, kbuf, m->entries[i].value);
        if (rc != 0) {
            if (p_Py_DecRef) p_Py_DecRef(d);
            vayu_raise_str(vayu_mkstr_c("TypeError"),
                           vayu_mkstr_c("py.call_kw: bad kwarg name"));
        }
    }
    int64_t tup = p_PyTuple_New(0);
    int64_t r = p_PyObject_Call(fn, tup, d);
    if (p_Py_DecRef) { p_Py_DecRef(tup); p_Py_DecRef(d); }
    /* leave the error pending so py.last_error() can read it */
    return r;
}

VayuList* vayu_py_list(int64_t obj) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuList* out = vayu_list_new();
    if (!obj || !p_PyList_Size) return out;
    int64_t n = p_PyList_Size(obj);
    if (n < 0) { if (p_PyErr_Clear) p_PyErr_Clear(); return out; }
    for (int64_t i = 0; i < n; ++i) {
        int64_t v = p_PyList_GetItem(obj, i);
        vayu_list_push_tagged(out, v, 0);
    }
    return out;
}

VayuMap* vayu_py_dict(int64_t obj) {
    if (!vayu_py_load_bidi()) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("Python runtime not available"));
    }
    VayuMap* out = vayu_map_new();
    if (!obj || !p_PyDict_Size) return out;
    int64_t keys = p_PyDict_Keys(obj);
    if (!keys) {
        if (p_PyErr_Clear) p_PyErr_Clear();
        return out;
    }
    int64_t n = p_PyList_Size(keys);
    for (int64_t i = 0; i < n; ++i) {
        int64_t k = p_PyList_GetItem(keys, i);
        int64_t v = p_PyDict_GetItem(obj, k);
        const char* ks = p_PyUnicode_AsUTF8(k);
        if (!ks) continue;
        vayu_map_put(out, vayu_mkstr_c(ks), v);
    }
    if (p_Py_DecRef) p_Py_DecRef(keys);
    return out;
}

VayuStr* vayu_py_last_error(void) {
    if (!vayu_py_load_bidi()) return vayu_mkstr("", 0);
    if (!p_PyErr_Occurred || !p_PyErr_Occurred()) return vayu_mkstr("", 0);
    int64_t tp = 0, val = 0, tb = 0;
    p_PyErr_Fetch(&tp, &val, &tb);
    if (p_PyErr_NormalizeException)
        p_PyErr_NormalizeException(&tp, &val, &tb);
    VayuStr* out = vayu_mkstr_c("(unknown error)");
    if (val && p_PyObject_Str) {
        int64_t s = p_PyObject_Str(val);
        if (s) {
            const char* cs = p_PyUnicode_AsUTF8(s);
            if (cs) out = vayu_mkstr_c(cs);
            if (p_Py_DecRef) p_Py_DecRef(s);
        }
    }
    if (tp && p_Py_DecRef) p_Py_DecRef(tp);
    if (val && p_Py_DecRef) p_Py_DecRef(val);
    if (tb && p_Py_DecRef) p_Py_DecRef(tb);
    return out;
}

/* ---- Phase 15.2d: compound lvalue addresses ---- */
int64_t* vayu_list_slot(VayuList* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("list index out of range"));
    }
    return &l->items[i];
}
int64_t* vayu_map_slot(VayuMap* m, VayuStr* k) {
    VayuMapEntry* e = map_find(m, k);
    if (e) return &e->value;
    map_grow(m);
    uint64_t h = hash_str(k) & (uint64_t)(m->cap - 1);
    while (m->entries[h].used) h = (h + 1) & (uint64_t)(m->cap - 1);
    m->entries[h].used = 1;
    m->entries[h].key = k;
    m->entries[h].value = 0;
    m->len++;
    return &m->entries[h].value;
}

/* ---- Phase 15.4: C callback test helper ---- */
int64_t vayu_test_apply(int64_t (*f)(int64_t), int64_t x) {
    if (!f) return 0;
    return f(x);
}

/* ---- Phase 15.5: FFI by-value single-field structs ---- */
typedef struct { int64_t v; } VayuWrap8;

VayuWrap8 vayu_ffi_wrap_double(VayuWrap8 w) {
    VayuWrap8 r;
    r.v = w.v * 2;
    return r;
}

int64_t vayu_ffi_wrap_sum(VayuWrap8 a, VayuWrap8 b) {
    return a.v + b.v;
}

/* ---- Phase 15.2b / 15.3: FFI test helpers ---- */
int64_t vayu_ffi_deref(int64_t p) { return *(int64_t*)p; }
void vayu_ffi_store(int64_t p, int64_t v) { *(int64_t*)p = v; }

void* vayu_ffi_point_new(int64_t x, int64_t y) {
    int64_t* p = (int64_t*)malloc(16);
    p[0] = x;
    p[1] = y;
    return p;
}
int64_t vayu_ffi_point_sum(void* p) {
    int64_t* ip = (int64_t*)p;
    return ip[0] + ip[1];
}

VayuStr* vayu_read_line(void) {
    size_t cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) {
        free(buf);
        return vayu_mkstr("", 0);
    }
    if (len > 0 && buf[len - 1] == '\r') --len;
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + len + 1);
    s->len = (int64_t)len;
    memcpy(s->data, buf, len);
    s->data[len] = 0;
    free(buf);
    return s;
}

VayuStr* vayu_read_all(void) {
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
    }
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + len + 1);
    s->len = (int64_t)len;
    memcpy(s->data, buf, len);
    s->data[len] = 0;
    free(buf);
    return s;
}

int64_t vayu_read_int(void) {
    VayuStr* s = vayu_read_line();
    return vayu_str_to_int(s);
}

VayuStr* vayu_input_plain(void) {
    return vayu_read_line();
}

VayuStr* vayu_input_prompt(VayuStr* prompt) {
    if (prompt->len) fwrite(prompt->data, 1, (size_t)prompt->len, stdout);
    fflush(stdout);
    return vayu_read_line();
}

static VayuStr* vayu_concat_c(const char* prefix, VayuStr* s) {
    int64_t plen = (int64_t)strlen(prefix);
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)(plen + s->len) + 1);
    r->len = plen + s->len;
    if (plen)   memcpy(r->data, prefix, (size_t)plen);
    if (s->len) memcpy(r->data + plen, s->data, (size_t)s->len);
    r->data[r->len] = 0;
    return r;
}

VayuStr* vayu_str_concat(VayuStr* a, VayuStr* b) {
    int64_t n = a->len + b->len;
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)n + 1);
    s->len = n;
    if (a->len) memcpy(s->data, a->data, (size_t)a->len);
    if (b->len) memcpy(s->data + a->len, b->data, (size_t)b->len);
    s->data[n] = 0;
    return s;
}
int64_t vayu_str_eq(VayuStr* a, VayuStr* b) {
    if (a == b) return 1;
    if (a->len != b->len) return 0;
    return memcmp(a->data, b->data, (size_t)a->len) == 0;
}
int64_t vayu_str_ne(VayuStr* a, VayuStr* b) { return !vayu_str_eq(a, b); }
int64_t vayu_str_len(VayuStr* s) { return s->len; }

VayuStr* vayu_int_to_str(long long v, long long kind) {
    char buf[64]; int n;
    if (kind == 1) n = snprintf(buf, sizeof(buf), "%s", v ? "true" : "false");
    else           n = snprintf(buf, sizeof(buf), "%lld", v);
    return vayu_mkstr(buf, n);
}
VayuStr* vayu_str_upper(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        r->data[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    r->data[s->len] = 0;
    return r;
}
VayuStr* vayu_str_lower(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    r->data[s->len] = 0;
    return r;
}
int64_t vayu_str_contains(VayuStr* s, VayuStr* sub) {
    if (sub->len == 0) return 1;
    if (sub->len > s->len) return 0;
    for (int64_t i = 0; i + sub->len <= s->len; ++i)
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return 1;
    return 0;
}
int64_t vayu_str_find(VayuStr* s, VayuStr* sub) {
    if (sub->len == 0) return 0;
    if (sub->len > s->len) return -1;
    for (int64_t i = 0; i + sub->len <= s->len; ++i)
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
    return -1;
}
int64_t vayu_str_starts_with(VayuStr* s, VayuStr* p) {
    if (p->len > s->len) return 0;
    return memcmp(s->data, p->data, (size_t)p->len) == 0;
}
int64_t vayu_str_ends_with(VayuStr* s, VayuStr* p) {
    if (p->len > s->len) return 0;
    return memcmp(s->data + (s->len - p->len), p->data, (size_t)p->len) == 0;
}

VayuStr* vayu_str_char_at(VayuStr* s, int64_t i) {
    if (i < 0) i += s->len;
    if (i < 0 || i >= s->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("string index out of range"));
    }
    return vayu_mkstr(s->data + i, 1);
}
VayuStr* vayu_str_substr(VayuStr* s, int64_t start, int64_t end) {
    if (start < 0) start += s->len;
    if (end   < 0) end   += s->len;
    if (start < 0) start = 0;
    if (end   > s->len) end = s->len;
    if (end < start) end = start;
    return vayu_mkstr(s->data + start, end - start);
}
VayuStr* vayu_str_capitalize(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (s->len > 0) {
        char c = r->data[0];
        if (c >= 'a' && c <= 'z') r->data[0] = (char)(c - 32);
    }
    r->data[s->len] = 0;
    return r;
}

VayuStr* vayu_str_title(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    int atStart = 1;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        int isWS = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                    c == '\f' || c == '\v');
        if (isWS) { r->data[i] = (char)c; atStart = 1; }
        else if (atStart) {
            r->data[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
            atStart = 0;
        } else {
            r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
        }
    }
    r->data[s->len] = 0;
    return r;
}

VayuStr* vayu_str_swapcase(VayuStr* s) {
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)s->len + 1);
    r->len = s->len;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (c >= 'a' && c <= 'z')      r->data[i] = (char)(c - 32);
        else if (c >= 'A' && c <= 'Z') r->data[i] = (char)(c + 32);
        else                           r->data[i] = c;
    }
    r->data[s->len] = 0;
    return r;
}

static VayuStr* vayu_str_pad_common(VayuStr* s, int64_t width,
                                    VayuStr* fill, int mode) {
    if (fill->len == 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("fill character must not be empty"));
    }
    int64_t pad = width - s->len;
    if (pad <= 0) return vayu_mkstr(s->data, s->len);
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) +
                                  (size_t)(s->len + pad) + 1);
    r->len = s->len + pad;
    int64_t lp = 0, rp = 0;
    if (mode == 1)      rp = pad;
    else if (mode == 2) lp = pad;
    else { lp = pad / 2; rp = pad - lp; }
    int64_t op = 0;
    for (int64_t i = 0; i < lp; ++i)
        r->data[op++] = fill->data[i % fill->len];
    if (s->len) memcpy(r->data + op, s->data, (size_t)s->len);
    op += s->len;
    for (int64_t i = 0; i < rp; ++i)
        r->data[op++] = fill->data[i % fill->len];
    r->data[op] = 0;
    return r;
}
VayuStr* vayu_str_center(VayuStr* s, int64_t w, VayuStr* f) {
    return vayu_str_pad_common(s, w, f, 0);
}
VayuStr* vayu_str_ljust(VayuStr* s, int64_t w, VayuStr* f) {
    return vayu_str_pad_common(s, w, f, 1);
}
VayuStr* vayu_str_rjust(VayuStr* s, int64_t w, VayuStr* f) {
    return vayu_str_pad_common(s, w, f, 2);
}

VayuStr* vayu_str_zfill(VayuStr* s, int64_t w) {
    int64_t pad = w - s->len;
    if (pad <= 0) return vayu_mkstr(s->data, s->len);
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)w + 1);
    r->len = w;
    int64_t op = 0, start = 0;
    if (s->len > 0 && (s->data[0] == '-' || s->data[0] == '+')) {
        r->data[op++] = s->data[0];
        start = 1;
    }
    for (int64_t i = 0; i < pad; ++i) r->data[op++] = '0';
    if (s->len > start) {
        memcpy(r->data + op, s->data + start, (size_t)(s->len - start));
        op += s->len - start;
    }
    r->data[op] = 0;
    return r;
}

int64_t vayu_str_count(VayuStr* s, VayuStr* sub) {
    if (sub->len == 0) return s->len + 1;
    int64_t count = 0;
    for (int64_t i = 0; i + sub->len <= s->len; ) {
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) {
            ++count; i += sub->len;
        } else ++i;
    }
    return count;
}

int64_t vayu_str_rfind(VayuStr* s, VayuStr* sub) {
    if (sub->len == 0) return s->len;
    if (sub->len > s->len) return -1;
    for (int64_t i = s->len - sub->len; i >= 0; --i) {
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return i;
    }
    return -1;
}

VayuList* vayu_str_partition(VayuStr* s, VayuStr* sep, int64_t right) {
    VayuList* out = vayu_list_new();
    if (sep->len == 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("empty separator"));
    }
    int64_t p = -1;
    if (right) {
        for (int64_t i = s->len - sep->len; i >= 0; --i) {
            if (memcmp(s->data + i, sep->data, (size_t)sep->len) == 0) {
                p = i; break;
            }
        }
    } else {
        for (int64_t i = 0; i + sep->len <= s->len; ++i) {
            if (memcmp(s->data + i, sep->data, (size_t)sep->len) == 0) {
                p = i; break;
            }
        }
    }
    if (p < 0) {
        if (right) {
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr("", 0), 2);
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr("", 0), 2);
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data, s->len), 2);
        } else {
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data, s->len), 2);
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr("", 0), 2);
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr("", 0), 2);
        }
        return out;
    }
    vayu_list_push_tagged(out, (int64_t)vayu_mkstr(s->data, p), 2);
    vayu_list_push_tagged(out, (int64_t)vayu_mkstr(sep->data, sep->len), 2);
    vayu_list_push_tagged(out,
        (int64_t)vayu_mkstr(s->data + p + sep->len,
                            s->len - p - sep->len), 2);
    return out;
}

VayuList* vayu_str_splitlines(VayuStr* s) {
    VayuList* out = vayu_list_new();
    int64_t start = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (c == '\n') {
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data + start, i - start), 2);
            start = i + 1;
        } else if (c == '\r') {
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data + start, i - start), 2);
            if (i + 1 < s->len && s->data[i + 1] == '\n') ++i;
            start = i + 1;
        }
    }
    if (start < s->len)
        vayu_list_push_tagged(out,
            (int64_t)vayu_mkstr(s->data + start, s->len - start), 2);
    return out;
}

VayuList* vayu_str_rsplit(VayuStr* s, VayuStr* sep, int64_t maxsplit) {
    VayuList* out = vayu_list_new();
    VayuStr** parts = NULL;
    int64_t n = 0, cap = 0;
    if (sep->len == 0) {
        int64_t end = s->len;
        while (end > 0) {
            if (maxsplit >= 0 && n >= maxsplit) break;
            while (end > 0 && (s->data[end-1]==' ' || s->data[end-1]=='\t' ||
                   s->data[end-1]=='\n' || s->data[end-1]=='\r' ||
                   s->data[end-1]=='\f' || s->data[end-1]=='\v')) --end;
            if (end == 0) break;
            int64_t st = end;
            while (st > 0 && !(s->data[st-1]==' ' || s->data[st-1]=='\t' ||
                   s->data[st-1]=='\n' || s->data[st-1]=='\r' ||
                   s->data[st-1]=='\f' || s->data[st-1]=='\v')) --st;
            if (n == cap) { cap = cap ? cap*2 : 4;
                parts = (VayuStr**)realloc(parts, sizeof(VayuStr*)*cap); }
            parts[n++] = vayu_mkstr(s->data + st, end - st);
            end = st;
        }
        if (end > 0) {
            int64_t a = 0;
            while (a < end && (s->data[a]==' ' || s->data[a]=='\t' ||
                   s->data[a]=='\n' || s->data[a]=='\r' ||
                   s->data[a]=='\f' || s->data[a]=='\v')) ++a;
            if (n == cap) { cap = cap ? cap*2 : 4;
                parts = (VayuStr**)realloc(parts, sizeof(VayuStr*)*cap); }
            parts[n++] = vayu_mkstr(s->data + a, end - a);
        }
    } else {
        int64_t end = s->len;
        while (maxsplit < 0 || n < maxsplit) {
            int64_t p = -1;
            if (end >= sep->len) {
                for (int64_t i = end - sep->len; i >= 0; --i) {
                    if (memcmp(s->data + i, sep->data,
                               (size_t)sep->len) == 0) { p = i; break; }
                }
            }
            if (p < 0) break;
            if (n == cap) { cap = cap ? cap*2 : 4;
                parts = (VayuStr**)realloc(parts, sizeof(VayuStr*)*cap); }
            parts[n++] = vayu_mkstr(s->data + p + sep->len,
                                    end - p - sep->len);
            end = p;
        }
        if (n == cap) { cap = cap ? cap*2 : 4;
            parts = (VayuStr**)realloc(parts, sizeof(VayuStr*)*cap); }
        parts[n++] = vayu_mkstr(s->data, end);
    }
    for (int64_t i = n - 1; i >= 0; --i)
        vayu_list_push_tagged(out, (int64_t)parts[i], 2);
    free(parts);
    return out;
}

int64_t vayu_str_is_lower(VayuStr* s) {
    int any = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (c >= 'A' && c <= 'Z') return 0;
        if (c >= 'a' && c <= 'z') any = 1;
    }
    return any;
}
int64_t vayu_str_is_upper(VayuStr* s) {
    int any = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (c >= 'a' && c <= 'z') return 0;
        if (c >= 'A' && c <= 'Z') any = 1;
    }
    return any;
}
int64_t vayu_str_is_alnum(VayuStr* s) {
    if (s->len == 0) return 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z'))) return 0;
    }
    return 1;
}
int64_t vayu_str_is_ascii(VayuStr* s) {
    for (int64_t i = 0; i < s->len; ++i)
        if ((unsigned char)s->data[i] > 127) return 0;
    return 1;
}
VayuStr* vayu_str_replace(VayuStr* s, VayuStr* from, VayuStr* to) {
    if (from->len == 0) return vayu_mkstr(s->data, s->len);
    int64_t count = 0;
    for (int64_t i = 0; i + from->len <= s->len; ) {
        if (memcmp(s->data + i, from->data, (size_t)from->len) == 0) {
            ++count; i += from->len;
        } else ++i;
    }
    int64_t newLen = s->len + count * (to->len - from->len);
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)newLen + 1);
    r->len = newLen;
    int64_t op = 0, ip = 0;
    while (ip < s->len) {
        if (ip + from->len <= s->len &&
            memcmp(s->data + ip, from->data, (size_t)from->len) == 0) {
            if (to->len) memcpy(r->data + op, to->data, (size_t)to->len);
            op += to->len;
            ip += from->len;
        } else {
            r->data[op++] = s->data[ip++];
        }
    }
    r->data[newLen] = 0;
    return r;
}
VayuStr* vayu_str_strip(VayuStr* s) {
    int64_t a = 0, b = s->len;
    while (a < b && (s->data[a]==' '||s->data[a]=='\t'||s->data[a]=='\n'||
                     s->data[a]=='\r'||s->data[a]=='\f'||s->data[a]=='\v')) ++a;
    while (b > a && (s->data[b-1]==' '||s->data[b-1]=='\t'||s->data[b-1]=='\n'||
                     s->data[b-1]=='\r'||s->data[b-1]=='\f'||s->data[b-1]=='\v')) --b;
    return vayu_mkstr(s->data + a, b - a);
}
VayuStr* vayu_str_lstrip(VayuStr* s) {
    int64_t a = 0;
    while (a < s->len && (s->data[a]==' '||s->data[a]=='\t'||s->data[a]=='\n'||
                          s->data[a]=='\r'||s->data[a]=='\f'||s->data[a]=='\v')) ++a;
    return vayu_mkstr(s->data + a, s->len - a);
}
VayuStr* vayu_str_rstrip(VayuStr* s) {
    int64_t b = s->len;
    while (b > 0 && (s->data[b-1]==' '||s->data[b-1]=='\t'||s->data[b-1]=='\n'||
                     s->data[b-1]=='\r'||s->data[b-1]=='\f'||s->data[b-1]=='\v')) --b;
    return vayu_mkstr(s->data, b);
}
int64_t vayu_str_is_digit(VayuStr* s) {
    if (s->len == 0) return 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (!isdigit(c)) return 0;
    }
    return 1;
}
int64_t vayu_str_is_alpha(VayuStr* s) {
    if (s->len == 0) return 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (!isalpha(c)) return 0;
    }
    return 1;
}
int64_t vayu_str_is_space(VayuStr* s) {
    if (s->len == 0) return 0;
    for (int64_t i = 0; i < s->len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        if (!isspace(c)) return 0;
    }
    return 1;
}
int64_t vayu_str_to_int(VayuStr* s) {
    int64_t n = 0;
    int64_t i = 0;
    int     neg = 0;
    while (i < s->len && (s->data[i]==' '||s->data[i]=='\t')) ++i;
    if (i < s->len && (s->data[i]=='-'||s->data[i]=='+')) {
        neg = (s->data[i]=='-'); ++i;
    }
    for (; i < s->len; ++i) {
        char c = s->data[i];
        if (c < '0' || c > '9') break;
        n = n * 10 + (c - '0');
    }
    return neg ? -n : n;
}

int64_t vayu_ord(VayuStr* s) {
    if (s->len != 1) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("ord() requires a length-1 string"));
    }
    return (int64_t)(unsigned char)s->data[0];
}
VayuStr* vayu_chr(int64_t n) {
    if (n < 0 || n > 255) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("chr() argument out of range"));
    }
    char c = (char)n;
    return vayu_mkstr(&c, 1);
}

VayuList* vayu_list_new() {
    VayuList* l = (VayuList*)malloc(sizeof(VayuList));
    l->len = 0; l->cap = 4;
    l->items = (int64_t*)malloc(sizeof(int64_t) * 4);
    l->tags  = (int8_t*)malloc(4);
    return l;
}
static void vayu_list_grow(VayuList* l) {
    if (l->len < l->cap) return;
    l->cap *= 2;
    l->items = (int64_t*)realloc(l->items, sizeof(int64_t) * (size_t)l->cap);
    l->tags  = (int8_t*)realloc(l->tags, (size_t)l->cap);
}
void vayu_list_push(VayuList* l, int64_t v) {
    vayu_list_grow(l);
    l->items[l->len] = v;
    l->tags[l->len]  = 0;
    l->len++;
}
/* Phase 13.3 fix: retag every element of a list.  Used at the boundary
   of a generic function call, where the erasure of `T` inside the body
   left the wrong tags on the result.  Caller-side code knows the actual
   element type and can restore the correct tag. */
void vayu_list_retag(VayuList* l, int64_t tag) {
    for (int64_t i = 0; i < l->len; ++i) l->tags[i] = (int8_t)tag;
}
void vayu_list_push_tagged(VayuList* l, int64_t v, int64_t tag) {
    vayu_list_grow(l);
    l->items[l->len] = v;
    l->tags[l->len]  = (int8_t)tag;
    l->len++;
}
int64_t vayu_list_get(VayuList* l, int64_t i) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("list index out of range"));
    }
    return l->items[i];
}
void vayu_list_set(VayuList* l, int64_t i, int64_t v) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("list index out of range"));
    }
    l->items[i] = v;
    l->tags[i]  = 0;
}
void vayu_list_set_tagged(VayuList* l, int64_t i, int64_t v, int64_t tag) {
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("list index out of range"));
    }
    l->items[i] = v;
    l->tags[i]  = (int8_t)tag;
}
int64_t vayu_list_pop(VayuList* l) {
    if (l->len == 0) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("pop from empty list"));
    }
    return l->items[--l->len];
}
int64_t vayu_list_len(VayuList* l) { return l->len; }
void vayu_list_clear(VayuList* l) { l->len = 0; }
int64_t vayu_list_contains(VayuList* l, int64_t v) {
    for (int64_t i = 0; i < l->len; ++i) if (l->items[i] == v) return 1;
    return 0;
}
void vayu_list_insert_tagged(VayuList* l, int64_t i, int64_t v, int64_t tag) {
    if (i < 0) i = 0;
    if (i > l->len) i = l->len;
    vayu_list_grow(l);
    memmove(l->items + i + 1, l->items + i,
            sizeof(int64_t) * (size_t)(l->len - i));
    memmove(l->tags + i + 1, l->tags + i,
            sizeof(int8_t) * (size_t)(l->len - i));
    l->items[i] = v;
    l->tags[i]  = (int8_t)tag;
    l->len++;
}
void vayu_list_insert(VayuList* l, int64_t i, int64_t v) {
    vayu_list_insert_tagged(l, i, v, 0);
}
void vayu_list_remove(VayuList* l, int64_t v) {
    for (int64_t i = 0; i < l->len; ++i) {
        if (l->items[i] == v) {
            memmove(l->items + i, l->items + i + 1,
                    sizeof(int64_t) * (size_t)(l->len - i - 1));
            memmove(l->tags + i, l->tags + i + 1,
                    sizeof(int8_t) * (size_t)(l->len - i - 1));
            l->len--;
            return;
        }
    }
}

void vayu_list_extend(VayuList* dst, VayuList* src) {
    for (int64_t i = 0; i < src->len; ++i) {
        vayu_list_grow(dst);
        dst->items[dst->len] = src->items[i];
        dst->tags[dst->len]  = src->tags[i];
        dst->len++;
    }
}

static int vayu_list_val_eq(int64_t a, int8_t at, int64_t b, int8_t bt) {
    if ((at == 0 || at == 1) && (bt == 0 || bt == 1)) return a == b;
    if (at != bt) return 0;
    if (at == 2) return vayu_str_eq((VayuStr*)a, (VayuStr*)b);
    return a == b;
}

int64_t vayu_list_count(VayuList* l, int64_t v, int64_t tag) {
    int64_t c = 0;
    for (int64_t i = 0; i < l->len; ++i)
        if (vayu_list_val_eq(l->items[i], l->tags[i], v, (int8_t)tag)) ++c;
    return c;
}

void vayu_list_reverse(VayuList* l) {
    for (int64_t i = 0, j = l->len - 1; i < j; ++i, --j) {
        int64_t t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t;
        int8_t  s = l->tags[i];  l->tags[i]  = l->tags[j];  l->tags[j]  = s;
    }
}

static int vayu_list_cmp_slot(VayuList* l, int64_t ia, int64_t ib, int allNum) {
    if (allNum) {
        int64_t a = l->items[ia], b = l->items[ib];
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    VayuStr* sa = (VayuStr*)l->items[ia];
    VayuStr* sb = (VayuStr*)l->items[ib];
    int64_t n = sa->len < sb->len ? sa->len : sb->len;
    int mc = memcmp(sa->data, sb->data, (size_t)n);
    if (mc < 0) return -1;
    if (mc > 0) return 1;
    return (sa->len < sb->len) ? -1 : (sa->len > sb->len) ? 1 : 0;
}

static void vayu_list_merge(VayuList* l, int64_t* tmpI, int8_t* tmpT,
                            int64_t lo, int64_t mid, int64_t hi, int allNum) {
    int64_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (vayu_list_cmp_slot(l, i, j, allNum) <= 0) {
            tmpI[k] = l->items[i]; tmpT[k] = l->tags[i]; ++i;
        } else {
            tmpI[k] = l->items[j]; tmpT[k] = l->tags[j]; ++j;
        }
        ++k;
    }
    while (i < mid) { tmpI[k] = l->items[i]; tmpT[k] = l->tags[i]; ++i; ++k; }
    while (j < hi)  { tmpI[k] = l->items[j]; tmpT[k] = l->tags[j]; ++j; ++k; }
    for (int64_t p = lo; p < hi; ++p) {
        l->items[p] = tmpI[p];
        l->tags[p]  = tmpT[p];
    }
}

void vayu_list_sort(VayuList* l) {
    if (l->len <= 1) return;
    int allNum = 1, allStr = 1;
    for (int64_t i = 0; i < l->len; ++i) {
        int8_t t = l->tags[i];
        if (t == 3 || t == 4) { allNum = 0; allStr = 0; }
        else if (t == 2)      { allNum = 0; }
        else                  { allStr = 0; }
        if (!allNum && !allStr) break;
    }
    if (!allNum && !allStr) {
        vayu_raise_str(vayu_mkstr_c("TypeError"),
                       vayu_mkstr_c("list.sort(): elements are not comparable"));
    }
    int64_t* tmpI = (int64_t*)malloc(sizeof(int64_t) * (size_t)l->len);
    int8_t*  tmpT = (int8_t*)malloc((size_t)l->len);
    int64_t width = 1;
    while (width < l->len) {
        for (int64_t i = 0; i < l->len; i += width * 2) {
            int64_t mid = i + width;
            int64_t hi  = i + width * 2;
            if (mid > l->len) mid = l->len;
            if (hi  > l->len) hi  = l->len;
            if (mid < hi)
                vayu_list_merge(l, tmpI, tmpT, i, mid, hi, allNum);
        }
        width *= 2;
    }
    free(tmpI);
    free(tmpT);
}

VayuList* vayu_list_copy(VayuList* l) {
    VayuList* r = (VayuList*)malloc(sizeof(VayuList));
    r->len = l->len;
    r->cap = l->len < 4 ? 4 : l->len;
    r->items = (int64_t*)malloc(sizeof(int64_t) * (size_t)r->cap);
    r->tags  = (int8_t*)malloc((size_t)r->cap);
    if (l->len) {
        memcpy(r->items, l->items, sizeof(int64_t) * (size_t)l->len);
        memcpy(r->tags,  l->tags,  (size_t)l->len);
    }
    return r;
}

/* ---- Phase 14.0: tuples (VayuList with tag 5) ---- */
typedef VayuList VayuTuple;

VayuTuple* vayu_tuple_new(void) { return vayu_list_new(); }
void vayu_tuple_push_tagged(VayuTuple* t, int64_t v, int64_t tag) {
    vayu_list_push_tagged(t, v, tag);
}
int64_t vayu_tuple_get(VayuTuple* t, int64_t i) {
    return vayu_list_get(t, i);
}
int64_t vayu_tuple_len(VayuTuple* t) { return t->len; }
VayuTuple* vayu_tuple_from_list(VayuList* l) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < l->len; ++i)
        vayu_list_push_tagged(r, l->items[i], l->tags[i]);
    return r;
}
int64_t vayu_tuple_count(VayuTuple* t, int64_t v, int64_t tag) {
    int64_t c = 0;
    for (int64_t i = 0; i < t->len; ++i)
        if (vayu_list_val_eq(t->items[i], t->tags[i], v, (int8_t)tag)) ++c;
    return c;
}

int64_t vayu_tuple_index(VayuTuple* t, int64_t v, int64_t tag) {
    for (int64_t i = 0; i < t->len; ++i)
        if (vayu_list_val_eq(t->items[i], t->tags[i], v, (int8_t)tag)) return i;
    vayu_raise_str(vayu_mkstr_c("ValueError"),
                   vayu_mkstr_c("tuple.index(): value not in tuple"));
    return -1;
}

VayuTuple* vayu_tuple_from_str(VayuStr* s) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < s->len; ++i)
        vayu_list_push_tagged(r, (int64_t)vayu_mkstr(s->data + i, 1), 2);
    return r;
}

static int64_t vayu_list_struct_eq(VayuList* a, VayuList* b) {
    if (a->len != b->len) return 0;
    for (int64_t i = 0; i < a->len; ++i) {
        int8_t ta = a->tags[i], tb = b->tags[i];
        if (ta != tb) return 0;
        if (ta == 2) {
            if (!vayu_str_eq((VayuStr*)a->items[i], (VayuStr*)b->items[i]))
                return 0;
        } else if (ta == 3 || ta == 5 || ta == 6) {
            if (!vayu_list_struct_eq((VayuList*)a->items[i],
                                     (VayuList*)b->items[i])) return 0;
        } else {
            if (a->items[i] != b->items[i]) return 0;
        }
    }
    return 1;
}
int64_t vayu_tuple_eq(VayuTuple* a, VayuTuple* b) {
    return vayu_list_struct_eq(a, b);
}
int64_t vayu_tuple_ne(VayuTuple* a, VayuTuple* b) {
    return !vayu_list_struct_eq(a, b);
}
/* ---- Phase 14.1: sets (VayuList with tag 6, dedup on push) ---- */
typedef VayuList VayuSet;

VayuSet* vayu_set_new(void) { return vayu_list_new(); }
void vayu_set_push_tagged(VayuSet* s, int64_t v, int64_t tag) {
    for (int64_t i = 0; i < s->len; ++i)
        if (vayu_list_val_eq(s->items[i], s->tags[i], v, (int8_t)tag)) return;
    vayu_list_push_tagged(s, v, tag);
}
int64_t vayu_set_has(VayuSet* s, int64_t v, int64_t tag) {
    for (int64_t i = 0; i < s->len; ++i)
        if (vayu_list_val_eq(s->items[i], s->tags[i], v, (int8_t)tag)) return 1;
    return 0;
}
void vayu_set_remove(VayuSet* s, int64_t v, int64_t tag) {
    for (int64_t i = 0; i < s->len; ++i) {
        if (vayu_list_val_eq(s->items[i], s->tags[i], v, (int8_t)tag)) {
            memmove(s->items + i, s->items + i + 1,
                    sizeof(int64_t) * (size_t)(s->len - i - 1));
            memmove(s->tags + i, s->tags + i + 1,
                    (size_t)(s->len - i - 1));
            s->len--;
            return;
        }
    }
}
void vayu_set_clear(VayuSet* s) { s->len = 0; }
VayuSet* vayu_set_union(VayuSet* a, VayuSet* b) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < a->len; ++i)
        vayu_list_push_tagged(r, a->items[i], a->tags[i]);
    for (int64_t i = 0; i < b->len; ++i)
        vayu_set_push_tagged(r, b->items[i], b->tags[i]);
    return r;
}
VayuSet* vayu_set_intersection(VayuSet* a, VayuSet* b) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < a->len; ++i)
        if (vayu_set_has(b, a->items[i], a->tags[i]))
            vayu_list_push_tagged(r, a->items[i], a->tags[i]);
    return r;
}
VayuSet* vayu_set_difference(VayuSet* a, VayuSet* b) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < a->len; ++i)
        if (!vayu_set_has(b, a->items[i], a->tags[i]))
            vayu_list_push_tagged(r, a->items[i], a->tags[i]);
    return r;
}
int64_t vayu_set_len(VayuSet* s) { return s->len; }
VayuSet* vayu_set_copy(VayuSet* s) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < s->len; ++i)
        vayu_list_push_tagged(r, s->items[i], s->tags[i]);
    return r;
}
VayuSet* vayu_set_from_list(VayuList* l) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < l->len; ++i)
        vayu_set_push_tagged(r, l->items[i], l->tags[i]);
    return r;
}
/* ---- Phase 14.4: slicing ---- */
int64_t vayu_slice(int64_t target, int64_t start, int64_t end,
                   int64_t hasStart, int64_t hasEnd, int64_t tag) {
    if (tag == 1) {  /* list */
        VayuList* src = (VayuList*)target;
        int64_t len = src->len;
        if (!hasStart) start = 0;
        else { if (start < 0) start += len; if (start < 0) start = 0;
               if (start > len) start = len; }
        if (!hasEnd) end = len;
        else { if (end < 0) end += len; if (end < 0) end = 0;
               if (end > len) end = len; }
        if (end < start) end = start;
        VayuList* out = vayu_list_new();
        for (int64_t i = start; i < end; ++i)
            vayu_list_push_tagged(out, src->items[i], src->tags[i]);
        return (int64_t)out;
    }
    if (tag == 5) {  /* tuple */
        VayuList* src = (VayuList*)target;
        int64_t len = src->len;
        if (!hasStart) start = 0;
        else { if (start < 0) start += len; if (start < 0) start = 0;
               if (start > len) start = len; }
        if (!hasEnd) end = len;
        else { if (end < 0) end += len; if (end < 0) end = 0;
               if (end > len) end = len; }
        if (end < start) end = start;
        VayuList* out = vayu_list_new();
        for (int64_t i = start; i < end; ++i)
            vayu_list_push_tagged(out, src->items[i], src->tags[i]);
        return (int64_t)out;
    }
    if (tag == 2) {  /* str */
        VayuStr* s = (VayuStr*)target;
        int64_t len = s->len;
        if (!hasStart) start = 0;
        else { if (start < 0) start += len; if (start < 0) start = 0;
               if (start > len) start = len; }
        if (!hasEnd) end = len;
        else { if (end < 0) end += len; if (end < 0) end = 0;
               if (end > len) end = len; }
        if (end < start) end = start;
        return (int64_t)vayu_mkstr(s->data + start, end - start);
    }
    vayu_raise_str(vayu_mkstr_c("TypeError"),
                   vayu_mkstr_c("cannot slice value"));
    return 0;
}

VayuTuple* vayu_tuple_concat(VayuTuple* a, VayuTuple* b) {
    VayuList* r = vayu_list_new();
    for (int64_t i = 0; i < a->len; ++i)
        vayu_list_push_tagged(r, a->items[i], a->tags[i]);
    for (int64_t i = 0; i < b->len; ++i)
        vayu_list_push_tagged(r, b->items[i], b->tags[i]);
    return r;
}
int64_t vayu_list_first(VayuList* l) {
    if (l->len == 0) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("first() on empty list"));
    }
    return l->items[0];
}
int64_t vayu_list_last(VayuList* l) {
    if (l->len == 0) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("last() on empty list"));
    }
    return l->items[l->len - 1];
}

VayuList* vayu_str_split(VayuStr* s, VayuStr* sep) {
    VayuList* out = vayu_list_new();
    if (sep->len == 0) {
        for (int64_t i = 0; i < s->len; ++i)
            vayu_list_push_tagged(out, (int64_t)vayu_mkstr(s->data + i, 1), 2);
        return out;
    }
    int64_t pos = 0;
    while (pos <= s->len) {
        int64_t next = -1;
        for (int64_t i = pos; i + sep->len <= s->len; ++i) {
            if (memcmp(s->data + i, sep->data, (size_t)sep->len) == 0) {
                next = i; break;
            }
        }
        if (next < 0) {
            vayu_list_push_tagged(out,
                (int64_t)vayu_mkstr(s->data + pos, s->len - pos), 2);
            break;
        }
        vayu_list_push_tagged(out,
            (int64_t)vayu_mkstr(s->data + pos, next - pos), 2);
        pos = next + sep->len;
    }
    return out;
}
VayuStr* vayu_str_join(VayuStr* sep, VayuList* parts) {
    int64_t total = 0;
    for (int64_t i = 0; i < parts->len; ++i) {
        VayuStr* p = (VayuStr*)parts->items[i];
        total += p->len;
        if (i + 1 < parts->len) total += sep->len;
    }
    VayuStr* r = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)total + 1);
    r->len = total;
    int64_t op = 0;
    for (int64_t i = 0; i < parts->len; ++i) {
        VayuStr* p = (VayuStr*)parts->items[i];
        if (p->len) memcpy(r->data + op, p->data, (size_t)p->len);
        op += p->len;
        if (i + 1 < parts->len) {
            if (sep->len) memcpy(r->data + op, sep->data, (size_t)sep->len);
            op += sep->len;
        }
    }
    r->data[total] = 0;
    return r;
}

static uint64_t hash_str(VayuStr* s) {
    uint64_t h = 1469598103934665603ULL;
    for (int64_t i = 0; i < s->len; ++i) { h ^= (uint8_t)s->data[i]; h *= 1099511628211ULL; }
    return h;
}
VayuMap* vayu_map_new() {
    VayuMap* m = (VayuMap*)malloc(sizeof(VayuMap));
    m->len = 0; m->cap = 8;
    m->entries = (VayuMapEntry*)calloc((size_t)m->cap, sizeof(VayuMapEntry));
    return m;
}
static VayuMapEntry* map_find(VayuMap* m, VayuStr* k) {
    uint64_t h = hash_str(k) & (uint64_t)(m->cap - 1);
    for (int64_t p = 0; p < m->cap; ++p) {
        VayuMapEntry* e = &m->entries[h];
        if (!e->used) return NULL;
        if (vayu_str_eq(e->key, k)) return e;
        h = (h + 1) & (uint64_t)(m->cap - 1);
    }
    return NULL;
}
static void map_grow(VayuMap* m) {
    if (m->len * 2 < m->cap) return;
    int64_t newCap = m->cap * 2;
    VayuMapEntry* ne = (VayuMapEntry*)calloc((size_t)newCap, sizeof(VayuMapEntry));
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        uint64_t h = hash_str(m->entries[i].key) & (uint64_t)(newCap - 1);
        while (ne[h].used) h = (h + 1) & (uint64_t)(newCap - 1);
        ne[h] = m->entries[i];
    }
    free(m->entries);
    m->entries = ne;
    m->cap = newCap;
}
void vayu_map_put(VayuMap* m, VayuStr* k, int64_t v) {
    VayuMapEntry* e = map_find(m, k);
    if (e) { e->value = v; return; }
    map_grow(m);
    uint64_t h = hash_str(k) & (uint64_t)(m->cap - 1);
    while (m->entries[h].used) h = (h + 1) & (uint64_t)(m->cap - 1);
    m->entries[h].used = 1;
    m->entries[h].key = k;
    m->entries[h].value = v;
    m->len++;
}
int64_t vayu_map_get(VayuMap* m, VayuStr* k) {
    VayuMapEntry* e = map_find(m, k);
    if (!e) vayu_raise_str(vayu_mkstr_c("KeyError"), k);
    return e->value;
}
int64_t vayu_map_has(VayuMap* m, VayuStr* k) { return map_find(m, k) != NULL; }
void vayu_map_remove(VayuMap* m, VayuStr* k) {
    VayuMapEntry* e = map_find(m, k); if (e) { e->used = 0; m->len--; }
}
int64_t vayu_map_len(VayuMap* m) { return m->len; }
void vayu_map_clear(VayuMap* m) {
    memset(m->entries, 0, sizeof(VayuMapEntry) * (size_t)m->cap);
    m->len = 0;
}
VayuMap* vayu_map_copy(VayuMap* m) {
    VayuMap* r = (VayuMap*)malloc(sizeof(VayuMap));
    r->len = m->len;
    r->cap = m->cap;
    r->entries = (VayuMapEntry*)calloc((size_t)r->cap, sizeof(VayuMapEntry));
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        r->entries[i] = m->entries[i];
    }
    return r;
}

int64_t vayu_map_get_or(VayuMap* m, VayuStr* k, int64_t def) {
    VayuMapEntry* e = map_find(m, k);
    return e ? e->value : def;
}

int64_t vayu_map_pop(VayuMap* m, VayuStr* k) {
    VayuMapEntry* e = map_find(m, k);
    if (!e) vayu_raise_str(vayu_mkstr_c("KeyError"), k);
    int64_t v = e->value;
    e->used = 0;
    m->len--;
    return v;
}

int64_t vayu_map_pop_or(VayuMap* m, VayuStr* k, int64_t def) {
    VayuMapEntry* e = map_find(m, k);
    if (!e) return def;
    int64_t v = e->value;
    e->used = 0;
    m->len--;
    return v;
}

void vayu_map_update(VayuMap* dst, VayuMap* src) {
    for (int64_t i = 0; i < src->cap; ++i) {
        if (!src->entries[i].used) continue;
        vayu_map_put(dst, src->entries[i].key, src->entries[i].value);
    }
}

VayuList* vayu_map_values(VayuMap* m, int64_t valTag) {
    VayuList* l = vayu_list_new();
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        vayu_list_push_tagged(l, m->entries[i].value, valTag);
    }
    return l;
}

VayuList* vayu_map_items(VayuMap* m, int64_t valTag) {
    VayuList* l = vayu_list_new();
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        VayuList* pair = vayu_list_new();
        vayu_list_push_tagged(pair, (int64_t)m->entries[i].key, 2);
        vayu_list_push_tagged(pair, m->entries[i].value, valTag);
        vayu_list_push_tagged(l, (int64_t)pair, 3);
    }
    return l;
}
VayuList* vayu_map_keys(VayuMap* m) {
    VayuList* l = vayu_list_new();
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        vayu_list_push_tagged(l, (int64_t)m->entries[i].key, 2);
    }
    return l;
}

int64_t vayu_len(int64_t v, int64_t kind) {
    switch (kind) {
        case 0: return vayu_str_len((VayuStr*)v);
        case 1: return vayu_list_len((VayuList*)v);
        case 2: return vayu_map_len((VayuMap*)v);
        case 3: return vayu_tuple_len((VayuTuple*)v);
        case 4: return vayu_set_len((VayuSet*)v);
    }
    return 0;
}
void vayu_print_value(int64_t v, int64_t kind) {
    switch (kind) {
        case 0: printf("%lld", (long long)v); break;
        case 1: printf("%s", v ? "true" : "false"); break;
        case 2: { VayuStr* s = (VayuStr*)v; fwrite(s->data, 1, (size_t)s->len, stdout); break; }
    }
}
void vayu_print_list_noln(VayuList* l);
void vayu_print_map_noln(VayuMap* m, int64_t vk);
void vayu_print_tuple_noln(VayuList* t);
void vayu_print_set_noln(VayuList* s);

void vayu_print_list_noln(VayuList* l) {
    putchar('[');
    for (int64_t i = 0; i < l->len; ++i) {
        if (i) printf(", ");
        int8_t t = l->tags[i];
        if (t == 1) {
            printf("%s", l->items[i] ? "true" : "false");
        } else if (t == 2) {
            VayuStr* s = (VayuStr*)l->items[i];
            putchar('"');
            fwrite(s->data, 1, (size_t)s->len, stdout);
            putchar('"');
        } else if (t == 3) {
            vayu_print_list_noln((VayuList*)l->items[i]);
        } else if (t == 4) {
            vayu_print_map_noln((VayuMap*)l->items[i], 0);
        } else if (t == 5) {
            vayu_print_tuple_noln((VayuTuple*)l->items[i]);
        } else if (t == 6) {
            vayu_print_set_noln((VayuSet*)l->items[i]);
        } else {
            printf("%lld", (long long)l->items[i]);
        }
    }
    putchar(']');
}
void vayu_print_tuple_noln(VayuTuple* t) {
    putchar('(');
    for (int64_t i = 0; i < t->len; ++i) {
        if (i) printf(", ");
        int8_t tag = t->tags[i];
        if (tag == 1) {
            printf("%s", t->items[i] ? "true" : "false");
        } else if (tag == 2) {
            VayuStr* s = (VayuStr*)t->items[i];
            putchar('"');
            fwrite(s->data, 1, (size_t)s->len, stdout);
            putchar('"');
        } else if (tag == 3) {
            vayu_print_list_noln((VayuList*)t->items[i]);
        } else if (tag == 4) {
            vayu_print_map_noln((VayuMap*)t->items[i], 0);
        } else if (tag == 5) {
            vayu_print_tuple_noln((VayuTuple*)t->items[i]);
        } else if (tag == 6) {
            vayu_print_set_noln((VayuSet*)t->items[i]);
        } else {
            printf("%lld", (long long)t->items[i]);
        }
    }
    if (t->len == 1) putchar(',');
    putchar(')');
}
void vayu_print_tuple(VayuTuple* t) { vayu_print_tuple_noln(t); putchar('\n'); }

void vayu_print_set_noln(VayuSet* s) {
    if (s->len == 0) { printf("set()"); return; }
    putchar('{');
    for (int64_t i = 0; i < s->len; ++i) {
        if (i) printf(", ");
        int8_t tag = s->tags[i];
        if (tag == 1) {
            printf("%s", s->items[i] ? "true" : "false");
        } else if (tag == 2) {
            VayuStr* str = (VayuStr*)s->items[i];
            putchar('"');
            fwrite(str->data, 1, (size_t)str->len, stdout);
            putchar('"');
        } else if (tag == 5) {
            vayu_print_tuple_noln((VayuTuple*)s->items[i]);
        } else if (tag == 6) {
            vayu_print_set_noln((VayuSet*)s->items[i]);
        } else {
            printf("%lld", (long long)s->items[i]);
        }
    }
    putchar('}');
}
void vayu_print_set(VayuSet* s) { vayu_print_set_noln(s); putchar('\n'); }
void vayu_print_list(VayuList* l, int64_t ek) {
    (void)ek;
    vayu_print_list_noln(l); putchar('\n');
}
void vayu_print_map_noln(VayuMap* m, int64_t vk) {
    putchar('{');
    int64_t printed = 0;
    for (int64_t i = 0; i < m->cap; ++i) {
        if (!m->entries[i].used) continue;
        if (printed) printf(", ");
        putchar('"');
        fwrite(m->entries[i].key->data, 1, (size_t)m->entries[i].key->len, stdout);
        printf("\": ");
        if (vk == 2) putchar('"');
        vayu_print_value(m->entries[i].value, vk);
        if (vk == 2) putchar('"');
        printed++;
    }
    putchar('}');
}
void vayu_print_map(VayuMap* m, int64_t vk) {
    vayu_print_map_noln(m, vk); putchar('\n');
}

long long vayu_floordiv(long long a, long long b) {
    if (b == 0) {
        vayu_raise_str(vayu_mkstr_c("ZeroDivisionError"),
                       vayu_mkstr_c("division by zero"));
    }
    long long q = a / b;
    if ((a ^ b) < 0 && q * b != a) q--;
    return q;
}
long long vayu_mod(long long a, long long b) {
    if (b == 0) {
        vayu_raise_str(vayu_mkstr_c("ZeroDivisionError"),
                       vayu_mkstr_c("modulo by zero"));
    }
    long long r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
long long vayu_pow_int(long long a, long long b) {
    if (b < 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("native: ** with a negative exponent is not supported"));
    }
    long long r = 1;
    while (b > 0) {
        if (b & 1) r *= a;
        b >>= 1;
        if (b) a *= a;
    }
    return r;
}

// ---- Phase 13.1: pure helpers ----
int64_t vayu_hash_int(int64_t x) {
    uint64_t h = (uint64_t)x;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (int64_t)h;
}
int64_t vayu_hash_str(VayuStr* s) {
    uint64_t h = 1469598103934665603ULL;
    for (int64_t i = 0; i < s->len; ++i) {
        h ^= (uint8_t)s->data[i];
        h *= 1099511628211ULL;
    }
    return (int64_t)h;
}
int64_t vayu_round_int(int64_t x) { return x; }
int64_t vayu_pow_mod(int64_t b, int64_t e, int64_t m) {
    if (m == 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("pow(): modulus cannot be zero"));
    }
    if (m < 0) m = -m;
    int64_t r = 1 % m;
    b %= m;
    if (b < 0) b += m;
    while (e > 0) {
        if (e & 1) r = (r * b) % m;
        b = (b * b) % m;
        e >>= 1;
    }
    return r;
}
int64_t vayu_sign_int(int64_t x) { return (x > 0) - (x < 0); }
int64_t vayu_gcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}
int64_t vayu_lcm(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    int64_t g = vayu_gcd(a, b);
    return (a / g) * b;
}
int64_t vayu_clamp_int(int64_t x, int64_t lo, int64_t hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
int64_t vayu_comb(int64_t n, int64_t k) {
    if (n < 0 || k < 0 || k > n) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("comb(): invalid arguments"));
    }
    if (k > n - k) k = n - k;
    int64_t r = 1;
    for (int64_t i = 1; i <= k; ++i) {
        r = r * (n - k + i) / i;
    }
    return r;
}

int64_t vayu_perm(int64_t n, int64_t k) {
    if (n < 0 || k < 0 || k > n) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("perm(): invalid arguments"));
    }
    int64_t r = 1;
    for (int64_t i = 0; i < k; ++i) r *= (n - i);
    return r;
}

int64_t vayu_isqrt(int64_t n) {
    if (n < 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("isqrt() of negative number"));
    }
    if (n < 2) return n;
    int64_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

int64_t vayu_factorial(int64_t n) {
    if (n < 0) {
        vayu_raise_str(vayu_mkstr_c("ValueError"),
                       vayu_mkstr_c("factorial() of negative number"));
    }
    if (n > 20) {
        vayu_raise_str(vayu_mkstr_c("OverflowError"),
                       vayu_mkstr_c("factorial() argument too large"));
    }
    int64_t r = 1;
    for (int64_t i = 2; i <= n; ++i) r *= i;
    return r;
}
int64_t vayu_list_size(VayuList* l) { return l->len; }

VayuStr* vayu_read_file(VayuStr* path) {
    char buf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(buf, path->data, (size_t)n);
    buf[n] = 0;

    FILE* f = fopen(buf, "rb");
    if (!f) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_concat_c("cannot open file: ", path));
    }
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    VayuStr* s = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)sz + 1);
    s->len = sz;
    if (sz > 0) fread(s->data, 1, (size_t)sz, f);
    s->data[sz] = 0;
    fclose(f);
    return s;
}
void vayu_write_file(VayuStr* path, VayuStr* content) {
    char buf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(buf, path->data, (size_t)n);
    buf[n] = 0;

    FILE* f = fopen(buf, "wb");
    if (!f) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_concat_c("cannot write file: ", path));
    }
    fwrite(content->data, 1, (size_t)content->len, f);
    fclose(f);
}
int64_t vayu_file_exists(VayuStr* path) {
    char buf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(buf, path->data, (size_t)n);
    buf[n] = 0;
    FILE* f = fopen(buf, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

VayuList* vayu_get_args(void) {
    VayuList* l = vayu_list_new();
    for (int i = 1; i < g_argc; ++i) {
        vayu_list_push(l, (int64_t)vayu_mkstr_c(g_argv[i]));
    }
    return l;
}
int64_t vayu_run_command(VayuStr* cmd) {
    char buf[8192];
    int64_t n = cmd->len < 8191 ? cmd->len : 8191;
    memcpy(buf, cmd->data, (size_t)n);
    buf[n] = 0;
#ifdef _WIN32
    {
        char wrapped[8256];
        int wr = snprintf(wrapped, sizeof(wrapped), "call %s", buf);
        if (wr > 0) return (int64_t)system(wrapped);
    }
#endif
    return (int64_t)system(buf);
}
void vayu_exit(int64_t code) { exit((int)code); }

// ---- try/except (thread-local for generator workers) ----

#define VAYU_MAX_TRY 64

#ifdef _MSC_VER
#  define VAYU_THREAD_LOCAL __declspec(thread)
#else
#  define VAYU_THREAD_LOCAL __thread
#endif

static VAYU_THREAD_LOCAL jmp_buf  g_jmpBufs[VAYU_MAX_TRY];
static VAYU_THREAD_LOCAL int      g_trySp = 0;
static VAYU_THREAD_LOCAL VayuExc* g_excValue = NULL;

int vayu_try_push(void) {
    if (g_trySp >= VAYU_MAX_TRY) {
        fprintf(stderr, "vayu: try stack overflow\n"); exit(1);
    }
    return g_trySp++;
}
void* vayu_try_buf(int id) { return &g_jmpBufs[id]; }
void vayu_try_pop(void) { if (g_trySp > 0) g_trySp--; }

VayuStr* vayu_get_exc_type(void) {
    return g_excValue ? g_excValue->typeName : NULL;
}
VayuExc* vayu_get_exc_value(void) { return g_excValue; }

VayuExc* vayu_mkexc(VayuStr* typeName, VayuStr* msg) {
    VayuExc* e = (VayuExc*)malloc(sizeof(VayuExc));
    e->typeName = typeName;
    e->message  = msg;
    return e;
}

void vayu_raise(VayuExc* e) {
    if (g_trySp == 0) {
        fwrite(e->typeName->data, 1, (size_t)e->typeName->len, stderr);
        fprintf(stderr, ": ");
        fwrite(e->message->data, 1, (size_t)e->message->len, stderr);
        fprintf(stderr, "\n");
        exit(1);
    }
    g_excValue = e;
    longjmp(g_jmpBufs[g_trySp - 1], 1);
}

void vayu_raise_str(VayuStr* typeName, VayuStr* msg) {
    VayuExc* e = vayu_mkexc(typeName, msg);
    vayu_raise(e);
}

void vayu_reraise(void) {
    if (g_trySp == 0) { fprintf(stderr, "vayu: uncaught\n"); exit(1); }
    longjmp(g_jmpBufs[g_trySp - 1], 1);
}

// ---- fs ----
static char* vayu_fs_cstr(VayuStr* s) {
    char* buf = (char*)malloc((size_t)s->len + 1);
    memcpy(buf, s->data, (size_t)s->len);
    buf[s->len] = 0;
    return buf;
}
static int64_t vayu_fs_stat_mode(const char* p) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p, &st) != 0) return -1;
    return (int64_t)st.st_mode;
#else
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (int64_t)st.st_mode;
#endif
}
int64_t vayu_fs_exists(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    int64_t mode = vayu_fs_stat_mode(p);
    free(p);
    return mode < 0 ? 0 : 1;
}
int64_t vayu_fs_is_file(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    int64_t mode = vayu_fs_stat_mode(p);
    free(p);
    if (mode < 0) return 0;
#ifdef _WIN32
    return (mode & _S_IFREG) ? 1 : 0;
#else
    return S_ISREG(mode) ? 1 : 0;
#endif
}
int64_t vayu_fs_is_dir(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    int64_t mode = vayu_fs_stat_mode(p);
    free(p);
    if (mode < 0) return 0;
#ifdef _WIN32
    return (mode & _S_IFDIR) ? 1 : 0;
#else
    return S_ISDIR(mode) ? 1 : 0;
#endif
}
int64_t vayu_fs_size(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p, &st) != 0) { free(p); return 0; }
#else
    struct stat st;
    if (stat(p, &st) != 0) { free(p); return 0; }
#endif
    int64_t sz = (int64_t)st.st_size;
    free(p);
    return sz;
}
VayuStr* vayu_fs_cwd(void) {
    char buf[4096];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf)) == NULL) return vayu_mkstr("", 0);
#else
    if (getcwd(buf, sizeof(buf)) == NULL) return vayu_mkstr("", 0);
#endif
    return vayu_mkstr_c(buf);
}
VayuStr* vayu_fs_abs(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
#ifdef _WIN32
    char full[4096];
    if (_fullpath(full, p, sizeof(full)) == NULL) { free(p); return vayu_mkstr("", 0); }
    free(p);
    return vayu_mkstr_c(full);
#else
    if (p[0] == '/') { VayuStr* r = vayu_mkstr_c(p); free(p); return r; }
    char* cwd_buf = getcwd(NULL, 0);
    if (!cwd_buf) { free(p); return vayu_mkstr("", 0); }
    size_t clen = strlen(cwd_buf);
    size_t plen = strlen(p);
    char* full = (char*)malloc(clen + 1 + plen + 1);
    memcpy(full, cwd_buf, clen);
    full[clen] = '/';
    memcpy(full + clen + 1, p, plen);
    full[clen + 1 + plen] = 0;
    free(cwd_buf); free(p);
    VayuStr* r = vayu_mkstr_c(full);
    free(full);
    return r;
#endif
}
VayuStr* vayu_fs_join(VayuStr* a, VayuStr* b) {
    if (a->len == 0) return vayu_mkstr(b->data, b->len);
    char last = a->data[a->len - 1];
    if (last == '/' || last == '\\') return vayu_str_concat(a, b);
#ifdef _WIN32
    return vayu_str_concat(vayu_str_concat(a, vayu_mkstr_c("\\")), b);
#else
    return vayu_str_concat(vayu_str_concat(a, vayu_mkstr_c("/")), b);
#endif
}
VayuStr* vayu_fs_extension(VayuStr* path) {
    int64_t dot = -1;
    int64_t i = path->len - 1;
    while (i >= 0) {
        char c = path->data[i];
        if (c == '.') { dot = i; break; }
        if (c == '/' || c == '\\') break;
        i = i - 1;
    }
    if (dot < 0) return vayu_mkstr("", 0);
    return vayu_mkstr(path->data + dot + 1, path->len - dot - 1);
}
VayuStr* vayu_fs_basename(VayuStr* path) {
    int64_t slash = -1;
    for (int64_t i = 0; i < path->len; ++i) {
        char c = path->data[i];
        if (c == '/' || c == '\\') slash = i;
    }
    return vayu_mkstr(path->data + slash + 1, path->len - slash - 1);
}
VayuStr* vayu_fs_dirname(VayuStr* path) {
    int64_t slash = -1;
    for (int64_t i = 0; i < path->len; ++i) {
        char c = path->data[i];
        if (c == '/' || c == '\\') slash = i;
    }
    if (slash < 0) return vayu_mkstr_c(".");
    if (slash == 0) return vayu_mkstr(path->data, 1);
    return vayu_mkstr(path->data, slash);
}
void vayu_fs_remove(VayuStr* path) { char* p = vayu_fs_cstr(path); remove(p); free(p); }
void vayu_fs_rename(VayuStr* a, VayuStr* b) {
    char* pa = vayu_fs_cstr(a);
    char* pb = vayu_fs_cstr(b);
    rename(pa, pb);
    free(pa); free(pb);
}
void vayu_fs_mkdir(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    int64_t n = (int64_t)strlen(p);
    for (int64_t i = 0; i < n; ++i) {
        if (p[i] == '/' || p[i] == '\\') {
            char save = p[i];
            p[i] = 0;
            if (p[0] != 0) {
#ifdef _WIN32
                _mkdir(p);
#else
                mkdir(p, 0755);
#endif
            }
            p[i] = save;
        }
    }
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
    free(p);
}
void vayu_fs_rmdir(VayuStr* path) {
    char* p = vayu_fs_cstr(path);
    char cmd[8192];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "cmd /c rmdir /s /q \"%s\" 2>nul", p);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" 2>/dev/null", p);
#endif
    system(cmd);
    free(p);
}
VayuList* vayu_fs_read_dir(VayuStr* path) {
    VayuList* lst = vayu_list_new();
    char* p = vayu_fs_cstr(path);
    DIR* d = opendir(p);
    if (!d) { free(p); return lst; }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* n = ent->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        vayu_list_push(lst, (int64_t)vayu_mkstr_c(n));
    }
    closedir(d);
    free(p);
    return lst;
}

// ---- time ----
int64_t vayu_time_now(void) { return (int64_t)time(NULL); }
int64_t vayu_time_now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER ui;
    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return (int64_t)((ui.QuadPart / 10000ULL) - 11644473600000LL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}
void vayu_time_sleep(int64_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}
VayuStr* vayu_time_format(int64_t unix_secs, VayuStr* fmt) {
    char fmtbuf[256];
    int64_t n = fmt->len < 255 ? fmt->len : 255;
    memcpy(fmtbuf, fmt->data, (size_t)n);
    fmtbuf[n] = 0;
    time_t t = (time_t)unix_secs;
    struct tm tmv;
#ifdef _WIN32
    if (localtime_s(&tmv, &t) != 0) return vayu_mkstr("", 0);
#else
    if (localtime_r(&t, &tmv) == NULL) return vayu_mkstr("", 0);
#endif
    char out[512];
    size_t len = strftime(out, sizeof(out), fmtbuf, &tmv);
    return vayu_mkstr(out, (int64_t)len);
}

// ---- json ----
typedef struct VayuJsonValue {
    int32_t tag;
    int32_t pad;
    int64_t data;
} VayuJsonValue;
typedef struct { const char* s; int64_t n; int64_t i; } JsonParser;
static VayuJsonValue* jp_new(int32_t tag, int64_t data) {
    VayuJsonValue* v = (VayuJsonValue*)malloc(sizeof(VayuJsonValue));
    v->tag = tag; v->pad = 0; v->data = data;
    return v;
}
static void jp_skip_ws(JsonParser* p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}
static VayuJsonValue* jp_parse_value(JsonParser* p);
static VayuJsonValue* jp_parse_string(JsonParser* p) {
    p->i++;
    size_t cap = 32, len = 0;
    char* buf = (char*)malloc(cap);
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') { p->i++; break; }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) break;
            char e = p->s[p->i++];
            if (e == 'u') {
                if (p->i + 4 > p->n) continue;
                unsigned int cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = p->s[p->i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                }
                if (len + 5 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                if (cp < 0x80) buf[len++] = (char)cp;
                else if (cp < 0x800) {
                    buf[len++] = (char)(0xC0 | (cp >> 6));
                    buf[len++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    buf[len++] = (char)(0xE0 | (cp >> 12));
                    buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[len++] = (char)(0x80 | (cp & 0x3F));
                }
                continue;
            }
            char out;
            switch (e) {
                case 'n': out = '\n'; break;
                case 't': out = '\t'; break;
                case 'r': out = '\r'; break;
                case 'b': out = '\b'; break;
                case 'f': out = '\f'; break;
                case '"': out = '"'; break;
                case '\\': out = '\\'; break;
                case '/': out = '/'; break;
                default:  out = e; break;
            }
            if (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            buf[len++] = out;
            continue;
        }
        if (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[len++] = (char)c;
        p->i++;
    }
    VayuStr* s = vayu_mkstr(buf, (int64_t)len);
    free(buf);
    return jp_new(3, (int64_t)s);
}
static VayuJsonValue* jp_parse_number(JsonParser* p) {
    int neg = 0;
    if (p->i < p->n && p->s[p->i] == '-') { neg = 1; p->i++; }
    int64_t n = 0;
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
        n = n * 10 + (p->s[p->i] - '0');
        p->i++;
    }
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    if (neg) n = -n;
    return jp_new(2, n);
}
static VayuJsonValue* jp_parse_array(JsonParser* p) {
    p->i++;
    VayuList* lst = vayu_list_new();
    jp_skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') {
        p->i++;
        return jp_new(4, (int64_t)lst);
    }
    while (p->i < p->n) {
        jp_skip_ws(p);
        VayuJsonValue* v = jp_parse_value(p);
        vayu_list_push(lst, (int64_t)v);
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
        break;
    }
    return jp_new(4, (int64_t)lst);
}
static VayuJsonValue* jp_parse_object(JsonParser* p) {
    p->i++;
    VayuMap* m = vayu_map_new();
    jp_skip_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') {
        p->i++;
        return jp_new(5, (int64_t)m);
    }
    while (p->i < p->n) {
        jp_skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != '"') break;
        VayuJsonValue* k = jp_parse_string(p);
        VayuStr* key = (VayuStr*)k->data;
        free(k);
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ':') p->i++;
        jp_skip_ws(p);
        VayuJsonValue* v = jp_parse_value(p);
        vayu_map_put(m, key, (int64_t)v);
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
        break;
    }
    return jp_new(5, (int64_t)m);
}
static VayuJsonValue* jp_parse_value(JsonParser* p) {
    jp_skip_ws(p);
    if (p->i >= p->n) return jp_new(0, 0);
    char c = p->s[p->i];
    if (c == 'n' && p->i + 4 <= p->n && strncmp(p->s + p->i, "null", 4) == 0) {
        p->i += 4; return jp_new(0, 0);
    }
    if (c == 't' && p->i + 4 <= p->n && strncmp(p->s + p->i, "true", 4) == 0) {
        p->i += 4; return jp_new(1, 1);
    }
    if (c == 'f' && p->i + 5 <= p->n && strncmp(p->s + p->i, "false", 5) == 0) {
        p->i += 5; return jp_new(1, 0);
    }
    if (c == '"') return jp_parse_string(p);
    if (c == '[') return jp_parse_array(p);
    if (c == '{') return jp_parse_object(p);
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(p);
    return jp_new(0, 0);
}
VayuJsonValue* vayu_json_parse(VayuStr* s) {
    JsonParser p;
    p.s = s->data;
    p.n = s->len;
    p.i = 0;
    return jp_parse_value(&p);
}
static void vayu_json_stringify_to(VayuJsonValue* v, VayuList* chunks) {
    if (!v) { vayu_list_push(chunks, (int64_t)vayu_mkstr_c("null")); return; }
    if (v->tag == 0) { vayu_list_push(chunks, (int64_t)vayu_mkstr_c("null")); return; }
    if (v->tag == 1) {
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c(v->data ? "true" : "false"));
        return;
    }
    if (v->tag == 2) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lld", (long long)v->data);
        vayu_list_push(chunks, (int64_t)vayu_mkstr(buf, n));
        return;
    }
    if (v->tag == 3) {
        VayuStr* s = (VayuStr*)v->data;
        size_t cap = (size_t)s->len + 16, len = 0;
        char* buf = (char*)malloc(cap);
        buf[len++] = '"';
        for (int64_t i = 0; i < s->len; i++) {
            unsigned char c = (unsigned char)s->data[i];
            const char* esc = NULL;
            switch (c) {
                case '"':  esc = "\\\""; break;
                case '\\': esc = "\\\\"; break;
                case '\n': esc = "\\n";  break;
                case '\r': esc = "\\r";  break;
                case '\t': esc = "\\t";  break;
                case '\b': esc = "\\b";  break;
                case '\f': esc = "\\f";  break;
            }
            if (esc) {
                while (len + 4 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                buf[len++] = esc[0]; buf[len++] = esc[1];
            } else if (c < 0x20) {
                while (len + 8 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                len += (size_t)snprintf(buf + len, cap - len, "\\u%04x", c);
            } else {
                while (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
                buf[len++] = (char)c;
            }
        }
        buf[len++] = '"';
        VayuStr* out = vayu_mkstr(buf, (int64_t)len);
        free(buf);
        vayu_list_push(chunks, (int64_t)out);
        return;
    }
    if (v->tag == 4) {
        VayuList* lst = (VayuList*)v->data;
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c("["));
        for (int64_t i = 0; i < lst->len; i++) {
            if (i) vayu_list_push(chunks, (int64_t)vayu_mkstr_c(","));
            vayu_json_stringify_to((VayuJsonValue*)lst->items[i], chunks);
        }
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c("]"));
        return;
    }
    if (v->tag == 5) {
        VayuMap* m = (VayuMap*)v->data;
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c("{"));
        int64_t printed = 0;
        for (int64_t i = 0; i < m->cap; i++) {
            if (!m->entries[i].used) continue;
            if (printed) vayu_list_push(chunks, (int64_t)vayu_mkstr_c(","));
            VayuJsonValue tmp;
            tmp.tag = 3; tmp.pad = 0; tmp.data = (int64_t)m->entries[i].key;
            vayu_json_stringify_to(&tmp, chunks);
            vayu_list_push(chunks, (int64_t)vayu_mkstr_c(":"));
            vayu_json_stringify_to((VayuJsonValue*)m->entries[i].value, chunks);
            printed++;
        }
        vayu_list_push(chunks, (int64_t)vayu_mkstr_c("}"));
        return;
    }
    vayu_list_push(chunks, (int64_t)vayu_mkstr_c("null"));
}
VayuStr* vayu_json_stringify(VayuJsonValue* v) {
    VayuList* chunks = vayu_list_new();
    vayu_json_stringify_to(v, chunks);
    int64_t total = 0;
    for (int64_t i = 0; i < chunks->len; i++)
        total += ((VayuStr*)chunks->items[i])->len;
    VayuStr* out = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)total + 1);
    out->len = total;
    int64_t op = 0;
    for (int64_t i = 0; i < chunks->len; i++) {
        VayuStr* c = (VayuStr*)chunks->items[i];
        memcpy(out->data + op, c->data, (size_t)c->len);
        op += c->len;
    }
    out->data[total] = 0;
    return out;
}
VayuJsonValue* vayu_json_get(VayuJsonValue* v, VayuStr* key) {
    if (!v || v->tag != 5) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.get: value is not an object"));
    }
    VayuMapEntry* e = map_find((VayuMap*)v->data, key);
    if (!e) vayu_raise_str(vayu_mkstr_c("KeyError"), key);
    return (VayuJsonValue*)e->value;
}
VayuJsonValue* vayu_json_index(VayuJsonValue* v, int64_t i) {
    if (!v || v->tag != 4) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.index: value is not an array"));
    }
    VayuList* lst = (VayuList*)v->data;
    if (i < 0) i += lst->len;
    if (i < 0 || i >= lst->len) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("json.index: index out of range"));
    }
    return (VayuJsonValue*)lst->items[i];
}
int64_t vayu_json_as_int(VayuJsonValue* v) {
    if (!v || v->tag != 2) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.as_int: value is not a number"));
    }
    return v->data;
}
VayuStr* vayu_json_as_str(VayuJsonValue* v) {
    if (!v || v->tag != 3) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.as_str: value is not a string"));
    }
    return (VayuStr*)v->data;
}
int64_t vayu_json_as_bool(VayuJsonValue* v) {
    if (!v || v->tag != 1) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("json.as_bool: value is not a boolean"));
    }
    return v->data;
}
int64_t vayu_json_len(VayuJsonValue* v) {
    if (!v) return 0;
    if (v->tag == 4) return ((VayuList*)v->data)->len;
    if (v->tag == 5) return ((VayuMap*)v->data)->len;
    return 0;
}
int64_t vayu_json_has(VayuJsonValue* v, VayuStr* key) {
    if (!v || v->tag != 5) return 0;
    return map_find((VayuMap*)v->data, key) != NULL ? 1 : 0;
}
VayuStr* vayu_json_type(VayuJsonValue* v) {
    if (!v) return vayu_mkstr_c("null");
    if (v->tag == 0) return vayu_mkstr_c("null");
    if (v->tag == 1) return vayu_mkstr_c("bool");
    if (v->tag == 2) return vayu_mkstr_c("int");
    if (v->tag == 3) return vayu_mkstr_c("str");
    if (v->tag == 4) return vayu_mkstr_c("list");
    if (v->tag == 5) return vayu_mkstr_c("map");
    return vayu_mkstr_c("?");
}
int64_t vayu_json_is_null(VayuJsonValue* v) { return (!v || v->tag == 0) ? 1 : 0; }
int64_t vayu_json_is_int (VayuJsonValue* v) { return (v && v->tag == 2) ? 1 : 0; }
int64_t vayu_json_is_str (VayuJsonValue* v) { return (v && v->tag == 3) ? 1 : 0; }
int64_t vayu_json_is_bool(VayuJsonValue* v) { return (v && v->tag == 1) ? 1 : 0; }
int64_t vayu_json_is_list(VayuJsonValue* v) { return (v && v->tag == 4) ? 1 : 0; }
int64_t vayu_json_is_map (VayuJsonValue* v) { return (v && v->tag == 5) ? 1 : 0; }
VayuList* vayu_json_keys(VayuJsonValue* v) {
    if (!v || v->tag != 5) return vayu_list_new();
    VayuMap* m = (VayuMap*)v->data;
    VayuList* out = vayu_list_new();
    for (int64_t i = 0; i < m->cap; i++) {
        if (!m->entries[i].used) continue;
        vayu_list_push(out, (int64_t)m->entries[i].key);
    }
    return out;
}
VayuJsonValue* vayu_json_make_null(void)          { return jp_new(0, 0); }
VayuJsonValue* vayu_json_make_bool(int64_t b)     { return jp_new(1, b ? 1 : 0); }
VayuJsonValue* vayu_json_make_int (int64_t n)     { return jp_new(2, n); }
VayuJsonValue* vayu_json_make_str (VayuStr* s)    { return jp_new(3, (int64_t)s); }

// ---- regex ----
typedef struct {
    const char* pat; int64_t plen;
    const char* txt; int64_t tlen;
} VayuRx;
static int vayu_rx_match_one(VayuRx* r, int64_t pi, int64_t ti,
                             int64_t* adv_pi, int64_t* adv_ti) {
    if (pi >= r->plen) return 0;
    if (ti >= r->tlen) return 0;
    char pc = r->pat[pi];
    char tc = r->txt[ti];
    if (pc == '.') { *adv_pi = pi + 1; *adv_ti = ti + 1; return 1; }
    if (pc == '\\') {
        if (pi + 1 >= r->plen) return 0;
        char e = r->pat[pi + 1];
        int ok = 0;
        if (e == 'd') ok = (tc >= '0' && tc <= '9');
        else if (e == 'D') ok = !(tc >= '0' && tc <= '9');
        else if (e == 'w') ok = (tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z')
                              || (tc >= '0' && tc <= '9') || tc == '_';
        else if (e == 'W') ok = !((tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z')
                              || (tc >= '0' && tc <= '9') || tc == '_');
        else if (e == 's') ok = (tc == ' ' || tc == '\t' || tc == '\n' ||
                                 tc == '\r' || tc == '\f' || tc == '\v');
        else if (e == 'S') ok = !(tc == ' ' || tc == '\t' || tc == '\n' ||
                                  tc == '\r' || tc == '\f' || tc == '\v');
        else ok = (tc == e);
        if (!ok) return 0;
        *adv_pi = pi + 2; *adv_ti = ti + 1;
        return 1;
    }
    if (pc == '[') {
        int64_t i = pi + 1;
        int neg = 0;
        if (i < r->plen && r->pat[i] == '^') { neg = 1; i++; }
        int found = 0;
        while (i < r->plen && r->pat[i] != ']') {
            if (r->pat[i] == '\\' && i + 1 < r->plen) {
                char e = r->pat[i + 1];
                int hit = 0;
                if (e == 'd') hit = (tc >= '0' && tc <= '9');
                else if (e == 'w') hit = (tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z')
                                      || (tc >= '0' && tc <= '9') || tc == '_';
                else if (e == 's') hit = (tc == ' ' || tc == '\t' || tc == '\n' || tc == '\r');
                else hit = (tc == e);
                if (hit) found = 1;
                i += 2;
                continue;
            }
            if (i + 2 < r->plen && r->pat[i + 1] == '-' && r->pat[i + 2] != ']') {
                char lo = r->pat[i];
                char hi = r->pat[i + 2];
                if (tc >= lo && tc <= hi) found = 1;
                i += 3;
                continue;
            }
            if (tc == r->pat[i]) found = 1;
            i++;
        }
        if (i >= r->plen || r->pat[i] != ']') return 0;
        if (neg) found = !found;
        if (!found) return 0;
        *adv_pi = i + 1; *adv_ti = ti + 1;
        return 1;
    }
    if (tc != pc) return 0;
    *adv_pi = pi + 1; *adv_ti = ti + 1;
    return 1;
}
static int64_t vayu_rx_atom_end(VayuRx* r, int64_t pi) {
    char c = r->pat[pi];
    if (c == '\\') return pi + 2;
    if (c == '[') {
        int64_t i = pi + 1;
        if (i < r->plen && r->pat[i] == '^') i++;
        while (i < r->plen && r->pat[i] != ']') {
            if (r->pat[i] == '\\') i += 2; else i++;
        }
        return i + 1;
    }
    return pi + 1;
}
static int vayu_rx_here(VayuRx* r, int64_t pi, int64_t ti, int64_t* out_end) {
    if (pi >= r->plen) { *out_end = ti; return 1; }
    char c = r->pat[pi];
    if (c == '^') {
        if (ti != 0) return 0;
        return vayu_rx_here(r, pi + 1, ti, out_end);
    }
    if (c == '$') {
        if (ti != r->tlen) return 0;
        return vayu_rx_here(r, pi + 1, ti, out_end);
    }
    int64_t atom_end = vayu_rx_atom_end(r, pi);
    char q = (atom_end < r->plen) ? r->pat[atom_end] : 0;
    if (q == '*' || q == '+') {
        int64_t cur_pi = atom_end + 1;
        int64_t positions[2048];
        int count = 0;
        positions[count++] = ti;
        int64_t tp = ti;
        while (count < 2048) {
            int64_t np = 0, nt = 0;
            if (!vayu_rx_match_one(r, pi, tp, &np, &nt)) break;
            tp = nt;
            positions[count++] = tp;
        }
        int min_matches = (q == '+') ? 1 : 0;
        for (int k = count - 1; k >= min_matches; k--) {
            if (vayu_rx_here(r, cur_pi, positions[k], out_end)) return 1;
        }
        return 0;
    }
    if (q == '?') {
        int64_t cur_pi = atom_end + 1;
        int64_t np = 0, nt = 0;
        if (vayu_rx_match_one(r, pi, ti, &np, &nt)) {
            if (vayu_rx_here(r, cur_pi, nt, out_end)) return 1;
        }
        return vayu_rx_here(r, cur_pi, ti, out_end);
    }
    int64_t np = 0, nt = 0;
    if (!vayu_rx_match_one(r, pi, ti, &np, &nt)) return 0;
    return vayu_rx_here(r, np, nt, out_end);
}
static int vayu_rx_find(VayuRx* r, int64_t from, int64_t* ms, int64_t* me) {
    int64_t start = from;
    while (start <= r->tlen) {
        int64_t end = 0;
        if (vayu_rx_here(r, 0, start, &end)) {
            *ms = start; *me = end;
            return 1;
        }
        start++;
    }
    return 0;
}
int64_t vayu_regex_match(VayuStr* pat, VayuStr* s) {
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    int64_t end = 0;
    if (!vayu_rx_here(&r, 0, 0, &end)) return 0;
    return end == s->len ? 1 : 0;
}
int64_t vayu_regex_search(VayuStr* pat, VayuStr* s) {
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    int64_t ms = 0, me = 0;
    if (!vayu_rx_find(&r, 0, &ms, &me)) return -1;
    return ms;
}
VayuList* vayu_regex_find_all(VayuStr* pat, VayuStr* s) {
    VayuList* out = vayu_list_new();
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    int64_t from = 0;
    while (from <= r.tlen) {
        int64_t ms = 0, me = 0;
        if (!vayu_rx_find(&r, from, &ms, &me)) break;
        vayu_list_push(out, (int64_t)vayu_mkstr(s->data + ms, me - ms));
        from = (me > ms) ? me : me + 1;
    }
    return out;
}
VayuStr* vayu_regex_replace(VayuStr* pat, VayuStr* s, VayuStr* repl) {
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    VayuList* chunks = vayu_list_new();
    int64_t pos = 0;
    int64_t from = 0;
    while (from <= r.tlen) {
        int64_t ms = 0, me = 0;
        if (!vayu_rx_find(&r, from, &ms, &me)) break;
        if (ms > pos)
            vayu_list_push(chunks, (int64_t)vayu_mkstr(s->data + pos, ms - pos));
        vayu_list_push(chunks, (int64_t)vayu_mkstr(repl->data, repl->len));
        pos = me;
        from = (me > ms) ? me : me + 1;
    }
    if (pos < s->len)
        vayu_list_push(chunks, (int64_t)vayu_mkstr(s->data + pos, s->len - pos));
    int64_t total = 0;
    for (int64_t i = 0; i < chunks->len; i++)
        total += ((VayuStr*)chunks->items[i])->len;
    VayuStr* out = (VayuStr*)malloc(sizeof(VayuStr) + (size_t)total + 1);
    out->len = total;
    int64_t op = 0;
    for (int64_t i = 0; i < chunks->len; i++) {
        VayuStr* c = (VayuStr*)chunks->items[i];
        memcpy(out->data + op, c->data, (size_t)c->len);
        op += c->len;
    }
    out->data[total] = 0;
    return out;
}
VayuList* vayu_regex_split(VayuStr* pat, VayuStr* s) {
    VayuList* out = vayu_list_new();
    VayuRx r;
    r.pat = pat->data; r.plen = pat->len;
    r.txt = s->data;   r.tlen = s->len;
    int64_t pos = 0;
    int64_t from = 0;
    while (from <= r.tlen) {
        int64_t ms = 0, me = 0;
        if (!vayu_rx_find(&r, from, &ms, &me)) break;
        vayu_list_push(out, (int64_t)vayu_mkstr(s->data + pos, ms - pos));
        pos = me;
        from = (me > ms) ? me : me + 1;
    }
    vayu_list_push(out, (int64_t)vayu_mkstr(s->data + pos, s->len - pos));
    return out;
}

// ---- thread ----
typedef int64_t (*vayu_thread_fn_t)(int64_t);
typedef struct {
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t tid;
#endif
    int64_t arg;
    int64_t result;
    vayu_thread_fn_t fn;
} VayuThread;
#ifdef _WIN32
static DWORD WINAPI vayu_thread_win_proc(LPVOID p) {
    VayuThread* t = (VayuThread*)p;
    t->result = t->fn(t->arg);
    return 0;
}
#else
static void* vayu_thread_posix_proc(void* p) {
    VayuThread* t = (VayuThread*)p;
    t->result = t->fn(t->arg);
    return NULL;
}
#endif
int64_t vayu_thread_spawn(void* fn, int64_t arg) {
    VayuThread* t = (VayuThread*)malloc(sizeof(VayuThread));
    t->fn = (vayu_thread_fn_t)fn;
    t->arg = arg;
    t->result = 0;
#ifdef _WIN32
    t->handle = CreateThread(NULL, 0, vayu_thread_win_proc, t, 0, NULL);
    if (!t->handle) { free(t); return 0; }
#else
    if (pthread_create(&t->tid, NULL, vayu_thread_posix_proc, t) != 0) {
        free(t); return 0;
    }
#endif
    return (int64_t)t;
}
int64_t vayu_thread_join(int64_t handle) {
    VayuThread* t = (VayuThread*)handle;
    if (!t) return 0;
#ifdef _WIN32
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
#else
    pthread_join(t->tid, NULL);
#endif
    int64_t r = t->result;
    free(t);
    return r;
}
int64_t vayu_thread_id(void) {
#ifdef _WIN32
    return (int64_t)GetCurrentThreadId();
#else
    return (int64_t)(uintptr_t)pthread_self();
#endif
}
typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
} VayuMutex;
int64_t vayu_mutex_new(void) {
    VayuMutex* m = (VayuMutex*)malloc(sizeof(VayuMutex));
#ifdef _WIN32
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutex_init(&m->mtx, NULL);
#endif
    return (int64_t)m;
}
void vayu_mutex_lock(int64_t h) {
    VayuMutex* m = (VayuMutex*)h;
#ifdef _WIN32
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->mtx);
#endif
}
void vayu_mutex_unlock(int64_t h) {
    VayuMutex* m = (VayuMutex*)h;
#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif
}

// ---- net ----
#ifdef _WIN32
typedef SOCKET vayu_socket_t;
#  define VAYU_INVALID_SOCKET INVALID_SOCKET
#  define VAYU_CLOSE_SOCKET   closesocket
static int vayu_net_init_done = 0;
static int vayu_net_init(void) {
    if (vayu_net_init_done) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    vayu_net_init_done = 1;
    return 0;
}
#else
typedef int vayu_socket_t;
#  define VAYU_INVALID_SOCKET (-1)
#  define VAYU_CLOSE_SOCKET   close
static int vayu_net_init(void) { return 0; }
#endif
int64_t vayu_net_listen(int64_t port) {
    if (vayu_net_init() != 0) return 0;
    vayu_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == VAYU_INVALID_SOCKET) return 0;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        VAYU_CLOSE_SOCKET(s); return 0;
    }
    if (listen(s, 16) != 0) {
        VAYU_CLOSE_SOCKET(s); return 0;
    }
    return (int64_t)s;
}
int64_t vayu_net_server_port(int64_t h) {
    vayu_socket_t s = (vayu_socket_t)h;
    struct sockaddr_in addr;
#ifdef _WIN32
    int alen = sizeof(addr);
#else
    socklen_t alen = sizeof(addr);
#endif
    if (getsockname(s, (struct sockaddr*)&addr, &alen) != 0) return 0;
    return (int64_t)ntohs(addr.sin_port);
}
int64_t vayu_net_accept(int64_t srv) {
    vayu_socket_t s = (vayu_socket_t)srv;
    vayu_socket_t c = accept(s, NULL, NULL);
    if (c == VAYU_INVALID_SOCKET) return 0;
    return (int64_t)c;
}
int64_t vayu_net_connect(VayuStr* host, int64_t port) {
    if (vayu_net_init() != 0) return 0;
    vayu_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == VAYU_INVALID_SOCKET) return 0;
    char hostbuf[256];
    int64_t n = host->len < 255 ? host->len : 255;
    memcpy(hostbuf, host->data, (size_t)n);
    hostbuf[n] = 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, hostbuf, &addr.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hostbuf, NULL, &hints, &res) != 0 || !res) {
            VAYU_CLOSE_SOCKET(s); return 0;
        }
        addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        VAYU_CLOSE_SOCKET(s); return 0;
    }
    return (int64_t)s;
}
int64_t vayu_net_send(int64_t h, VayuStr* s) {
    vayu_socket_t sock = (vayu_socket_t)h;
    int64_t sent = 0;
    while (sent < s->len) {
        int n = send(sock, s->data + sent, (int)(s->len - sent), 0);
        if (n <= 0) return sent;
        sent += n;
    }
    return sent;
}
VayuStr* vayu_net_recv(int64_t h, int64_t maxlen) {
    vayu_socket_t sock = (vayu_socket_t)h;
    if (maxlen <= 0) maxlen = 4096;
    if (maxlen > 65536) maxlen = 65536;
    char* buf = (char*)malloc((size_t)maxlen);
    int n = recv(sock, buf, (int)maxlen, 0);
    if (n <= 0) { free(buf); return vayu_mkstr("", 0); }
    VayuStr* r = vayu_mkstr(buf, n);
    free(buf);
    return r;
}
int64_t vayu_net_send_line(int64_t h, VayuStr* s) {
    int64_t r = vayu_net_send(h, s);
    vayu_socket_t sock = (vayu_socket_t)h;
    send(sock, "\n", 1, 0);
    return r;
}
VayuStr* vayu_net_recv_line(int64_t h) {
    vayu_socket_t sock = (vayu_socket_t)h;
    size_t cap = 128, len = 0;
    char* buf = (char*)malloc(cap);
    char c;
    while (1) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[len++] = c;
    }
    VayuStr* r = vayu_mkstr(buf, (int64_t)len);
    free(buf);
    return r;
}
void vayu_net_close(int64_t h) {
    if (!h) return;
    vayu_socket_t s = (vayu_socket_t)h;
    VAYU_CLOSE_SOCKET(s);
}

// ---- crypto ----
static VayuStr* vayu_hex(const uint8_t* b, int64_t n) {
    static const char hx[] = "0123456789abcdef";
    char* buf = (char*)malloc((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) {
        buf[i*2]   = hx[(b[i] >> 4) & 0xf];
        buf[i*2+1] = hx[b[i] & 0xf];
    }
    VayuStr* r = vayu_mkstr(buf, n * 2);
    free(buf);
    return r;
}
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
} VayuSHA256Ctx;
static const uint32_t vayu_sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define VAYU_ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void vayu_sha256_transform(VayuSHA256Ctx* ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i, j;
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | (uint32_t)data[j+3];
    for (; i < 64; ++i) {
        uint32_t s0 = VAYU_ROTR32(m[i-15], 7) ^ VAYU_ROTR32(m[i-15], 18) ^ (m[i-15] >> 3);
        uint32_t s1 = VAYU_ROTR32(m[i-2], 17) ^ VAYU_ROTR32(m[i-2], 19) ^ (m[i-2] >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        uint32_t S1 = VAYU_ROTR32(e, 6) ^ VAYU_ROTR32(e, 11) ^ VAYU_ROTR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + vayu_sha256_k[i] + m[i];
        uint32_t S0 = VAYU_ROTR32(a, 2) ^ VAYU_ROTR32(a, 13) ^ VAYU_ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}
static void vayu_sha256_init(VayuSHA256Ctx* ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}
static void vayu_sha256_update(VayuSHA256Ctx* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            vayu_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}
static void vayu_sha256_final(VayuSHA256Ctx* ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0;
        vayu_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    vayu_sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4]  = (uint8_t)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8]  = (uint8_t)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = (uint8_t)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = (uint8_t)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = (uint8_t)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = (uint8_t)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}
typedef struct {
    uint32_t state[4];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
} VayuMD5Ctx;
static const uint32_t vayu_md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
    0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
    0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
    0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
    0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
    0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const uint32_t vayu_md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};
#define VAYU_ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void vayu_md5_transform(VayuMD5Ctx* ctx, const uint8_t data[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = (uint32_t)data[i*4] | ((uint32_t)data[i*4+1] << 8) |
               ((uint32_t)data[i*4+2] << 16) | ((uint32_t)data[i*4+3] << 24);
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) { f = (b & c) | ((~b) & d); g = i; }
        else if (i < 32) { f = (d & b) | ((~d) & c); g = (5*i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) % 16; }
        else { f = c ^ (b | (~d)); g = (7*i) % 16; }
        uint32_t tmp = d;
        d = c; c = b;
        b = b + VAYU_ROTL32(a + f + vayu_md5_k[i] + m[g], vayu_md5_s[i]);
        a = tmp;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
}
static void vayu_md5_init(VayuMD5Ctx* ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}
static void vayu_md5_update(VayuMD5Ctx* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            vayu_md5_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}
static void vayu_md5_final(VayuMD5Ctx* ctx, uint8_t hash[16]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0;
        vayu_md5_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[56] = (uint8_t)(ctx->bitlen);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[63] = (uint8_t)(ctx->bitlen >> 56);
    vayu_md5_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)((ctx->state[0] >> (i * 8)) & 0xff);
        hash[i + 4]  = (uint8_t)((ctx->state[1] >> (i * 8)) & 0xff);
        hash[i + 8]  = (uint8_t)((ctx->state[2] >> (i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (i * 8)) & 0xff);
    }
}
VayuStr* vayu_crypto_sha256(VayuStr* s) {
    VayuSHA256Ctx ctx;
    vayu_sha256_init(&ctx);
    vayu_sha256_update(&ctx, (const uint8_t*)s->data, (size_t)s->len);
    uint8_t h[32];
    vayu_sha256_final(&ctx, h);
    return vayu_hex(h, 32);
}
VayuStr* vayu_crypto_md5(VayuStr* s) {
    VayuMD5Ctx ctx;
    vayu_md5_init(&ctx);
    vayu_md5_update(&ctx, (const uint8_t*)s->data, (size_t)s->len);
    uint8_t h[16];
    vayu_md5_final(&ctx, h);
    return vayu_hex(h, 16);
}
VayuStr* vayu_crypto_hmac_sha256(VayuStr* key, VayuStr* msg) {
    uint8_t k[64];
    memset(k, 0, 64);
    if (key->len > 64) {
        VayuSHA256Ctx kctx;
        vayu_sha256_init(&kctx);
        vayu_sha256_update(&kctx, (const uint8_t*)key->data, (size_t)key->len);
        vayu_sha256_final(&kctx, k);
    } else {
        memcpy(k, key->data, (size_t)key->len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    VayuSHA256Ctx c;
    uint8_t inner[32], out[32];
    vayu_sha256_init(&c);
    vayu_sha256_update(&c, ipad, 64);
    vayu_sha256_update(&c, (const uint8_t*)msg->data, (size_t)msg->len);
    vayu_sha256_final(&c, inner);
    vayu_sha256_init(&c);
    vayu_sha256_update(&c, opad, 64);
    vayu_sha256_update(&c, inner, 32);
    vayu_sha256_final(&c, out);
    return vayu_hex(out, 32);
}
static const char vayu_b64e[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
VayuStr* vayu_crypto_base64_encode(VayuStr* s) {
    int64_t n = s->len;
    int64_t outLen = ((n + 2) / 3) * 4;
    char* out = (char*)malloc((size_t)outLen + 1);
    int64_t i = 0, o = 0;
    while (i + 2 < n) {
        uint32_t v = ((uint8_t)s->data[i] << 16) |
                     ((uint8_t)s->data[i+1] << 8) |
                     ((uint8_t)s->data[i+2]);
        out[o++] = vayu_b64e[(v >> 18) & 0x3F];
        out[o++] = vayu_b64e[(v >> 12) & 0x3F];
        out[o++] = vayu_b64e[(v >> 6) & 0x3F];
        out[o++] = vayu_b64e[v & 0x3F];
        i += 3;
    }
    if (i < n) {
        uint32_t v = ((uint8_t)s->data[i]) << 16;
        if (i + 1 < n) v |= ((uint8_t)s->data[i+1]) << 8;
        out[o++] = vayu_b64e[(v >> 18) & 0x3F];
        out[o++] = vayu_b64e[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? vayu_b64e[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    VayuStr* r = vayu_mkstr(out, o);
    free(out);
    return r;
}
static int vayu_b64_dec_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
VayuStr* vayu_crypto_base64_decode(VayuStr* s) {
    int64_t n = s->len;
    char* buf = (char*)malloc((size_t)n + 1);
    size_t len = 0;
    for (int64_t i = 0; i < n; ++i) {
        char c = s->data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        buf[len++] = c;
    }
    buf[len] = 0;
    size_t outCap = (len / 4) * 3 + 3;
    char* out = (char*)malloc(outCap);
    size_t oi = 0;
    size_t i = 0;
    while (i + 3 < len) {
        int v0 = vayu_b64_dec_char(buf[i]);
        int v1 = vayu_b64_dec_char(buf[i+1]);
        int v2 = (buf[i+2] == '=') ? -2 : vayu_b64_dec_char(buf[i+2]);
        int v3 = (buf[i+3] == '=') ? -2 : vayu_b64_dec_char(buf[i+3]);
        if (v0 < 0 || v1 < 0) break;
        uint32_t v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);
        if (v2 >= 0) v |= ((uint32_t)v2 << 6);
        if (v3 >= 0) v |= (uint32_t)v3;
        out[oi++] = (char)((v >> 16) & 0xff);
        if (v2 >= 0) out[oi++] = (char)((v >> 8) & 0xff);
        if (v3 >= 0) out[oi++] = (char)(v & 0xff);
        i += 4;
    }
    VayuStr* r = vayu_mkstr(out, oi);
    free(out); free(buf);
    return r;
}
VayuStr* vayu_crypto_random_bytes(int64_t n) {
    if (n <= 0) return vayu_mkstr("", 0);
    char* buf = (char*)malloc((size_t)n);
#ifdef _WIN32
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        for (int64_t i = 0; i < n; ++i) buf[i] = (char)(rand() & 0xff);
    }
#else
    {
        FILE* f = fopen("/dev/urandom", "rb");
        if (f) {
            size_t got = fread(buf, 1, (size_t)n, f);
            if (got < (size_t)n) {
                for (int64_t i = (int64_t)got; i < n; ++i) buf[i] = (char)(rand() & 0xff);
            }
            fclose(f);
        } else {
            for (int64_t i = 0; i < n; ++i) buf[i] = (char)(rand() & 0xff);
        }
    }
#endif
    VayuStr* r = vayu_mkstr(buf, n);
    free(buf);
    return r;
}

// ---- random ----
static int vayu_rand_seeded = 0;
void vayu_random_seed(int64_t s) { srand((unsigned)s); vayu_rand_seeded = 1; }
static void vayu_random_ensure_seed(void) {
    if (!vayu_rand_seeded) { srand((unsigned)time(NULL)); vayu_rand_seeded = 1; }
}
int64_t vayu_random_randint(int64_t lo, int64_t hi) {
    if (hi < lo) { int64_t t = lo; lo = hi; hi = t; }
    vayu_random_ensure_seed();
    int64_t span = hi - lo + 1;
    if (span <= 0) return lo;
    return lo + (int64_t)(rand() % span);
}
int64_t vayu_random_randrange(int64_t lo, int64_t hi) {
    if (hi <= lo) return lo;
    vayu_random_ensure_seed();
    return lo + (int64_t)(rand() % (hi - lo));
}
int64_t vayu_random_choice(VayuList* lst) {
    if (!lst || lst->len == 0) {
        vayu_raise_str(vayu_mkstr_c("IndexError"),
                       vayu_mkstr_c("random.choice: empty list"));
    }
    vayu_random_ensure_seed();
    return lst->items[rand() % lst->len];
}
void vayu_random_shuffle(VayuList* lst) {
    if (!lst || lst->len <= 1) return;
    vayu_random_ensure_seed();
    for (int64_t i = lst->len - 1; i > 0; --i) {
        int64_t j = rand() % (i + 1);
        int64_t t = lst->items[i];
        lst->items[i] = lst->items[j];
        lst->items[j] = t;
    }
}
VayuList* vayu_random_sample(VayuList* lst, int64_t k) {
    if (!lst || lst->len == 0 || k <= 0) return vayu_list_new();
    if (k > lst->len) k = lst->len;
    vayu_random_ensure_seed();
    int64_t* idx = (int64_t*)malloc(sizeof(int64_t) * (size_t)lst->len);
    for (int64_t i = 0; i < lst->len; ++i) idx[i] = i;
    for (int64_t i = lst->len - 1; i > 0; --i) {
        int64_t j = rand() % (i + 1);
        int64_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    VayuList* out = vayu_list_new();
    for (int64_t i = 0; i < k; ++i)
        vayu_list_push(out, lst->items[idx[i]]);
    free(idx);
    return out;
}

// ---- os ----
VayuStr* vayu_os_getenv(VayuStr* name) {
    char namebuf[256];
    int64_t n = name->len < 255 ? name->len : 255;
    memcpy(namebuf, name->data, (size_t)n);
    namebuf[n] = 0;
    const char* v = getenv(namebuf);
    if (!v) return vayu_mkstr("", 0);
    return vayu_mkstr_c(v);
}
void vayu_os_setenv(VayuStr* name, VayuStr* val) {
    char namebuf[256], valbuf[4096];
    int64_t n = name->len < 255 ? name->len : 255;
    int64_t m = val->len < 4095 ? val->len : 4095;
    memcpy(namebuf, name->data, (size_t)n); namebuf[n] = 0;
    memcpy(valbuf, val->data, (size_t)m);  valbuf[m] = 0;
#ifdef _WIN32
    _putenv_s(namebuf, valbuf);
#else
    setenv(namebuf, valbuf, 1);
#endif
}
VayuStr* vayu_os_platform(void) {
#ifdef _WIN32
    return vayu_mkstr_c("windows");
#elif defined(__APPLE__)
    return vayu_mkstr_c("macos");
#elif defined(__linux__)
    return vayu_mkstr_c("linux");
#else
    return vayu_mkstr_c("unknown");
#endif
}
VayuStr* vayu_os_hostname(void) {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0) return vayu_mkstr("", 0);
    buf[sizeof(buf) - 1] = 0;
    return vayu_mkstr_c(buf);
}
VayuStr* vayu_os_cwd(void) {
    char buf[4096];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf)) == NULL) return vayu_mkstr("", 0);
#else
    if (getcwd(buf, sizeof(buf)) == NULL) return vayu_mkstr("", 0);
#endif
    return vayu_mkstr_c(buf);
}
void vayu_os_chdir(VayuStr* path) {
    char buf[4096];
    int64_t n = path->len < 4095 ? path->len : 4095;
    memcpy(buf, path->data, (size_t)n);
    buf[n] = 0;
#ifdef _WIN32
    _chdir(buf);
#else
    chdir(buf);
#endif
}
void vayu_os_exit(int64_t code) { exit((int)code); }

// ---- Phase 11.1k4: native generators ----

typedef struct VayuGen {
#ifdef _WIN32
    HANDLE                worker;
    CRITICAL_SECTION      mtx;
    CONDITION_VARIABLE    cv;
#else
    pthread_t             worker;
    pthread_mutex_t       mtx;
    pthread_cond_t        cv;
#endif
    int                   state;   /* 0=fresh, 1=running, 2=suspended, 3=done */
    int                   resume;
    int                   cancel;
    int64_t               yielded;
    int                   has_error;
    VayuExc*              error;
    int64_t             (*fn)(int64_t);
    int64_t               arg;
} VayuGen;

static VAYU_THREAD_LOCAL VayuGen* tls_current_gen = NULL;

#ifdef _WIN32
#  define VG_LOCK(g)      EnterCriticalSection(&(g)->mtx)
#  define VG_UNLOCK(g)    LeaveCriticalSection(&(g)->mtx)
#  define VG_WAIT(g)      SleepConditionVariableCS(&(g)->cv, &(g)->mtx, INFINITE)
#  define VG_SIGNAL(g)    WakeAllConditionVariable(&(g)->cv)
#else
#  define VG_LOCK(g)      pthread_mutex_lock(&(g)->mtx)
#  define VG_UNLOCK(g)    pthread_mutex_unlock(&(g)->mtx)
#  define VG_WAIT(g)      pthread_cond_wait(&(g)->cv, &(g)->mtx)
#  define VG_SIGNAL(g)    pthread_cond_broadcast(&(g)->cv)
#endif

static void vayu_gen_worker_body(VayuGen* g) {
    tls_current_gen = g;

    VG_LOCK(g);
    while (!g->resume && !g->cancel) VG_WAIT(g);
    if (g->cancel) {
        g->state = 3;
        VG_SIGNAL(g);
        VG_UNLOCK(g);
        return;
    }
    g->resume = 0;
    g->state = 1;
    VG_UNLOCK(g);

    int fid = vayu_try_push();
    if (setjmp(g_jmpBufs[fid]) == 0) {
        g->fn(g->arg);
    } else {
        g->has_error = 1;
        g->error = g_excValue;
        g_excValue = NULL;
        vayu_try_pop();
    }

    VG_LOCK(g);
    g->state = 3;
    VG_SIGNAL(g);
    VG_UNLOCK(g);
}

#ifdef _WIN32
static DWORD WINAPI vayu_gen_worker_win(LPVOID p) {
    vayu_gen_worker_body((VayuGen*)p);
    return 0;
}
#else
static void* vayu_gen_worker_posix(void* p) {
    vayu_gen_worker_body((VayuGen*)p);
    return NULL;
}
#endif

VayuGen* vayu_gen_new(int64_t (*fn)(int64_t), int64_t arg) {
    VayuGen* g = (VayuGen*)malloc(sizeof(VayuGen));
#ifdef _WIN32
    InitializeCriticalSection(&g->mtx);
    InitializeConditionVariable(&g->cv);
#else
    pthread_mutex_init(&g->mtx, NULL);
    pthread_cond_init(&g->cv, NULL);
#endif
    g->state = 0;
    g->resume = 0;
    g->cancel = 0;
    g->yielded = 0;
    g->has_error = 0;
    g->error = NULL;
    g->fn = fn;
    g->arg = arg;
#ifdef _WIN32
    g->worker = CreateThread(NULL, 0, vayu_gen_worker_win, g, 0, NULL);
#else
    pthread_create(&g->worker, NULL, vayu_gen_worker_posix, g);
#endif
    return g;
}

int64_t vayu_gen_done(VayuGen* g) {
    if (!g) return 1;
    VG_LOCK(g);
    int64_t r = (g->state == 3) ? 1 : 0;
    VG_UNLOCK(g);
    return r;
}

int64_t vayu_gen_try_next(VayuGen* g, int64_t* out) {
    VG_LOCK(g);
    if (g->state == 3) {
        if (g->has_error) {
            VayuExc* err = g->error;
            g->has_error = 0;
            g->error = NULL;
            VG_UNLOCK(g);
            vayu_raise(err);
        }
        VG_UNLOCK(g);
        return 0;
    }
    if (g->state == 1) {
        VG_UNLOCK(g);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("generator already running"));
    }
    g->resume = 1;
    g->state = 1;
    VG_SIGNAL(g);
    while (g->state == 1) VG_WAIT(g);
    if (g->state == 3) {
        if (g->has_error) {
            VayuExc* err = g->error;
            g->has_error = 0;
            g->error = NULL;
            VG_UNLOCK(g);
            vayu_raise(err);
        }
        VG_UNLOCK(g);
        return 0;
    }
    *out = g->yielded;
    VG_UNLOCK(g);
    return 1;
}

int64_t vayu_gen_next(VayuGen* g) {
    VG_LOCK(g);
    if (g->state == 3) {
        int has_err = g->has_error;
        VayuExc* err = g->error;
        g->has_error = 0;
        g->error = NULL;
        VG_UNLOCK(g);
        if (has_err && err) vayu_raise(err);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("generator exhausted"));
    }
    if (g->state == 1) {
        VG_UNLOCK(g);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("generator already running"));
    }
    g->resume = 1;
    g->state = 1;
    VG_SIGNAL(g);
    while (g->state == 1) VG_WAIT(g);
    if (g->state == 3) {
        int has_err = g->has_error;
        VayuExc* err = g->error;
        g->has_error = 0;
        g->error = NULL;
        VG_UNLOCK(g);
        if (has_err && err) vayu_raise(err);
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("generator exhausted"));
    }
    int64_t v = g->yielded;
    VG_UNLOCK(g);
    return v;
}

void vayu_gen_yield(int64_t value) {
    VayuGen* g = tls_current_gen;
    if (!g) {
        vayu_raise_str(vayu_mkstr_c("RuntimeError"),
                       vayu_mkstr_c("'yield' outside generator"));
    }
    VG_LOCK(g);
    g->yielded = value;
    g->state = 2;
    VG_SIGNAL(g);
    while (!g->resume && !g->cancel) VG_WAIT(g);
    if (g->cancel) {
        VG_UNLOCK(g);
#ifdef _WIN32
        ExitThread(0);
#else
        pthread_exit(NULL);
#endif
        return;
    }
    g->resume = 0;
    g->state = 1;
    VG_UNLOCK(g);
}

extern void vayu_main(void);

int main(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
    vayu_main();
    return 0;
}
