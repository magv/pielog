# Pielog

In long-running tools that center around calculating a result,
one needs a way to see what the code has done before, and what
is it doing right now. The former is logging; the latter is
execution tracing.

Pielog is a minimal logging & execution tracing library for C++,
Python, and Cython. It allows for seamless logging & tracing from
a mixture of Python and C++ sources, threads, and subprocesses.

## How to use

In Python:

    import pielog as pl

    with pl.trace("task"):
        for i in range(10):
            pl.log(f"working on item {i}")
            pl.trace_progress(i+1, 10)

In C++:

    #include <pielog.h>

    ...

    {
        trace_scope("task");
        for (int i = 0; i < 10; i++) {
            pr("working on item {}", i);
            trace_progress(i+1, 10);
        }
    }

Note that the formatted printing here is its own facility,
available standalone via `#include <bprint.h>`. We need it
because the built-in C printing (`printf`) is not extensible,
and the built-in C++ printing (`iostream`) is not convenient.

If you want your custom type to work in `pr()`, define this
function:

    static inline void
    bprint(Buf &b, const your_type &x)
    {
        b.print("my_type({}, {}, {})", 1, 2, "etc");
    }

## C++ API

<a id="pr"></a>
**pr**(format, ...)

Print a log message.

<a id="trace_scope"></a>
**trace_scope**(format, ...)

Start a span (a traced region) with the given name, and automatically
exit it at the end of the current lexical scope.

<a id="trace_enter"></a>
**trace_enter**(format, ...)

Enter a span with the given name.

<a id="trace_exit"></a>
**trace_exit**()

Exit the current span.

<a id="log_trace_scope"></a>
**log_trace_scope**(format, ...)

Start a span with the given name, log it, and automatically exit
it at the end of the current lexical scope.

<a id="log_trace_enter"></a>
**log_trace_enter**(format, ...)

Enter a span with the given name, and log it.

<a id="log_trace_exit"></a>
**log_trace_exit**()

Exit the current span, and log it.

<a id="trace_text"></a>
**trace_text**(format, ...)

Change the text of the current span.

<a id="trace_progress"></a>
**trace_progress**(ndone, ntotal)

Change the progress bar of the current span.

<a id="trace_here"></a>
**trace_here**() → span

Get a reference to the current span that can be passed down to
[**trace_scope_under**](#trace_scope_under).

<a id="trace_scope_under"></a>
**trace_scope_under**(span)

Set the given span to be the current, but automatically undo
that at the end of the current lexical scope.

<a id="pielog_start_terminal"></a>
**pielog_start_terminal**()

Start a terminal renderer of the trace, unless a different one
is already started by a parent process. This renderer will work
as a separate thread, updating the display from time to time.

<a id="pielog_stop_terminal"></a>
**pielog_stop_terminal**()

Stop the terminal renderer, if it was started by us.

## Python API

### Functions

<a id="pielog.auto_log_trace"></a>
**auto_log_trace** (fn: *Callable[..., _T]*) → *Callable[..., _T]*

Decorator: wrap each call of fn in a span named after it, logging entry and exit.

<a id="pielog.auto_trace"></a>
**auto_trace** (fn: *Callable[..., _T]*) → *Callable[..., _T]*

Decorator: wrap each call of fn in a span named after it.

<a id="pielog.here"></a>
**here** () → *[SpanRef](#pielog.SpanRef)*

Return this context's current span, for use with trace_under()/wrap().

<a id="pielog.log"></a>
**log** (text: *str*)

Print one log line, indented one level under the nearest enclosing
log_trace() (a trace() adds no indent).

<a id="pielog.log_trace"></a>
**log_trace** (text: *str*) → *AbstractContextManager[None]*

Context manager: open a span around the body, logging entry and exit.

<a id="pielog.log_trace_enter"></a>
**log_trace_enter** (text: *str*)

Open a span around the code that follows, and log its opening.

<a id="pielog.log_trace_exit"></a>
**log_trace_exit** ()

Close the span opened by log_trace_enter(), and log its completion.

<a id="pielog.start_terminal"></a>
**start_terminal** ()

Start a terminal renderer of the trace, unless a different one
is already started by a parent process.

<a id="pielog.stop_terminal"></a>
**stop_terminal** ()

Stop the terminal renderer, if it was started by us.

<a id="pielog.trace"></a>
**trace** (text: *str*) → *AbstractContextManager[None]*

Context manager: open a span around the body.

<a id="pielog.trace_enter"></a>
**trace_enter** (text: *str*)

Open a span around the code that follows.

<a id="pielog.trace_exit"></a>
**trace_exit** ()

Close the span opened by trace_enter().

<a id="pielog.trace_progress"></a>
**trace_progress** (items_done: *int | float*, items_total: *int | float*)

Report progress of the current span.

<a id="pielog.trace_text"></a>
**trace_text** (text: *str*)

Rename the current span.

<a id="pielog.trace_under"></a>
**trace_under** (parent: *[SpanRef](#pielog.SpanRef)*) → *AbstractContextManager[None]*

Context manager: log the body under the given span, opening no
span of its own -- scopes started inside hang under it. The body must
be balanced: a trace_exit() closes the target span, and
trace_text()/trace_progress() act on it.

<a id="pielog.wrap"></a>
**wrap** (fn: *Callable[..., _T]*, span: *[SpanRef](#pielog.SpanRef) | None*) → *Callable[..., _T]*

Return fn wrapped to run under the given span (or current one).

### Classes

<a id="pielog.SpanRef"></a>
class **SpanRef**

