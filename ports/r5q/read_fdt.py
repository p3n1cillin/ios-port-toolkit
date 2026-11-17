"""Read selected hardware facts from a saved FDT without accessing phone data."""
import json
import pathlib
import struct
import sys


def read_tree(path):
    data = pathlib.Path(path).read_bytes()
    header = struct.unpack_from(">10I", data)
    magic, total, structure, strings, _, _, _, _, strings_size, structure_size = header
    if magic != 0xD00DFEED or total > len(data):
        raise ValueError("Invalid FDT header")
    if structure + structure_size > total or strings + strings_size > total:
        raise ValueError("Invalid FDT section bounds")
    names = data[strings:strings + strings_size]
    offset = structure
    end = structure + structure_size
    stack = []
    tree = {}
    while offset + 4 <= end:
        token = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if token == 1:
            name_end = data.index(b"\0", offset, end)
            stack.append(data[offset:name_end].decode("ascii"))
            offset = (name_end + 4) & ~3
            tree["/".join(stack) or "/"] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name_offset = struct.unpack_from(">II", data, offset)
            offset += 8
            if offset + length > end or name_offset >= len(names):
                raise ValueError("Invalid FDT property")
            name_end = names.index(b"\0", name_offset)
            name = names[name_offset:name_end].decode("ascii")
            tree["/".join(stack) or "/"][name] = data[offset:offset + length]
            offset = (offset + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            return tree
        else:
            raise ValueError(f"Unexpected FDT token {token}")
    raise ValueError("FDT has no end token")


def format_value(name, value):
    if name in {"compatible", "model", "status", "method", "label"}:
        return value.rstrip(b"\0").decode("ascii", errors="replace").split("\0")
    if len(value) % 4 == 0:
        return [f"0x{word:x}" for word in struct.unpack(f">{len(value) // 4}I", value)]
    return value.hex()


if __name__ == '__main__':
    tree = read_tree(sys.argv[1])
    selected = {}
    for path, properties in tree.items():
        relevant = path == "/" or path.startswith("/memory") or path == "/psci"
        relevant |= path.startswith("/reserved-memory/") and "reg" in properties
        relevant |= path.startswith("/soc/i2c@c80000")
        relevant |= "qcom,mdss-dsi-panel-width" in properties
        relevant |= "ufshc@1d84000" in path
        if relevant:
            fields = {"model", "compatible", "reg", "status", "method", "label",
                      "qcom,mdss-dsi-panel-width", "qcom,mdss-dsi-panel-height",
                      "qcom,mdss-dsi-panel-framerate", "interrupts", "sec,irq-gpio",
                      "sec,reset-gpio", "sec,max_coords", "vdd-supply", "avdd-supply"}
            selected[path] = {name: format_value(name, value)
                              for name, value in properties.items() if name in fields}
    print(json.dumps(selected, indent=2))
