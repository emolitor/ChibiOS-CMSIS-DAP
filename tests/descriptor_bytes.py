"""Evaluate usbcfg.c descriptor byte arrays and validate the emitted bytes.

Rather than matching source-text tokens, this expands the ChibiOS USB_DESC_*
helper macros to the actual bytes the device sends and checks structural
invariants (bLength fields, wTotalLength sums, string lengths). It catches
descriptor inconsistencies (e.g. a wrong string bLength) that a token match
misses, without a full ChibiOS host build.
"""

import ast
import operator
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "usbcfg.c").read_text(encoding="utf-8")
HEADER = (ROOT / "usbcfg.h").read_text(encoding="utf-8")

# Standard USB descriptor type codes (USB 2.0 / framework spec, fixed values).
USB_DESC_TYPES = {
    "USB_DESCRIPTOR_DEVICE": 0x01,
    "USB_DESCRIPTOR_CONFIGURATION": 0x02,
    "USB_DESCRIPTOR_STRING": 0x03,
    "USB_DESCRIPTOR_INTERFACE": 0x04,
    "USB_DESCRIPTOR_ENDPOINT": 0x05,
    "USB_DESCRIPTOR_INTERFACE_ASSOCIATION": 0x0B,
}

# Fixed helper-macro descriptor sizes from ChibiOS hal_usb.h.
BASE_SYMBOLS = {
    **USB_DESC_TYPES,
    "USB_DESC_DEVICE_SIZE": 18,
    "USB_DESC_CONFIGURATION_SIZE": 9,
    "USB_DESC_INTERFACE_SIZE": 9,
    "USB_DESC_INTERFACE_ASSOCIATION_SIZE": 8,
    "USB_DESC_ENDPOINT_SIZE": 7,
}


def _symbols():
    syms = dict(BASE_SYMBOLS)
    # Integer #defines from usbcfg.h (endpoints, vendor code) and usbcfg.c
    # (e.g. CONFIG_DESC_SIZE, MS_OS_20_DESC_SET_SIZE).
    for text in (HEADER, SOURCE):
        for name, value in re.findall(
            r"^#define\s+([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)U?\b",
            text,
            re.MULTILINE,
        ):
            syms[name] = int(value, 0)
    return syms


SYMBOLS = _symbols()


# Operators permitted in descriptor integer expressions.
_BINOPS = {
    ast.BitOr: operator.or_,
    ast.BitAnd: operator.and_,
    ast.BitXor: operator.xor,
    ast.LShift: operator.lshift,
    ast.RShift: operator.rshift,
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
}
_UNARYOPS = {ast.UAdd: operator.pos, ast.USub: operator.neg, ast.Invert: operator.invert}


def _eval_ast(node):
    """Evaluate a whitelisted arithmetic AST node against the symbol table."""
    if isinstance(node, ast.Expression):
        return _eval_ast(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, int):
        return node.value
    if isinstance(node, ast.Name):
        if node.id in SYMBOLS:
            return SYMBOLS[node.id]
        raise ValueError(f"unknown descriptor symbol: {node.id}")
    if isinstance(node, ast.BinOp) and type(node.op) in _BINOPS:
        return _BINOPS[type(node.op)](_eval_ast(node.left), _eval_ast(node.right))
    if isinstance(node, ast.UnaryOp) and type(node.op) in _UNARYOPS:
        return _UNARYOPS[type(node.op)](_eval_ast(node.operand))
    raise ValueError(f"unsupported expression element: {ast.dump(node)}")


def _eval_expr(expr):
    """Evaluate a C integer/char expression using the descriptor symbols.

    Only integer/character literals, whitelisted operators, and the known
    descriptor symbols are accepted; the expression is parsed and walked as an
    AST rather than eval()'d, so no arbitrary code can run even if usbcfg.c is
    malformed.
    """
    expr = expr.strip()
    # Character literal, e.g. 'R' or ')'.
    m = re.fullmatch(r"'(\\?.)'", expr)
    if m:
        return ord(m.group(1).encode().decode("unicode_escape"))
    # Strip integer suffixes (U/L) so Python's parser accepts the literals.
    normalized = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)[UuLl]+", r"\1", expr)
    try:
        tree = ast.parse(normalized, mode="eval")
    except SyntaxError as exc:
        raise ValueError(f"cannot parse expression {expr!r}") from exc
    return int(_eval_ast(tree)) & 0xFFFFFFFF


def _le(value, width):
    return [(value >> (8 * i)) & 0xFF for i in range(width)]


# Helper macros -> ordered list of (field_name_ignored, width) emitters. The
# byte-emitting shape mirrors hal_usb.h exactly.
def _macro_device(a):
    return (
        [18, USB_DESC_TYPES["USB_DESCRIPTOR_DEVICE"]]
        + _le(a[0], 2)                       # bcdUSB
        + [a[1] & 0xFF, a[2] & 0xFF, a[3] & 0xFF, a[4] & 0xFF]
        + _le(a[5], 2) + _le(a[6], 2) + _le(a[7], 2)  # idVendor/idProduct/bcdDevice
        + [a[8] & 0xFF, a[9] & 0xFF, a[10] & 0xFF, a[11] & 0xFF]
    )


def _macro_configuration(a):
    return (
        [9, USB_DESC_TYPES["USB_DESCRIPTOR_CONFIGURATION"]]
        + _le(a[0], 2)                       # wTotalLength
        + [a[1] & 0xFF, a[2] & 0xFF, a[3] & 0xFF, a[4] & 0xFF, a[5] & 0xFF]
    )


def _macro_interface(a):
    return [9, USB_DESC_TYPES["USB_DESCRIPTOR_INTERFACE"]] + [x & 0xFF for x in a]


def _macro_iad(a):
    return [8, USB_DESC_TYPES["USB_DESCRIPTOR_INTERFACE_ASSOCIATION"]] + [
        x & 0xFF for x in a
    ]


def _macro_endpoint(a):
    return (
        [7, USB_DESC_TYPES["USB_DESCRIPTOR_ENDPOINT"]]
        + [a[0] & 0xFF, a[1] & 0xFF]
        + _le(a[2], 2)                       # wMaxPacketSize
        + [a[3] & 0xFF]
    )


MACROS = {
    "USB_DESC_BYTE": lambda a: [a[0] & 0xFF],
    "USB_DESC_INDEX": lambda a: [a[0] & 0xFF],
    "USB_DESC_WORD": lambda a: _le(a[0], 2),
    "USB_DESC_BCD": lambda a: _le(a[0], 2),
    "USB_DESC_DEVICE": _macro_device,
    "USB_DESC_CONFIGURATION": _macro_configuration,
    "USB_DESC_INTERFACE": _macro_interface,
    "USB_DESC_INTERFACE_ASSOCIATION": _macro_iad,
    "USB_DESC_ENDPOINT": _macro_endpoint,
}


def _split_top_level(text):
    """Split a brace-initializer body on top-level commas.

    Bracket depth is tracked so commas inside macro arguments do not split,
    and character/string literals are passed through verbatim so brackets or
    commas inside them (e.g. '(' or ')') do not affect the depth.
    """
    elems, depth, cur, i = [], 0, "", 0
    while i < len(text):
        ch = text[i]
        if ch in "'\"":
            # Copy the whole literal, honoring backslash escapes.
            quote, cur, i = ch, cur + ch, i + 1
            while i < len(text):
                cur += text[i]
                if text[i] == "\\":
                    i += 1
                    if i < len(text):
                        cur += text[i]
                elif text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            elems.append(cur)
            cur = ""
        else:
            cur += ch
        i += 1
    if cur.strip():
        elems.append(cur)
    return [e.strip() for e in elems if e.strip()]


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def emit_bytes(initializer):
    """Expand a descriptor initializer body to the concrete byte list."""
    out = []
    for elem in _split_top_level(_strip_comments(initializer)):
        m = re.fullmatch(r"(USB_DESC_[A-Z_]+)\s*\((.*)\)", elem, re.DOTALL)
        if m and m.group(1) in MACROS:
            args = [_eval_expr(x) for x in _split_top_level(m.group(2))]
            out.extend(MACROS[m.group(1)](args))
        else:
            out.append(_eval_expr(elem) & 0xFF)
    return out


def declared_size(name):
    """Return the declared dimension N in `uint8_t NAME[N]`, or None if `[]`."""
    m = re.search(
        r"uint8_t\s+" + re.escape(name) + r"\s*\[\s*(\d+)?\s*\]",
        SOURCE,
    )
    if not m:
        raise KeyError(name)
    return int(m.group(1)) if m.group(1) else None


def descriptor(name):
    """Return the emitted bytes for a `static ... uint8_t NAME[...] = {...};`.

    An explicitly sized array (`NAME[N]`) emits N bytes: C zero-initializes any
    positions the initializer leaves out. This models that by zero-padding to
    the declared dimension (and rejecting an initializer that overflows it).
    """
    m = re.search(
        r"uint8_t\s+" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};",
        SOURCE,
        re.DOTALL,
    )
    if not m:
        raise KeyError(name)
    out = emit_bytes(m.group(1))
    size = declared_size(name)
    if size is not None:
        if len(out) > size:
            raise ValueError(f"{name}: {len(out)} initializers exceed [{size}]")
        out += [0] * (size - len(out))
    return out


def walk_children(desc, start):
    """Yield (offset, bLength, bDescriptorType) for each child descriptor."""
    off = start
    while off < len(desc):
        blen = desc[off]
        assert blen >= 2, f"child bLength {blen} at offset {off} too small"
        assert off + blen <= len(desc), f"child at {off} overruns descriptor"
        yield off, blen, desc[off + 1]
        off += blen
    assert off == len(desc), "child descriptors do not tile the container"
