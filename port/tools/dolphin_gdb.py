import argparse
import socket
import subprocess
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recompiler"))

import game

GAME = game.load()
DEFAULT_DOLPHIN = ROOT / "reference" / "dolphin" / "Dolphin.exe"
DEFAULT_GAME = GAME.disc
SPECIAL_REGISTERS = {"pc": 0x40, "msr": 0x41, "cr": 0x42, "lr": 0x43, "ctr": 0x44, "xer": 0x45}
FIRST_FPR = 0x20

GPR_COUNT = 32
FPR_COUNT = 32
SPECIAL = list(SPECIAL_REGISTERS)
CHUNK = 1024


class GdbError(Exception):
    pass


class Session:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = b""

    def close(self):
        self.sock.close()

    def read_byte(self):
        if not self.buffer:
            data = self.sock.recv(4096)
            if not data:
                raise GdbError("connection closed")
            self.buffer = data
        value = self.buffer[:1]
        self.buffer = self.buffer[1:]
        return value

    def read_packet(self):
        while self.read_byte() != b"$":
            pass
        body = b""
        while True:
            ch = self.read_byte()
            if ch == b"#":
                break
            body += ch
        self.read_byte()
        self.read_byte()
        self.sock.sendall(b"+")
        return body.decode("ascii")

    def send(self, payload, wait=True):
        checksum = sum(payload.encode("ascii")) & 0xFF
        self.sock.sendall(f"${payload}#{checksum:02x}".encode("ascii"))
        if not wait:
            return None
        while True:
            ch = self.read_byte()
            if ch == b"+":
                break
            if ch == b"-":
                raise GdbError("packet rejected")
        return self.read_packet()

    def interrupt(self):
        self.sock.sendall(b"\x03")

    def memory(self, address, length):
        data = b""
        while length > 0:
            size = min(length, CHUNK)
            reply = self.send(f"m{address:x},{size:x}")
            if reply.startswith("E"):
                raise GdbError(f"read error at {address:08x}")
            data += bytes.fromhex(reply)
            address += size
            length -= size
        return data

    def write_memory(self, address, data):
        reply = self.send(f"M{address:x},{len(data):x}:{data.hex()}")
        if reply != "OK":
            raise GdbError(f"write error at {address:08x}: {reply}")

    def registers(self):
        raw = bytes.fromhex(self.send("g"))
        names = {}
        for i in range(GPR_COUNT):
            names[f"r{i}"] = struct.unpack(">I", raw[4 * i:4 * i + 4])[0]
        for name, number in SPECIAL_REGISTERS.items():
            names[name] = int(self.send(f"p{number:x}"), 16)
        for i in range(FPR_COUNT):
            names[f"f{i}"] = struct.unpack(">d", bytes.fromhex(self.send(f"p{FIRST_FPR + i:x}")))[0]
        return names

    def wait_stop(self, limit):
        deadline = time.time() + limit
        self.sock.settimeout(1.0)
        try:
            while time.time() < deadline:
                try:
                    return self.read_packet()
                except socket.timeout:
                    continue
        finally:
            self.sock.settimeout(None)
        self.interrupt()
        self.sock.settimeout(5.0)
        return self.read_packet()


def parse_int(text):
    return int(text, 0) if text.lower().startswith("0x") else int(text, 16)


def show_registers(regs, only):
    names = only or [f"r{i}" for i in range(GPR_COUNT)] + SPECIAL
    for name in names:
        if name in regs:
            value = regs[name]
            print(f"{name:5} {value:08x}" if isinstance(value, int) else f"{name:5} {value}")


def hexdump(address, data):
    for offset in range(0, len(data), 16):
        row = data[offset:offset + 16]
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        print(f"{address + offset:08x}  {row.hex(' '):47}  {text}")


def run(session, command):
    words = command.split()
    name, args = words[0], words[1:]
    if name == "regs":
        show_registers(session.registers(), args)
    elif name == "mem":
        address, length = parse_int(args[0]), int(args[1], 0)
        hexdump(address, session.memory(address, length))
    elif name == "u32":
        address = parse_int(args[0])
        count = int(args[1], 0) if len(args) > 1 else 1
        data = session.memory(address, 4 * count)
        for i in range(count):
            print(f"{address + 4 * i:08x} {struct.unpack('>I', data[4 * i:4 * i + 4])[0]:08x}")
    elif name == "write":
        session.write_memory(parse_int(args[0]), bytes.fromhex(args[1]))
        print("ok")
    elif name in ("break", "unbreak", "watch", "unwatch", "rwatch", "awatch"):
        kinds = {"break": ("Z", 0), "unbreak": ("z", 0), "watch": ("Z", 2), "unwatch": ("z", 2), "rwatch": ("Z", 3), "awatch": ("Z", 4)}
        prefix, kind = kinds[name]
        address = parse_int(args[0])
        size = int(args[1], 0) if len(args) > 1 else 4
        print(session.send(f"{prefix}{kind},{address:x},{size:x}"))
    elif name == "step":
        print(session.send("s"))
    elif name == "continue":
        limit = float(args[0]) if args else 30.0
        session.send("c", wait=False)
        print(session.wait_stop(limit))
    elif name == "halt":
        session.interrupt()
        print(session.read_packet())
    elif name == "raw":
        print(session.send(args[0]))
    elif name == "sleep":
        time.sleep(float(args[0]))
    else:
        raise GdbError(f"unknown command {command}")


def launch(args):
    subprocess.run(["taskkill", "/F", "/IM", Path(args.dolphin).name], capture_output=True)
    time.sleep(1.5)
    process = subprocess.Popen([args.dolphin, "-b", "-e", args.game], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 40
    while time.time() < deadline:
        probe = socket.socket()
        try:
            probe.bind(("0.0.0.0", args.port))
        except OSError:
            break
        finally:
            probe.close()
        time.sleep(0.5)
    else:
        raise GdbError("the Dolphin GDB port did not open")
    return process


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2159)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--launch", action="store_true")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--dolphin", default=str(DEFAULT_DOLPHIN))
    parser.add_argument("--game", default=str(DEFAULT_GAME))
    parser.add_argument("commands", nargs="+")
    args = parser.parse_args()
    process = launch(args) if args.launch else None
    session = Session(args.host, args.port, args.timeout)
    try:
        print(session.send("?"))
        for command in args.commands:
            print(f"> {command}")
            run(session, command)
    finally:
        session.close()
        if process is not None and not args.keep:
            process.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
