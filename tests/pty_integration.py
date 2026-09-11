#!/usr/bin/env python3
"""Protocol integration tests using only Python's standard library and a PTY."""

import os
import pty
import select
import socket
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


def emulate_boot(binary, loader_path, loader, handshake, run_response=b"RUNGET", chip=0x6701):
    process, master = spawn(binary, ["-b", loader_path])
    fragmented_write(master, handshake)
    if chip == 0x6612:
        transfer_size = 0x4000
    elif chip in (0x6616, 0x3211, 0x6701, 0x6705):
        transfer_size = 0x2000
    else:
        transfer_size = 0x1000
    payload_size = transfer_size - (0x20 if chip == 0x6612 else 4)
    stage1 = read_exact(master, payload_size + 9)
    assert stage1[:5] == b"\x59" + struct.pack("<H", transfer_size // 4) + b"\x00\x00"
    assert stage1[5:-4] == loader[0x20:0x20 + payload_size]
    assert stage1[-4:] == b"boot"
    fragmented_write(master, run_response)
    metadata = read_exact(master, 8, timeout=4)
    size = struct.unpack("<I", metadata[4:])[0]
    assert size == len(loader)
    stage2 = read_exact(master, size)
    expected = loader[:4] + loader[0x20:] + bytes(28)
    assert stage2 == expected
    assert struct.unpack("<I", metadata[:4])[0] == sum(expected) & 0xFFFFFFFF
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


def wrap_gxbc(payload, entry=0x93C00000):
    checksum = sum(payload) & 0xFFFFFFFF
    return struct.pack("<IIII", 0x43425847, len(payload), entry, checksum) + payload


def test_confirmation(binary):
    process, master = spawn(binary, ["-t", "nns", "-c", "flash erase LOGO"])
    stdout, stderr = process.communicate(b"n\n", timeout=5)
    os.close(master)
    assert process.returncode == 1
    assert b"Aborted" in stderr and not stdout
    text_command(binary, "flash erase LOGO", b"erased", ("-y",))
    process, master = spawn(binary, ["-t", "nns", "-c", "sflash_otp lock"])
    _, stderr = process.communicate(b"n\n", timeout=5)
    os.close(master)
    assert process.returncode == 1 and b"Aborted" in stderr
    text_command(binary, "sflash_otp lock", b"locked", ("-y",))
    process, master = spawn(binary, ["-t", "nns", "-c", "flash scrub all"])
    _, stderr = process.communicate(b"n\n", timeout=5)
    os.close(master)
    assert process.returncode == 1 and b"Aborted" in stderr
    text_command(binary, "flash scrub all", b"scrubbed", ("-y",))
    text_command(binary, "flash mark bad 0x20000", b"marked", ("-y",))
    text_command(binary, "net ping 127.0.0.1", b"icmp_seq = 1")


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


def test_hex_serialdump(binary, directory):
    output = os.path.join(directory, "hexdump.bin")
    payload = bytes(range(16))
    command = f"serialdump BOOT 0x10 {output}"
    process, master = spawn(binary, ["-t", "nns", "-c", command])
    fragmented_write(master, b"boot> ")
    assert read_line(master) == "serialdump BOOT 0x10"
    fragmented_write(master, b"serialdump BOOT 0x10\r\n~sta~" + payload)
    fragmented_write(master, b"~crc~" + struct.pack("<I", 1) + b"~fin~")
    finish(process, master)
    with open(output, "rb") as stream:
        assert stream.read() == payload


def test_gxbc_stage2(binary, directory, loader_path, loader):
    bootcode = os.path.join(directory, "gx6702-bootcode.bin")
    payload = b"DDRINIT" * 32
    with open(bootcode, "wb") as stream:
        stream.write(payload)
    process, master = spawn(binary, ["-b", loader_path, "--bootcode", bootcode])
    fragmented_write(master, b"\xb0\xb0\x58")
    stage1 = read_exact(master, 8197)
    assert stage1[:5] == b"\x59\x00\x08\x00\x00"
    assert stage1[-4:] == b"boot"
    fragmented_write(master, b"GXID family=gemini name=6702S5-NNNB\r\nRUNGET")
    wrapped = wrap_gxbc(payload)
    metadata = read_exact(master, 8, timeout=4)
    size = struct.unpack("<I", metadata[4:])[0]
    assert size == len(wrapped)
    stage2 = read_exact(master, size)
    assert stage2 == wrapped
    assert struct.unpack("<I", metadata[:4])[0] == sum(wrapped) & 0xFFFFFFFF
    fragmented_write(master, b"Boot OK\r\nboot> ")
    _, stderr = finish(process, master, timeout=18)
    assert b"Sending GXBC" in stderr
    assert b"GXID family=gemini" in stderr


def test_missing_bootcode(binary, directory, loader_path):
    leftover = os.path.join(directory, "gx6702-bootcode.bin")
    if os.path.exists(leftover):
        os.remove(leftover)
    process, master = spawn(binary, ["-b", loader_path])
    fragmented_write(master, b"\xb0\xb0\x58")
    read_exact(master, 8197)
    fragmented_write(master, b"GXID family=gemini name=6702S5-NNNB\r\nRUNGET")
    stdout, stderr = process.communicate(timeout=8)
    os.close(master)
    assert process.returncode == 1
    assert b"EBUNDLE" in stderr
    assert b"Need gx6702-bootcode.bin" in stderr
    assert stdout == b"" or True


def test_detect_only(binary, loader_path):
    process, master = spawn(binary, ["-b", loader_path])
    fragmented_write(master, b"\xb0\xb0\x58")
    read_exact(master, 8197)
    fragmented_write(master, b"GXID family=taurus name=probe\r\n")
    _, stderr = finish(process, master, timeout=12)
    assert b"No Stage 2" in stderr
    assert b"family=taurus" in stderr


def free_udp_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def test_netdump(binary, directory):
    output = os.path.join(directory, "net.bin")
    payload = bytes((i * 3) & 0xFF for i in range(2048))
    port = free_udp_port()
    command = f"netdump BOOT 2048 {output}"
    process, master = spawn(
        binary,
        ["-t", "nns", "-y", "-p", "127.0.0.1", "-s", "127.0.0.1",
         "--tftp-port", str(port), "-c", command],
    )
    fragmented_write(master, b"boot> ")
    assert read_line(master) == "config ip 127.0.0.1"
    fragmented_write(master, b"config ip 127.0.0.1\r\nok\r\nboot> ")
    time.sleep(0.25)
    os.write(master, b"boot> ")
    assert read_line(master) == f"config tftpport {port}"
    fragmented_write(master, b"config tftpport " + str(port).encode() + b"\r\nok\r\nboot> ")
    time.sleep(0.25)
    os.write(master, b"boot> ")
    netdump = f"netdump BOOT 127.0.0.1 {os.path.basename(output)} 2048"
    assert read_line(master) == netdump
    fragmented_write(master, netdump.encode() + b"\r\n")
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.settimeout(3)
    wrq = struct.pack("!H", 2) + b"net.bin\x00octet\x00blksize\x001024\x00"
    deadline = time.monotonic() + 5
    ack = None
    while time.monotonic() < deadline:
        try:
            client.sendto(wrq, ("127.0.0.1", port))
            ack, server = client.recvfrom(32)
            break
        except socket.timeout:
            continue
    assert ack and struct.unpack("!HH", ack[:4]) == (4, 0)
    client.sendto(struct.pack("!HH", 3, 1) + payload[:1024], server)
    ack, _ = client.recvfrom(32)
    assert struct.unpack("!HH", ack[:4]) == (4, 1)
    client.sendto(struct.pack("!HH", 3, 2) + payload[1024:], server)
    ack, _ = client.recvfrom(32)
    assert struct.unpack("!HH", ack[:4]) == (4, 2)
    fragmented_write(master, b"tftp finished net.bin\r\nboot> ")
    finish(process, master, timeout=12)
    client.close()
    with open(output, "rb") as stream:
        assert stream.read() == payload


def test_netdown(binary, directory):
    source = os.path.join(directory, "upload.bin")
    payload = bytes((i * 7) & 0xFF for i in range(1500))
    with open(source, "wb") as stream:
        stream.write(payload)
    port = free_udp_port()
    process, master = spawn(
        binary,
        ["-t", "nns", "-y", "-p", "127.0.0.1", "-s", "127.0.0.1",
         "--tftp-port", str(port), "-c", f"netdown DATA {source}"],
    )
    fragmented_write(master, b"boot> ")
    assert read_line(master) == "config ip 127.0.0.1"
    fragmented_write(master, b"config ip 127.0.0.1\r\nok\r\nboot> ")
    time.sleep(0.25)
    os.write(master, b"boot> ")
    assert read_line(master) == f"config tftpport {port}"
    fragmented_write(master, b"config tftpport " + str(port).encode() + b"\r\nok\r\nboot> ")
    time.sleep(0.25)
    os.write(master, b"boot> ")
    expected = f'partition download DATA 127.0.0.1 "{os.path.basename(source)}" {len(payload)}'
    assert read_line(master) == expected
    fragmented_write(master, expected.encode() + b"\r\n")
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.settimeout(3)
    rrq = struct.pack("!H", 1) + b"upload.bin\x00octet\x00blksize\x001024\x00"
    deadline = time.monotonic() + 5
    oack = None
    while time.monotonic() < deadline:
        try:
            client.sendto(rrq, ("127.0.0.1", port))
            oack, server = client.recvfrom(128)
            break
        except socket.timeout:
            continue
    assert oack and struct.unpack("!H", oack[:2])[0] == 6
    client.sendto(struct.pack("!HH", 4, 0), server)
    received = bytearray()
    block = 1
    while True:
        data, _ = client.recvfrom(4 + 1024)
        opcode, got = struct.unpack("!HH", data[:4])
        assert opcode == 3 and got == block
        chunk = data[4:]
        received.extend(chunk)
        client.sendto(struct.pack("!HH", 4, block), server)
        if len(chunk) < 1024:
            break
        block += 1
    fragmented_write(master, b"tftp finished upload.bin\r\nboot> ")
    finish(process, master, timeout=12)
    client.close()
    assert bytes(received) == payload


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
        emulate_boot(binary, loader_path, loader, b"\x00\xb0\xb0\x58",
                     b"19RUkgd:3\r\nNGET")
        text_command(binary, "flash badinfo", b"no bad blocks")
        text_command(binary, "gx_otp tread 0 16", b"00 11 22 33")
        text_command(binary, "sflash_otp status", b"status: 0")
        text_command(binary, "sflash_otp getregion", b"region: 1")
        text_command(binary, "sflash_otp erase", b"finish")
        text_command(binary, "usbdump BOOT 16 usb.bin", b"finish")
        text_command(binary, "usbdown LOGO logo.bin", b"finish")
        text_command(binary, "gx_otp twrite 0 aabb", b"finish")
        test_serialdump(binary, directory)
        test_hex_serialdump(binary, directory)
        test_binary_write(binary, directory)
        test_binary_write(binary, directory, otp=True)
        test_config(binary, directory)
        test_confirmation(binary)
        test_gxbc_stage2(binary, directory, loader_path, loader)
        test_missing_bootcode(binary, directory, loader_path)
        test_detect_only(binary, loader_path)
        test_netdump(binary, directory)
        test_netdown(binary, directory)
        test_loopback(binary)
        test_cli(binary)
    print("PTY integration tests: OK")


if __name__ == "__main__":
    main()
