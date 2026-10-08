def decompress(data):
    if data[0] != 0x11:
        raise ValueError("not an LZ11 stream")
    size = int.from_bytes(data[1:4], "little")
    pos = 4
    if size == 0:
        size = int.from_bytes(data[4:8], "little")
        pos = 8
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(7, -1, -1):
            if len(out) >= size:
                break
            if not (flags >> bit) & 1:
                out.append(data[pos])
                pos += 1
                continue
            b0 = data[pos]
            indicator = b0 >> 4
            if indicator == 0:
                length = ((b0 << 4) | (data[pos + 1] >> 4)) + 0x11
                disp = (((data[pos + 1] & 0xF) << 8) | data[pos + 2]) + 1
                pos += 3
            elif indicator == 1:
                length = (((b0 & 0xF) << 12) | (data[pos + 1] << 4) | (data[pos + 2] >> 4)) + 0x111
                disp = (((data[pos + 2] & 0xF) << 8) | data[pos + 3]) + 1
                pos += 4
            else:
                length = indicator + 1
                disp = (((b0 & 0xF) << 8) | data[pos + 1]) + 1
                pos += 2
            for _ in range(length):
                out.append(out[-disp])
    return bytes(out[:size])
