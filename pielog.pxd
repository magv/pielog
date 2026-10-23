# Cython declarations for pielog's C API (pielog/pielog.h).

from cpython.pycapsule cimport PyCapsule_GetPointer

cdef extern from "pielog.h":
    ctypedef struct pielog_span:
        unsigned int proc
        unsigned int rec
        unsigned int gen

    ctypedef struct Pielog:
        int abi_version
        void (*pr)(pielog_span s, const char *text, size_t len)
        pielog_span (*begin)(pielog_span s, const char *text, size_t len)
        pielog_span (*end)(pielog_span s)
        pielog_span (*log_begin)(pielog_span s, const char *text, size_t len)
        pielog_span (*log_end)(pielog_span s)
        void (*set_text)(pielog_span s, const char *text, size_t len)
        void (*set_progress)(pielog_span s, double done, double total)
        pielog_span (*here)()
        void (*set)(pielog_span s)

    Pielog *pielog_get()
    pielog_span pielog_here()
    void pielog_set(pielog_span s)
    pielog_span pielog_begin(pielog_span s, const char *text, size_t len)
    pielog_span pielog_end(pielog_span s)
    void pielog_pr(pielog_span s, const char *text, size_t len)
    pielog_span pielog_log_begin(pielog_span s, const char *text, size_t len)
    pielog_span pielog_log_end(pielog_span s)
    void pielog_set_text(pielog_span s, const char *text, size_t len)
    void pielog_set_progress(pielog_span s, double done, double total)
    pielog_span pielog_root()
    const char *pielog_region_name()
    void pielog_start_terminal()
    void pielog_stop_terminal()
    void pielog_render_now()

cdef inline Pielog *c_api() except NULL:
    """Fetch the C API struct from pielog's capsule (imports pielog)."""
    import pielog
    return <Pielog *>PyCapsule_GetPointer(pielog._c_api(), "pielog._C_API")
