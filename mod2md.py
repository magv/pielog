#!/usr/bin/env python3
"""
Usage: mod2md.py module [--md|--pyi]

Format the contents of a module as Markdown (--md, default),
or generate a .pyi stub for it (--pyi).
"""

import argparse
import ast
import contextlib
import importlib
import inspect
import io
import re
import sys
import types
import typing

def strip_indent(text: str) -> str:
    lines = text.splitlines()
    common_n = None
    for line in lines:
        line_lstrip = line.lstrip()
        if line_lstrip != "":
            n = len(line) - len(line_lstrip)
            common_n = n if common_n is None else min(common_n, n)
    return "\n".join(line[common_n:] for line in lines).strip()

def class_member_docstrings(typ: type) -> dict[str, str]:
    docstrings = {}
    tree = ast.parse(inspect.getsource(typ))
    prev_el = None
    for el in tree.body[0].body:
        match (prev_el, el):
            case (
                ast.AnnAssign(target=ast.Name(id=name)),
                ast.Expr(value=ast.Constant(comment)),
            ):
                docstrings[name] = comment
        prev_el = el
    return docstrings

# First pass

HREFS = {}
IDS = {}

def is_cython_function(obj) -> bool:
    return type(obj).__name__ == "cython_function_or_method"

def is_local(value, mod: types.ModuleType) -> bool:
    return getattr(value, "__module__", None) == mod.__name__

def unwrap(obj):
    """@contextmanager results document as their wrapped generator."""
    if getattr(obj, "__wrapped__", None) is not None:
        return obj.__wrapped__
    return obj

def prewr_function(name: str, fun: types.FunctionType, mod: types.ModuleType):
    anchor = f"{mod.__name__}.{name}"
    HREFS[f"{fun.__module__}.{name}"] = f"#{anchor}"
    IDS[f"{fun.__module__}.{name}"] = anchor

def prewr_class(name: str, typ: type, mod: types.ModuleType):
    anchor = f"{mod.__name__}.{name}"
    HREFS[f"{typ.__module__}.{name}"] = f"#{anchor}"
    IDS[f"{typ.__module__}.{name}"] = anchor

def prewr_module(mod: types.ModuleType):
    for name, value in inspect.getmembers(mod):
        if name.startswith("_"): continue
        if inspect.isfunction(value) or is_cython_function(value):
            if is_local(value, mod):
                prewr_function(name, value, mod)
        elif (value := unwrap(value)) is not value:
            if is_local(value, mod):
                prewr_function(name, value, mod)
        elif inspect.isclass(value):
            if is_local(value, mod):
                prewr_class(name, value, mod)
        else:
            pass

# Second pass

def wr(text: str):
    sys.stdout.write(text)

def wr_type(typ):
    if typ is Ellipsis:
        wr("...")
    elif isinstance(typ, types.GenericAlias):
        wr_type(typ.__origin__)
        wr("[")
        for i, arg in enumerate(typ.__args__):
            if i != 0: wr(", ")
            wr_type(arg)
        wr("]")
    elif isinstance(typ, types.UnionType):
        for i, arg in enumerate(typ.__args__):
            if i != 0: wr(" | ")
            wr_type(arg)
    elif typ is None or typ == types.NoneType:
        wr("None")
    elif isinstance(typ, typing.TypeVar):
        wr(typ.__name__)
    elif isinstance(typ, type):
        href = HREFS.get(f"{typ.__module__}.{typ.__name__}")
        if href:
            wr(f"[{typ.__name__}]({href})")
        else:
            wr(f"{typ.__name__}")
    else:
        raise ValueError(f"Can't format type {typ} (of type {type(typ)})")

def wr_function(name: str, fun: types.FunctionType, mod: types.ModuleType):
    fun = unwrap(fun)
    anchor = IDS.get(f"{fun.__module__}.{fun.__name__}")
    if anchor:
        wr(f"<a id=\"{anchor}\"></a>\n")
    wr(f"**{name}** (")
    ann = inspect.get_annotations(fun, eval_str=True)
    for i, (k, v) in enumerate(ann.items()):
        if k == "return": continue
        if i != 0: wr(", ")
        wr(f"{k}: *")
        wr_type(v)
        wr(f"*")
    wr(")")
    if "return" in ann:
        wr(" → *")
        wr_type(ann["return"])
        wr("*")
    wr("\n\n")
    for line in strip_indent(fun.__doc__).splitlines():
        wr(f"{line}\n")
    wr("\n")

def wr_dataclass(name: str, typ: type, mod: types.ModuleType):
    docstrings = class_member_docstrings(typ)
    anchor = IDS.get(f"{typ.__module__}.{typ.__name__}")
    if anchor:
        wr(f"<a id=\"{anchor}\"></a>\n")
    doc = strip_indent(typ.__doc__)
    wr(f"**{name}**. {doc}\n\n")
    for i, (fld, fld_type) in enumerate(inspect.get_annotations(typ, eval_str=True).items()):
        wr(f"* **{fld}**: *")
        wr_type(fld_type)
        wr("*")
        doc = docstrings.get(fld)
        if doc:
            wr(f". {doc.strip()}")
        wr("\n")
    wr(f"\n")

def wr_class(name: str, typ: type, mod: types.ModuleType):
    anchor = IDS.get(f"{typ.__module__}.{typ.__name__}")
    if anchor:
        wr(f"<a id=\"{anchor}\"></a>\n")
    wr(f"class **{name}**\n\n")

# Stub generation

STUB_IMPORTS = {}
STUB_INCOMPLETE = False

def stub_reset():
    global STUB_INCOMPLETE
    STUB_IMPORTS.clear()
    STUB_INCOMPLETE = False

def stub_type(typ, mod):
    """Render an annotation object for a stub, collecting needed imports."""
    global STUB_INCOMPLETE
    if typ is Ellipsis:
        return "..."
    if isinstance(typ, types.GenericAlias):
        args = ", ".join(stub_type(a, mod) for a in typ.__args__)
        return f"{stub_type(typ.__origin__, mod)}[{args}]"
    if isinstance(typ, types.UnionType):
        return " | ".join(stub_type(a, mod) for a in typ.__args__)
    if typ is None or typ is types.NoneType:
        return "None"
    if isinstance(typ, typing.TypeVar):
        return typ.__name__
    if isinstance(typ, type):
        if typ.__module__ in ("builtins", mod.__name__):
            return typ.__name__
        STUB_IMPORTS.setdefault(typ.__module__, set()).add(typ.__name__)
        return typ.__name__
    STUB_INCOMPLETE = True
    return "Incomplete"

def stub_ann(ann: dict, key: str, mod) -> str:
    global STUB_INCOMPLETE
    if key in ann:
        return stub_type(ann[key], mod)
    STUB_INCOMPLETE = True
    return "Incomplete"

def stub_ann_str(node: ast.expr, mod) -> str:
    """Render a source-level annotation, resolving names through the module."""
    s = ast.unparse(node)
    try:
        return stub_type(eval(s, vars(mod)), mod)
    except Exception:
        return s

def stub_params(sig, ann: dict, mod) -> str:
    parts = []
    names = list(sig.parameters)
    for i, (pname, p) in enumerate(sig.parameters.items()):
        if p.kind == inspect.Parameter.VAR_POSITIONAL:
            s = f"*{pname}"
            if pname in ann: s += f": {stub_ann(ann, pname, mod)}"
        elif p.kind == inspect.Parameter.VAR_KEYWORD:
            s = f"**{pname}"
            if pname in ann: s += f": {stub_ann(ann, pname, mod)}"
        else:
            if pname in ann:
                s = f"{pname}: {stub_ann(ann, pname, mod)}"
            elif pname == "self":
                s = pname
            else:
                global STUB_INCOMPLETE
                STUB_INCOMPLETE = True
                s = f"{pname}: Incomplete"
            if p.default is not inspect.Parameter.empty:
                s += f" = {p.default!r}"
        parts.append(s)
        if p.kind == inspect.Parameter.POSITIONAL_ONLY and (
            i + 1 == len(names)
            or sig.parameters[names[i + 1]].kind != inspect.Parameter.POSITIONAL_ONLY
        ):
            parts.append("/")
    return ", ".join(parts)

def wr_doc(doc: str | None, indent: str):
    if doc is None:
        wr(" ...\n")
        return
    lines = doc.splitlines()
    if len(lines) == 1:
        wr(f' """{lines[0]}"""\n')
        return
    b = indent + "    "
    wr("\n")
    wr(f'{b}"""\n')
    for line in lines:
        wr(f"{b}{line}\n")
    wr(f'{b}"""\n')

def wr_def_stub(name: str, fun, mod, indent: str = ""):
    fun = unwrap(fun)
    try:
        sig = inspect.signature(fun)
        ann = inspect.get_annotations(fun, eval_str=True)
    except (ValueError, TypeError):
        global STUB_INCOMPLETE
        STUB_INCOMPLETE = True
        wr(f"{indent}def {name}(*args: Incomplete, **kwargs: Incomplete) -> Incomplete: ...\n")
        return
    if "return" in ann:
        wr(f"{indent}def {name}({stub_params(sig, ann, mod)}) -> {stub_type(ann['return'], mod)}:")
    else:
        wr(f"{indent}def {name}({stub_params(sig, ann, mod)}):")
    wr_doc(inspect.cleandoc(fun.__doc__) if fun.__doc__ else None, indent)

def pyx_path(mod) -> str:
    path = mod.__file__
    for suffix in (".abi3.so", ".so", ".pyd"):
        if path.endswith(suffix):
            path = path[: -len(suffix)]
            break
    return path + ".pyx"

def pyx_class_tree(typ: type, mod):
    """Parse the cdef class block out of the module's .pyx source.

    Runtime introspection of a compiled cdef class yields slot wrappers
    and opaque getset descriptors, so the source is the only record of
    member signatures and annotations.
    """
    try:
        text = open(pyx_path(mod)).read()
        m = re.search(rf"^cdef class {typ.__name__}\b.*?(?=^\S|\Z)", text, re.M | re.S)
        block = re.sub("^cdef class", "class", m.group(0), count=1, flags=re.M)
        block = re.sub(r"^\s+cdef\b.*$", "", block, flags=re.M)  # cdef attribute lines
        return ast.parse(block)
    except (OSError, AttributeError, SyntaxError):
        return None

def wr_method_stub(el: ast.FunctionDef, mod):
    a = el.args
    parts = []
    plain = a.posonlyargs + a.args
    for i, arg in enumerate(plain):
        s = arg.arg
        if arg.annotation: s += f": {stub_ann_str(arg.annotation, mod)}"
        d = i - (len(plain) - len(a.defaults))
        if d >= 0: s += f" = {ast.unparse(a.defaults[d])}"
        parts.append(s)
    if a.posonlyargs: parts.append("/")
    if a.vararg:
        s = f"*{a.vararg.arg}"
        if a.vararg.annotation: s += f": {stub_ann_str(a.vararg.annotation, mod)}"
        parts.append(s)
    elif a.kwonlyargs:
        parts.append("*")
    for arg, d in zip(a.kwonlyargs, a.kw_defaults):
        s = arg.arg
        if arg.annotation: s += f": {stub_ann_str(arg.annotation, mod)}"
        if d is not None: s += f" = {ast.unparse(d)}"
        parts.append(s)
    if a.kwarg: parts.append(f"**{a.kwarg.arg}")
    ret = f" -> {stub_ann_str(el.returns, mod)}" if el.returns else ""
    wr(f"    def {el.name}({', '.join(parts)}){ret}:")
    wr_doc(ast.get_docstring(el), "    ")

def wr_class_stub(name: str, typ: type, mod):
    wr(f"class {name}:")
    tree = pyx_class_tree(typ, mod)
    if tree is None:
        wr("    ...\n")
        return
    doc = ast.get_docstring(tree.body[0])
    if doc is None:
        wr("    ...\n")
    else:
        wr_doc(doc, "")
    for el in tree.body[0].body:
        match el:
            case ast.AnnAssign(target=ast.Name(id=nid)):
                wr(f"    {nid}: {stub_ann_str(el.annotation, mod)}\n")
            case ast.FunctionDef(decorator_list=[ast.Name(id="property")]):
                ret = stub_ann_str(el.returns, mod) if el.returns else "Incomplete"
                wr(f"    {el.name}: {ret}\n")
            case ast.FunctionDef():
                wr_method_stub(el, mod)

def wr_module_stub(mod: types.ModuleType):
    stub_reset()
    buf = io.StringIO()
    old = sys.stdout
    sys.stdout = buf
    try:
        for name, value in inspect.getmembers(mod):
            if name.startswith("_") and name != "__version__": continue
            if is_cython_function(value) or inspect.isfunction(value): continue
            if inspect.isclass(value) or inspect.ismodule(value): continue
            if isinstance(value, typing.TypeVar): continue
            wr(f"{name}: {stub_type(type(value), mod)}\n")
        wr("\n")
        for name, value in inspect.getmembers(mod):
            if name.startswith("_") or not is_local(value, mod): continue
            if is_cython_function(value) or inspect.isfunction(value):
                wr_def_stub(name, value, mod)
            elif (value := unwrap(value)) is not value:
                wr_def_stub(name, value, mod)
        classes = [name for name, value in inspect.getmembers(mod)
                   if not name.startswith("_") and inspect.isclass(value)
                   and is_local(value, mod)]
        if classes:
            wr("\n")
        for k, name in enumerate(classes):
            if k != 0: wr("\n")
            wr_class_stub(name, getattr(mod, name), mod)
    finally:
        sys.stdout = old
    wr("# Generated by mod2md.py; do not edit.\n")
    wr(f'"""Type stub for {mod.__name__}."""\n')
    for module, names in sorted(STUB_IMPORTS.items()):
        wr(f"from {module} import {', '.join(sorted(names))}\n")
    typevars = [v for v in vars(mod).values() if isinstance(v, typing.TypeVar)]
    if typevars: wr("from typing import TypeVar\n")
    if STUB_INCOMPLETE: wr("from _typeshed import Incomplete\n")
    if STUB_IMPORTS or typevars or STUB_INCOMPLETE: wr("\n")
    for tv in typevars:
        wr(f'{tv.__name__} = TypeVar("{tv.__name__}")\n')
    if typevars: wr("\n")
    wr(buf.getvalue())

def wr_module(mod: types.ModuleType):
    wr("### Functions\n\n")
    for name, value in inspect.getmembers(mod):
        if name.startswith("_"): continue
        if inspect.isfunction(value) or is_cython_function(value):
            if is_local(value, mod):
                wr_function(name, value, mod)
        elif (value := unwrap(value)) is not value:
            if is_local(value, mod):
                wr_function(name, value, mod)
    wr("### Classes\n\n")
    for name, value in inspect.getmembers(mod):
        if name.startswith("_"): continue
        if inspect.isclass(value):
            if not is_local(value, mod): continue
            if hasattr(value, "__dataclass_fields__"):
                wr_dataclass(name, value, mod)
            else:
                wr_class(name, value, mod)
    # Double-check there's nothing else.
    for name, value in inspect.getmembers(mod):
        if name.startswith("_"): continue
        if inspect.isfunction(value) or is_cython_function(value): continue
        if inspect.isclass(value): continue
        if inspect.ismodule(value): continue
        if unwrap(value) is not value: continue
        if not is_local(value, mod): continue
        raise ValueError(f"Can't document {mod.__name__}.{name} (a {type(value)})")

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("module")
    fmt = ap.add_mutually_exclusive_group()
    fmt.add_argument("--md", action="store_true", help="format as Markdown (default)")
    fmt.add_argument("--pyi", action="store_true", help="generate a .pyi stub")
    args = ap.parse_args()
    mod = importlib.import_module(args.module)
    if args.pyi:
        wr_module_stub(mod)
    else:
        prewr_module(mod)
        wr_module(mod)

if __name__ == "__main__":
    main()
