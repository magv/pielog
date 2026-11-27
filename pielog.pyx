# cython: language_level = 3
# cython: language = c++

from collections.abc import Callable
from contextlib import AbstractContextManager
from contextlib import contextmanager
from contextvars import ContextVar
from cpython.pycapsule cimport PyCapsule_New
from functools import partial
from functools import wraps
import os
import sys
from typing import Any
from typing import TypeVar

from pielog cimport (
    Pielog,
    pielog_begin,
    pielog_end,
    pielog_get,
    pielog_log_begin,
    pielog_log_end,
    pielog_pr,
    pielog_region_name,
    pielog_root,
    pielog_set,
    pielog_set_progress,
    pielog_set_text,
    pielog_span,
    pielog_start_terminal,
    pielog_stop_terminal,
)

cdef extern from "pielog/pielog.cpp":
    Pielog *pielog
    Pielog *_api()

cdef extern from "pielog/_version.h":
    const char* _VERSION "PIELOG_VERSION"

__version__: str = _VERSION.decode("utf-8")

cdef class SpanRef:
    """Handle of a span; stable across contexts, invalid once the span is over."""
    cdef pielog_span ref

    def __init__(self, proc: int = 0, rec: int = 0, gen: int = 0):
        self.ref.proc = proc
        self.ref.rec = rec
        self.ref.gen = gen

    @property
    def proc(self) -> int: return self.ref.proc

    @property
    def rec(self) -> int: return self.ref.rec

    @property
    def gen(self) -> int: return self.ref.gen

    def __repr__(self) -> str:
        return f"SpanRef(proc={self.ref.proc}, rec={self.ref.rec}, gen={self.ref.gen})"

    def __reduce__(self) -> tuple[type[SpanRef], tuple[int, int, int]]:
        """Return (proc, rec, gen) for pickling."""
        return (SpanRef, (self.ref.proc, self.ref.rec, self.ref.gen))

_CUR: ContextVar = ContextVar("pielog_span", default=None)
cdef SpanRef _ROOT = None

cdef pielog_span _here():
    cdef SpanRef c = _CUR.get()
    if pielog == NULL or c is None:
        _api()
        c = _ROOT
        _CUR.set(c)
    # Mirror into the thread's current span, so C API users would
    # continue from the same span.
    pielog_set(c.ref)
    return c.ref

cdef void _set(pielog_span span):
    cdef SpanRef r = SpanRef.__new__(SpanRef)
    r.ref = span
    _CUR.set(r)
    pielog_set(span)

def log(text: str):
    """
    Print one log line, indented one level under the nearest enclosing
    log_trace() (a trace() adds no indent).
    """
    cdef bytes b = text.encode("utf-8")
    pielog_pr(_here(), b, len(b))

def trace_enter(text: str):
    """Open a span around the code that follows."""
    cdef bytes b = text.encode("utf-8")
    cdef pielog_span s = _here()
    _set(pielog_begin(s, b, len(b)))

def trace_exit():
    """Close the span opened by trace_enter()."""
    cdef pielog_span s = _here()
    _set(pielog_end(s))

def log_trace_enter(text: str):
    """Open a span around the code that follows, and log its opening."""
    cdef bytes b = text.encode("utf-8")
    cdef pielog_span s = _here()
    _set(pielog_log_begin(s, b, len(b)))

def log_trace_exit():
    """Close the span opened by log_trace_enter(), and log its completion."""
    cdef pielog_span s = _here()
    _set(pielog_log_end(s))

def trace_text(text: str):
    """Rename the current span."""
    cdef bytes b = text.encode("utf-8")
    pielog_set_text(_here(), b, len(b))

def trace_progress(items_done: int | float, items_total: int | float):
    """Report progress of the current span."""
    pielog_set_progress(_here(), items_done, items_total)

def here() -> SpanRef:
    """Return this context's current span, for use with trace_under()/wrap()."""
    cdef SpanRef r = SpanRef.__new__(SpanRef)
    r.ref = _here()
    return r

def start_terminal():
    """
    Start a terminal renderer of the trace, unless a different one
    is already started by a parent process.
    """
    pielog_start_terminal()

def stop_terminal():
    """Stop the terminal renderer, if it was started by us."""
    pielog_stop_terminal()

def _c_api() -> object:
    """Return the capsule C consumers fetch the C API from."""
    return PyCapsule_New(pielog_get(), "pielog._C_API", NULL)

_T = TypeVar("_T")

def auto_trace(fn: Callable[..., _T]) -> Callable[..., _T]:
    """Decorator: wrap each call of fn in a span named after it."""
    @wraps(fn)
    def fn_wrapper(*args: Any, **kwargs: Any) -> _T:
        saved = _here()
        trace_enter(fn.__name__)
        try:
            return fn(*args, **kwargs)
        finally:
            trace_exit()
            _set(saved)
    return fn_wrapper

def auto_log_trace(fn: Callable[..., _T]) -> Callable[..., _T]:
    """Decorator: wrap each call of fn in a span named after it, logging entry and exit."""

    @wraps(fn)
    def fn_wrapper(*args: Any, **kwargs: Any) -> _T:
        saved = _here()
        log_trace_enter(fn.__name__)
        try:
            return fn(*args, **kwargs)
        finally:
            log_trace_exit()
            _set(saved)

    return fn_wrapper

@contextmanager
def trace(text: str) -> AbstractContextManager[None]:
    """Context manager: open a span around the body."""
    saved = _here()
    trace_enter(text)
    try:
        yield
    finally:
        trace_exit()
        _set(saved)

@contextmanager
def log_trace(text: str) -> AbstractContextManager[None]:
    """Context manager: open a span around the body, logging entry and exit."""
    saved = _here()
    log_trace_enter(text)
    try:
        yield
    finally:
        log_trace_exit()
        _set(saved)

@contextmanager
def trace_under(parent: SpanRef) -> AbstractContextManager[None]:
    """
    Context manager: log the body under the given span, opening no
    span of its own -- scopes started inside hang under it. The body must
    be balanced: a trace_exit() closes the target span, and
    trace_text()/trace_progress() act on it.
    """
    saved = _here()
    _set(parent.ref)
    try:
        yield
    finally:
        _set(saved)

def _run_under(
    span: SpanRef, fn: Callable[..., _T], /, *args: Any, **kwargs: Any
) -> _T:
    with trace_under(span):
        return fn(*args, **kwargs)

def wrap(fn: Callable[..., _T], span: SpanRef | None = None) -> Callable[..., _T]:
    """Return fn wrapped to run under the given span (or current one)."""
    return partial(_run_under, span if span is not None else here(), fn)

# The shared region exists from the first use on; export its name so
# every child process (fork, spawn, exec) attaches to the same tree.
_SHM_NAME = pielog_region_name().decode("ascii")
if os.environ.get("PIELOG") != _SHM_NAME:
    os.environ["PIELOG"] = _SHM_NAME
_ROOT = SpanRef(0, 0, 0)

if sys.stderr.isatty():
    start_terminal()
