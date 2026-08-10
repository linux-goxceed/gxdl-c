#!/usr/bin/env python3
"""Protocol integration tests using only Python's standard library and a PTY."""

import os
import pty
import select
import struct
import subprocess
import sys
import tempfile
import time


GX_KEY = bytes((0x12, 0x34, 0x56, 0x78))


def make_loader(path):
    data = bytearray((i * 29 + 7) & 0xFF for i in range(0x2020))
    data[:4] = b"toob"
    data[4:6] = struct.pack("<H", 1)
    data[6:8] = struct.pack("<H", 0x6701)
    data[8:12] = struct.pack("<I", 115200)
    with open(path, "wb") as stream:
        stream.write(data)
    return bytes(data)


def read_exact(fd, count, timeout=8.0):
    result = bytearray()
    deadline = time.monotonic() + timeout
    while len(result) < count:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(f"PTY timeout at {len(result)}/{count} bytes")
        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            continue
        result.extend(os.read(fd, count - len(result)))
    return bytes(result)


def read_line(fd, timeout=5.0):
    result = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], deadline - time.monotonic())
        if not ready:
            continue
        byte = os.read(fd, 1)
        if byte == b"\n":
            line = bytes(result).rstrip(b"\r")
            if line:
                return line.decode("ascii")
            result.clear()
        else:
            result.extend(byte)
    raise AssertionError("PTY timeout waiting for command line")


def fragmented_write(fd, data, cuts=(1, 2, 3)):
    offset = 0
    for amount in cuts:
        if offset >= len(data):
            break
        os.write(fd, data[offset:offset + amount])
        offset += amount
        time.sleep(0.005)
    if offset < len(data):
        os.write(fd, data[offset:])


def spawn(binary, arguments, stdin=subprocess.PIPE):
    master, slave = pty.openpty()
    device = os.ttyname(slave)
    process = subprocess.Popen(
        [binary, "-d", device, *arguments],
        stdin=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    os.close(slave)
    time.sleep(0.15)
    return process, master


def finish(process, master, input_data=None, timeout=8):
    try:
        stdout, stderr = process.communicate(input_data, timeout=timeout)
    finally:
        os.close(master)
    if process.returncode != 0:
        raise AssertionError(
            f"gxdl-c exited {process.returncode}\nstdout={stdout!r}\nstderr={stderr!r}"
        )
    return stdout, stderr


def emulate_boot(binary, loader_path, loader, handshake, run_response=b"RUNGET"):
    process, master = spawn(binary, ["-b", loader_path])
    fragmented_write(master, handshake)
    stage1 = read_exact(master, 8197)
    assert stage1[:5] == b"\x59\x00\x08\x00\x00"
    assert stage1[5:-4] == loader[0x20:0x201C]
    assert stage1[-4:] == b"boot"
    fragmented_write(master, run_response)
    metadata = read_exact(master, 8, timeout=4)
    size = struct.unpack("<I", metadata[4:])[0]
    assert size == len(loader)
    stage2 = read_exact(master, size)
    expected = loader[:4] + loader[0x20:] + bytes(28)
    assert stage2 == expected
    assert struct.unpack("<H", metadata[:2])[0] == sum(expected) & 0xFFFF
    assert metadata[2:4] == b"\xc2\x00"
    fragmented_write(master, b"Boot OK\r\nboot> ")
    finish(process, master, timeout=18)


def text_command(binary, command, response=b"OK", extra_args=()):
    process, master = spawn(binary, [*extra_args, "-t", "nns", "-c", command])
    fragmented_write(master, b"boot> ")
    received = read_line(master)
    assert received == command
    fragmented_write(master, command.encode() + b"\r\n" + response + b"\r\nboot> ")
    stdout, _ = finish(process, master)
    assert response in stdout


def test_confirmation(binary):
    process, master = spawn(binary, ["-t", "nns", "-c", "flash erase LOGO"])
    stdout, stderr = process.communicate(b"n\n", timeout=5)
    os.close(master)
    assert process.returncode == 1
    assert b"Aborted" in stderr and not stdout
    text_command(binary, "flash erase LOGO", b"erased", ("-y",))


def test_serialdump(binary, directory):
    output = os.path.join(directory, "dump.bin")
    payload = bytearray((i * 13) & 0xFF for i in range(4096))
    payload[300:305] = b"~crc~"
    command = f"serialdump BOOT {len(payload)} {output}"
    wire_command = f"serialdump BOOT {len(payload)}"
    process, master = spawn(binary, ["-t", "nns", "-c", command])
    fragmented_write(master, b"boot> ")
    assert read_line(master) == wire_command
    response = wire_command.encode() + b"\r\n~s" + b"ta~" + payload
    fragmented_write(master, response, (2, 1, 7, 13))
    fragmented_write(master, b"~c" + b"rc~" + struct.pack("<I", 0x12345678) + b"~fin~")
    finish(process, master)
    with open(output, "rb") as stream:
        assert stream.read() == payload


def test_binary_write(binary, directory, otp=False):
    input_path = os.path.join(directory, "write.bin")
    payload = bytes((i * 19 + 5) & 0xFF for i in range(3073 if not otp else 333))
    with open(input_path, "wb") as stream:
        stream.write(payload)
    if otp:
        command = f"gx_otp write 4 {input_path}"
        prefix = f"gx_otp write 4 {len(payload)}"
    else:
        command = f"serialdown LOGO {input_path}"
        prefix = f"serialdown LOGO {len(payload)}"
    process, master = spawn(binary, ["-t", "nns", "-c", command])
    fragmented_write(master, b"boot> ")
    assert read_line(master) == prefix
    fragmented_write(master, prefix.encode() + b"\r\n~sta~")
    assert read_exact(master, len(payload)) == payload
    fragmented_write(master, b"~crc~")
    checksum = read_exact(master, 4)
    expected = sum(GX_KEY[i % 4] ^ byte for i, byte in enumerate(payload)) & 0xFFFFFFFF
    assert checksum == struct.pack(">I", expected)
    if otp:
        fragmented_write(master, b"~fin~")
    else:
        fragmented_write(master, b"~fin~\r\nboot> ")
    finish(process, master)


def test_config(binary, directory):
    config = os.path.join(directory, "commands.conf")
    with open(config, "w", encoding="utf-8") as stream:
        stream.write("# comment\nflash badinfo\nusbdump BOOT 16 usb.bin\n")
    top = f"load_conf_down {config} serial"
    process, master = spawn(binary, ["-t", "nns", "-c", top])
    fragmented_write(master, b"boot> ")
    assert read_line(master) == "flash badinfo"
    fragmented_write(master, b"flash badinfo\r\nnone\r\nboot> ")
    # The first response consumed the prompt, so the next wait nudges with a blank line.
    time.sleep(0.25)
    os.write(master, b"boot> ")
    assert read_line(master) == "usbdump BOOT 16 usb.bin"
    fragmented_write(master, b"usbdump BOOT 16 usb.bin\r\nfinish\r\nboot> ")
    finish(process, master)


def test_loopback(binary):
    process, master = spawn(binary, ["--loopback-test"])
    data = read_exact(master, len(b"LOOPBACK_TEST_12345"))
    assert data == b"LOOPBACK_TEST_12345"
    os.write(master, data)
    finish(process, master)


def test_cli(binary):
    result = subprocess.run([binary], capture_output=True)
    assert result.returncode == 0
    assert b"Usage: gxdl-c" in result.stdout
    assert b"Commands:" in result.stdout
    assert b"device is required" not in result.stderr
    result = subprocess.run([binary, "--list-loaders"], capture_output=True, check=True)
    assert b"loader" in result.stdout.lower()
    listing = result.stdout.decode("utf-8")
    if "Embedded loaders" in listing:
        names = [line.strip() for line in listing.splitlines()[1:] if line.strip()]
        assert names
        for name in names:
            probe = subprocess.run(
                [binary, "-d", "/dev/null", "-m", name], capture_output=True
            )
            assert probe.returncode == 1
            assert b"Loaded boot image" in probe.stderr
            assert b"Embedded loader not found" not in probe.stderr
    result = subprocess.run(
        [binary, "-d", "/dev/null", "-b", "a", "-m", "b"], capture_output=True
    )
    assert result.returncode == 2 and b"mutually exclusive" in result.stderr
    result = subprocess.run([binary, "-d", "/dev/null"], capture_output=True)
    assert result.returncode == 2 and b"requires" in result.stderr


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: pty_integration.py GXDL-C")
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="gxdl-test-") as directory:
        loader_path = os.path.join(directory, "test.boot")
        loader = make_loader(loader_path)
        variants = [
            b"\xb0\xb0\x58",
            b"\xb8\xb0\xff\x58",
            b"\x00\xb0\xb0\x58",
            b"\xb0\x30\xff\x58",
        ]
        for index, handshake in enumerate(variants):
            emulate_boot(binary, loader_path, loader, handshake,
                         b"RUN" if index == len(variants) - 1 else b"RUNGET")
        text_command(binary, "flash badinfo", b"no bad blocks")
        text_command(binary, "gx_otp tread 0 16", b"00 11 22 33")
        text_command(binary, "sflash_otp status", b"status: 0")
        text_command(binary, "sflash_otp getregion", b"region: 1")
        text_command(binary, "sflash_otp erase", b"finish")
        text_command(binary, "usbdump BOOT 16 usb.bin", b"finish")
        text_command(binary, "usbdown LOGO logo.bin", b"finish")
        text_command(binary, "gx_otp twrite 0 aabb", b"finish")
        test_serialdump(binary, directory)
        test_binary_write(binary, directory)
        test_binary_write(binary, directory, otp=True)
        test_config(binary, directory)
        test_confirmation(binary)
        test_loopback(binary)
        test_cli(binary)
    print("PTY integration tests: OK")


if __name__ == "__main__":
    main()
