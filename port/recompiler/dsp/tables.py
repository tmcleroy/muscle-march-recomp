import re
from dataclasses import dataclass
from pathlib import Path

DSP_DIR = Path(__file__).resolve().parents[2] / "third_party" / "dolphin" / "Source" / "Core" / "Core" / "DSP"

TEMPLATE = re.compile(
    r'\{"(\w+)",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(\d+),\s*(\d+),\s*\{.*\},\s*'
    r"(true|false),\s*(true|false),\s*(true|false),\s*(true|false),\s*(true|false)\}"
)
FUNCTION = re.compile(r"\{(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*&Interpreter::(\w+)\}")


@dataclass(frozen=True)
class Template:
    name: str
    opcode: int
    mask: int
    size: int
    extended: bool
    branch: bool
    uncond_branch: bool


@dataclass(frozen=True)
class Handler:
    opcode: int
    mask: int
    function: str


def _split(text, marker):
    index = text.index(marker)
    return text[:index], text[index:]


def load_templates():
    text = (DSP_DIR / "DSPTables.cpp").read_text(encoding="utf-8")
    _, text = _split(text, "s_opcodes =")
    main, ext = _split(text, "s_opcodes_ext =")
    main = main[: main.index("}};")]
    ext = ext[: ext.index("}};")]

    def parse(block):
        result = []
        for line in block.splitlines():
            match = TEMPLATE.search(line)
            if match:
                name, opcode, mask, size, _, extended, branch, uncond, _, _ = match.groups()
                result.append(Template(name, int(opcode, 16), int(mask, 16), int(size), extended == "true", branch == "true", uncond == "true"))
        return result

    return parse(main), parse(ext)


def load_handlers():
    text = (DSP_DIR / "Interpreter" / "DSPIntTables.cpp").read_text(encoding="utf-8")
    main, ext = _split(text, "s_opcodes_ext")

    def parse(block):
        return [Handler(int(a, 16), int(b, 16), f) for a, b, f in FUNCTION.findall(block)]

    return parse(main), parse(ext)


def find(entries, value):
    for entry in entries:
        if value & entry.mask == entry.opcode:
            return entry
    return None


CW = Template("CW", 0x0000, 0x0000, 1, False, False, False)


class Tables:
    def __init__(self):
        self.templates, self.ext_templates = load_templates()
        self.handlers, self.ext_handlers = load_handlers()
        if len(self.templates) != 230 or len(self.ext_templates) != 25:
            raise RuntimeError(f"unexpected template counts {len(self.templates)} {len(self.ext_templates)}")
        if len(self.handlers) != 125 or len(self.ext_handlers) != 25:
            raise RuntimeError(f"unexpected handler counts {len(self.handlers)} {len(self.ext_handlers)}")

    def template(self, inst):
        return find(self.templates, inst) or CW

    def handler(self, inst):
        found = find(self.handlers, inst)
        return found.function if found else "nop"

    def ext_handler(self, inst):
        value = inst & 0x7F if (inst >> 12) == 0x3 else inst & 0xFF
        found = find(self.ext_handlers, value)
        return found.function if found else "nop"


def load_idle_signatures():
    text = (DSP_DIR / "DSPAnalyzer.cpp").read_text(encoding="utf-8")
    start = text.index("idle_skip_sigs")
    block = text[text.index("{", start) + 1: text.index("};", start)]
    block = re.sub(r"//[^\n]*", "", block)
    return [[int(value, 16) if value.lower().startswith("0x") else int(value) for value in re.findall(r"0x[0-9a-fA-F]+|\d+", group)]
            for group in re.findall(r"\{([^{}]*)\}", block)]
