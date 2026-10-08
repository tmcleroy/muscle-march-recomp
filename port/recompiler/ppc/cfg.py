RD_DESTINATION = {
    "addi", "addis", "addic", "addic.", "subfic", "mulli", "add", "addc", "adde", "addze",
    "addme", "subf", "subfc", "subfe", "subfze", "subfme", "neg", "mullw", "mulhw", "mulhwu",
    "divw", "divwu", "lwz", "lwzx", "lwzu", "lwzux", "lbz", "lbzx", "lbzu", "lbzux", "lhz",
    "lhzx", "lhzu", "lhzux", "lha", "lhax", "lhau", "lhaux", "mfspr", "mfcr", "mftb",
}

RA_DESTINATION = {
    "ori", "oris", "xori", "xoris", "andi.", "andis.", "and", "or", "xor", "nand", "nor",
    "eqv", "andc", "orc", "extsb", "extsh", "cntlzw", "slw", "srw", "sraw", "srawi",
    "rlwinm", "rlwimi", "rlwnm",
}

MAX_TABLE_ENTRIES = 512
MFCTR_LOOKBACK = 12
LOAD_LOOKBACK = 8
BOUND_LOOKBACK = 64
CONSTANT_LOOKBACK = 256


def destination(instr):
    if instr.mn in RD_DESTINATION:
        return instr.rt
    if instr.mn in RA_DESTINATION:
        return instr.ra
    return None


def resolve_constant(instrs, index, register, depth=0):
    if depth > 4:
        return None
    for k in range(index - 1, max(index - CONSTANT_LOOKBACK, -1), -1):
        instr = instrs[k]
        if destination(instr) != register:
            continue
        if instr.mn == "addis":
            base = 0 if instr.ra == 0 else resolve_constant(instrs, k, instr.ra, depth + 1)
            return None if base is None else (base + (instr.simm << 16)) & 0xFFFFFFFF
        if instr.mn == "addi":
            base = 0 if instr.ra == 0 else resolve_constant(instrs, k, instr.ra, depth + 1)
            return None if base is None else (base + instr.simm) & 0xFFFFFFFF
        if instr.mn == "ori":
            base = resolve_constant(instrs, k, instr.rs, depth + 1)
            return None if base is None else base | instr.uimm
        return None
    return None


def find_bound(instrs, index):
    for k in range(index - 1, max(index - BOUND_LOOKBACK, -1), -1):
        if instrs[k].mn == "cmpli":
            return instrs[k].uimm + 1
    return None


def find_jump_table(instrs, index, dol):
    branch = instrs[index]
    if branch.mn != "bcctr" or branch.lk or branch.bo != 20:
        return None
    mtctr = None
    for k in range(index - 1, max(index - MFCTR_LOOKBACK, -1), -1):
        if instrs[k].mn == "mtspr" and instrs[k].spr == 9:
            mtctr = k
            break
    if mtctr is None:
        return None
    loaded = instrs[mtctr].rs
    load = None
    for k in range(mtctr - 1, max(mtctr - LOAD_LOOKBACK, -1), -1):
        if instrs[k].mn == "lwzx" and instrs[k].rt == loaded:
            load = k
            break
    if load is None:
        return None
    base = None
    for register in (instrs[load].ra, instrs[load].rb):
        base = resolve_constant(instrs, load, register)
        if base is not None:
            break
    if base is None:
        return None
    count = find_bound(instrs, load)
    limit = count if count is not None else MAX_TABLE_ENTRIES
    targets = []
    for n in range(limit):
        word = dol.word_at(base + 4 * n)
        valid = word is not None and word % 4 == 0 and dol.code_section_end(word) is not None
        if not valid:
            if count is not None:
                return None
            break
        targets.append(word)
    return targets or None


def find_jump_tables(instrs, dol):
    tables = {}
    unresolved = 0
    for index, instr in enumerate(instrs):
        if instr.mn == "bcctr" and not instr.lk and instr.bo == 20:
            table = find_jump_table(instrs, index, dol)
            if table is None:
                unresolved += 1
            else:
                tables[instr.addr] = table
    return tables, unresolved


def find_labels(instrs, start, end, jump_tables):
    labels = set()
    for instr in instrs:
        if instr.mn in ("b", "bc") and not instr.lk and start <= instr.target < end:
            labels.add(instr.target)
    for targets in jump_tables.values():
        labels.update(target for target in targets if start <= target < end)
    return labels


def code_constants(instrs):
    values = set()
    for index, instr in enumerate(instrs):
        if instr.mn == "addi" and instr.ra != 0:
            base = resolve_constant(instrs, index, instr.ra)
            if base is not None:
                values.add((base + instr.simm) & 0xFFFFFFFF)
        elif instr.mn == "ori":
            base = resolve_constant(instrs, index, instr.rs)
            if base is not None:
                values.add(base | instr.uimm)
    return values
