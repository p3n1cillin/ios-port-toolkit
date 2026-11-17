"""A small parser for decompiled device trees (the text `dtc -I dtb -O dts` writes).

Only what dtc emits is supported: nodes `[label:] name[@unit] { ... };`, properties `name;` or
`name = value[, value...];` where a value is `<cells>`, `"string"` or `[bytes]`. Phandles are
resolved through each node's `phandle` property (dtc writes them as plain numbers once the tree
has been through a DTB). No include files, no macros, no /delete-node/.

    tree = parse(open('live.dts').read())
    for node in tree.walk():
        if node.compatible_has('sec,sec_ts'): ...
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

_TOKEN = re.compile(r'''
    (?P<ws>\s+)
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:[^"\\]|\\.)*")
  | (?P<cells><[^>]*>)
  | (?P<bytes>\[[^\]]*\])
  | (?P<punct>[{};=,])
  | (?P<word>[^\s{};=,"<\[]+(?:,[^\s{};=,"<\[]+)*)     # names keep their vendor prefix: qcom,pon-type
''', re.S | re.X)


@dataclass
class Node:
    name: str
    parent: 'Node | None' = None
    props: dict = field(default_factory=dict)
    children: list = field(default_factory=list)
    labels: list = field(default_factory=list)

    # -- identity -----------------------------------------------------------------------------
    @property
    def base(self) -> str:
        return self.name.split('@', 1)[0]

    @property
    def unit(self) -> int | None:
        if '@' not in self.name:
            return None
        try:
            return int(self.name.split('@', 1)[1].split(',')[0], 16)
        except ValueError:
            return None

    @property
    def path(self) -> str:
        if self.parent is None:
            return '/'
        up = self.parent.path
        return (up if up != '/' else '') + '/' + self.name

    # -- properties ---------------------------------------------------------------------------
    def strings(self, key: str) -> list[str]:
        return [v for v in self.props.get(key, []) if isinstance(v, str)]

    def string(self, key: str, default=None):
        s = self.strings(key)
        return s[0] if s else default

    def cells(self, key: str) -> list[int]:
        out = []
        for v in self.props.get(key, []):
            if isinstance(v, list):
                out.extend(v)
        return out

    def cell(self, key: str, default=None):
        c = self.cells(key)
        return c[0] if c else default

    def has(self, key: str) -> bool:
        return key in self.props

    def compatible_has(self, *needles: str) -> bool:
        comp = self.strings('compatible')
        return any(n in c for n in needles for c in comp)

    @property
    def status_ok(self) -> bool:
        return self.string('status', 'okay') in ('okay', 'ok')

    # -- tree ---------------------------------------------------------------------------------
    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()

    def child(self, name: str):
        for c in self.children:
            if c.name == name or c.base == name:
                return c
        return None

    def find(self, path: str):
        node = self
        for part in [p for p in path.split('/') if p]:
            node = node.child(part) if node else None
        return node

    def address_cells(self) -> int:
        """#address-cells that applies to this node's reg (its parent's)."""
        p = self.parent
        return p.cell('#address-cells', 2) if p is not None else 2

    def size_cells(self) -> int:
        p = self.parent
        return p.cell('#size-cells', 1) if p is not None else 1

    def reg(self) -> list[tuple[int, int]]:
        """(address, size) pairs of `reg`, with the parent's cell counts (no ranges translation)."""
        ac, sc = self.address_cells(), self.size_cells()
        raw = self.cells('reg')
        step = ac + sc
        out = []
        if step == 0:
            return out
        for i in range(0, len(raw) - step + 1, step):
            a = 0
            for c in raw[i:i + ac]:
                a = (a << 32) | c
            s = 0
            for c in raw[i + ac:i + step]:
                s = (s << 32) | c
            out.append((a, s))
        return out

    def reg_names(self) -> dict[str, tuple[int, int]]:
        names = self.strings('reg-names')
        return dict(zip(names, self.reg()))


class Tree:
    def __init__(self, root: Node):
        self.root = root
        self.by_phandle: dict[int, Node] = {}
        self.by_label: dict[str, Node] = {}
        for n in root.walk():
            ph = n.cell('phandle') or n.cell('linux,phandle')
            if ph is not None:
                self.by_phandle[ph] = n
            for lab in n.labels:
                self.by_label[lab] = n

    def walk(self):
        return self.root.walk()

    def find(self, path: str):
        return self.root.find(path)

    def phandle(self, value):
        return self.by_phandle.get(value)

    def compatible(self, *needles: str, okay_only=False):
        return [n for n in self.walk() if n.compatible_has(*needles) and (n.status_ok or not okay_only)]


def _parse_value(tok_kind: str, text: str):
    if tok_kind == 'string':
        return bytes(text[1:-1], 'utf-8').decode('unicode_escape')
    if tok_kind == 'cells':
        body = text[1:-1].split()
        out = []
        for c in body:
            if c.startswith('&'):
                out.append(c)            # unresolved reference (source DTS); kept as text
                continue
            try:
                out.append(int(c, 0))
            except ValueError:
                out.append(c)
        return out
    if tok_kind == 'bytes':
        return bytes(int(b, 16) for b in text[1:-1].split())
    return text


def parse(text: str) -> Tree:
    toks = []
    for m in _TOKEN.finditer(text):
        kind = m.lastgroup
        if kind in ('ws', 'comment'):
            continue
        toks.append((kind, m.group()))
    i = 0
    root = Node('/')
    stack = [root]
    pending_labels: list[str] = []
    while i < len(toks):
        kind, val = toks[i]
        if kind == 'word' and val.startswith('/') and val in ('/dts-v1/', '/plugin/'):
            i += 2 if i + 1 < len(toks) and toks[i + 1][1] == ';' else 1
            continue
        if kind == 'word' and val == '/memreserve/':
            while toks[i][1] != ';':
                i += 1
            i += 1
            continue
        if kind == 'word' and val.endswith(':'):
            pending_labels.append(val[:-1])
            i += 1
            continue
        if kind == 'punct' and val == '}':
            stack.pop()
            i += 1
            if i < len(toks) and toks[i][1] == ';':
                i += 1
            continue
        if kind == 'word' and i + 1 < len(toks) and toks[i + 1][1] == '{':
            if val == '/' and len(stack) == 1:
                node = root          # the root node (possibly repeated in overlays-merged output)
            else:
                node = Node(val, parent=stack[-1])
                stack[-1].children.append(node)
            node.labels.extend(pending_labels)
            pending_labels = []
            stack.append(node)
            i += 2
            continue
        if kind == 'word':
            name = val
            pending_labels = []
            if i + 1 < len(toks) and toks[i + 1][1] == ';':
                stack[-1].props[name] = []
                i += 2
                continue
            if i + 1 < len(toks) and toks[i + 1][1] == '=':
                i += 2
                values = []
                while i < len(toks) and toks[i][1] != ';':
                    k, v = toks[i]
                    if k == 'word' and v.endswith(':'):   # label inside a value: ignore
                        i += 1
                        continue
                    if k != 'punct':
                        values.append(_parse_value(k, v))
                    i += 1
                stack[-1].props[name] = values
                i += 1
                continue
        i += 1   # anything unexpected: skip
    return Tree(root)


def load(path) -> Tree:
    with open(path, encoding='utf-8', errors='replace') as f:
        return parse(f.read())
