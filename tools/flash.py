#!/usr/bin/env python3
"""Fail-closed STM32F407 image checking, backup, flashing and independent readback.
No shell expansion, mass erase, RDP modification, option-byte write or automatic run.
"""
from __future__ import annotations
import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from source_fingerprint import fingerprint

DEVICE_ID = 0x413
FLASH_BASE = 0x08000000
FLASH_SIZE_REGISTER = 0x1FFF7A22
IDCODE_REGISTER = 0xE0042000
VECTOR_WORDS = 16 + 82  # CMSIS STM32F407: highest IRQ FPU_IRQn = 81
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
ROOT = Path(__file__).resolve().parent.parent

class Failure(RuntimeError):
    pass

def number(value):
    if isinstance(value, bool):
        raise ValueError("Boolean is not a memory address/size")
    if isinstance(value,int):
        return value
    if isinstance(value,str):
        return int(value,0)
    raise ValueError("Manifest addresses/sizes must be integers or strings")

def image_info(image: Path, manifest: Path):
    try:
        metadata = json.loads(manifest.read_text())
        content = image.read_bytes()
        size = number(metadata["image_size"])
        capacity = number(metadata["flash_size"])
        base = number(metadata["flash_base"])
        device = number(metadata["device_id"])
        digest = metadata["sha256"]
        filename = metadata["filename"]
    except (OSError, ValueError, TypeError, KeyError) as error:
        raise Failure(f"Cannot validate image/manifest: {error}") from error
    if image.suffix.lower() != ".bin" or filename != image.name:
        raise Failure("Use the .bin file named by the manifest")
    if device != DEVICE_ID or base != FLASH_BASE or capacity not in (512*1024, 1024*1024):
        raise Failure("Manifest target must be F405/F407 ID 0x413, flash base 0x08000000 and documented 512/1024 KiB")
    if size != len(content) or not VECTOR_WORDS*4 < size <= capacity:
        raise Failure("Image length is invalid or exceeds manifest Flash capacity")
    if not isinstance(digest, str) or re.fullmatch(r"[a-fA-F0-9]{64}", digest) is None or hashlib.sha256(content).hexdigest() != digest.lower():
        raise Failure("Image SHA-256 differs from the build manifest")
    stack, reset = struct.unpack_from("<II", content)
    # F407 SRAM1/2 is 0x20000000..0x20020000; CCM RAM is a distinct 64-KiB region.
    stack_valid = (0x20000000 < stack <= 0x20020000) or (0x10000000 < stack <= 0x10010000)
    if not stack_valid or stack % 8 != 0:
        raise Failure("Initial stack vector is outside F407 SRAM/CCM or not 8-byte aligned")
    if reset & 1 == 0 or not base + VECTOR_WORDS*4 <= (reset & ~1) < base + size:
        raise Failure("Reset vector must be Thumb code inside the validated image")
    vectors=struct.unpack_from(f"<{VECTOR_WORDS}I",content)
    for index,handler in enumerate(vectors[1:],start=1):
        if handler!=0 and (handler & 1 == 0 or not base+VECTOR_WORDS*4 <= (handler & ~1) < base+size):
            raise Failure(f"ISR vector {index} is not Thumb code inside the standalone image")
    return content, capacity, digest.lower()

def check_source_revision(manifest: Path):
    metadata = json.loads(manifest.read_text())
    if metadata.get("source_fingerprint") != fingerprint(ROOT):
        raise Failure("Image is from an older or unknown source revision. Build the current firmware first; do not reuse historical BIN/HEX files")
    selected = metadata.get("deployment_source")
    expected = metadata.get("deployment_sha256")
    relative = metadata.get("deployment_relative")
    if relative is not None:
        if not isinstance(relative, str) or Path(relative).is_absolute():
            raise Failure("Deployment relative path must stay within the source package")
        candidate = (ROOT / relative).resolve()
        if not candidate.is_relative_to(ROOT.resolve()):
            raise Failure("Deployment relative path escapes the source package")
        selected = str(candidate)
    if selected is not None:
        try:
            actual = hashlib.sha256(Path(selected).read_bytes()).hexdigest()
        except (OSError, TypeError, ValueError) as error:
            raise Failure("The deployment profile used by this image is unavailable; configure and build using the local reviewed profile") from error
        if actual != expected:
            raise Failure("Deployment profile changed after the image was built; configure and build again before flashing")
    elif expected is not None:
        raise Failure("Configured image has no verifiable deployment source path")

def locate_programmer(requested):
    candidates = []
    if requested:
        candidates.append(Path(requested).expanduser())
    elif os.environ.get("STM32_PROGRAMMER_CLI"):
        candidates.append(Path(os.environ["STM32_PROGRAMMER_CLI"]).expanduser())
    else:
        located = shutil.which("STM32_Programmer_CLI")
        if located:
            candidates.append(Path(located))
        candidates.extend([
            Path("/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI"),
            Path("/Applications/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI"),
            Path.home()/"STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
        ])
        candidates.extend(sorted(Path("/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins").glob("com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.*/tools/bin/STM32_Programmer_CLI")))
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise Failure("STM32_Programmer_CLI was not found; install STM32CubeProgrammer or pass --programmer PATH")

class Programmer:
    def __init__(self, executable, timeout):
        self.executable = executable
        self.timeout = timeout
        self.serial = ""

    def run(self, arguments):
        try:
            process = subprocess.Popen([str(self.executable), *arguments], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True, errors="replace", start_new_session=True)
            try:
                output, _ = process.communicate(timeout=self.timeout)
            except subprocess.TimeoutExpired as error:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
                raise Failure("Programmer deadline exceeded; outcome is unknown; no command will be retried") from error
        except OSError as error:
            raise Failure(f"Cannot start programmer: {error}") from error
        cleaned = ANSI.sub("", output)
        if process.returncode != 0 or re.search(r"(?im)^\s*(?:error|fatal)\s*:", cleaned):
            # Device output is intentionally not echoed: the summary is enough for this tool.
            raise Failure(f"Programmer operation failed (exit {process.returncode}); inspect USB, target power, SWD and read protection manually")
        return cleaned

    def select(self, serial):
        listing = self.run(["-l", "st-link-only"])
        detected = list(dict.fromkeys(re.findall(r"(?im)^\s*(?:ST-?LINK\s+SN|Serial\s+number)\s*:\s*([A-Za-z0-9]+)", listing)))
        if not detected:
            raise Failure("No ST-LINK serial number found; no target connection attempted")
        if serial:
            if serial not in detected:
                raise Failure("Requested ST-LINK serial was not found")
            self.serial = serial
        elif len(detected) == 1:
            self.serial = detected[0]
        else:
            raise Failure("Several ST-LINK probes found; select one with --stlink-sn")

    def connection(self):
        return ["-c", "port=SWD", f"sn={self.serial}", "mode=NORMAL", "freq=1000"]

    def upload(self, address, size, destination):
        self.run([*self.connection(), "-u", f"0x{address:08x}", str(size), str(destination)])
        try:
            result = destination.read_bytes()
        except OSError as error:
            raise Failure("Programmer reported upload completion but no readback file exists") from error
        if len(result) != size:
            raise Failure("Programmer readback file length differs from requested bytes")
        return result

    def inspect(self, capacity, temporary):
        connected = self.run(self.connection())
        reported = re.findall(r"(?i)Device\s+ID\s*:\s*(0x[0-9a-f]+)", connected)
        if len(reported) != 1 or int(reported[0], 16) != DEVICE_ID:
            raise Failure("Connected MCU family is unknown or differs from 0x413; refusing memory operations")
        idcode = self.upload(IDCODE_REGISTER, 4, temporary/"idcode.bin")
        if int.from_bytes(idcode,"little") & 0xFFF != DEVICE_ID:
            raise Failure("DBGMCU_IDCODE register differs from STM32F405/F407 family 0x413")
        actual = int.from_bytes(self.upload(FLASH_SIZE_REGISTER, 2, temporary/"flash_size.bin"),"little")*1024
        if actual != capacity:
            raise Failure(f"Hardware Flash size {actual} differs from build manifest {capacity}; refusing write")

@contextmanager
def probe_lock(serial):
    directory = Path(tempfile.gettempdir())/f"eurobot-next-programmer-{os.getuid()}"
    directory.mkdir(mode=0o700, exist_ok=True)
    name = hashlib.sha256(serial.encode()).hexdigest()[:24]
    descriptor = os.open(directory/f"{name}.lock", os.O_RDWR|os.O_CREAT|getattr(os,"O_NOFOLLOW",0),0o600)
    try:
        try:
            fcntl.flock(descriptor,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise Failure("This ST-LINK is already owned by another Eurobot programming operation") from error
        yield
    finally:
        os.close(descriptor)

def backup(programmer, capacity, directory):
    directory.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    # mkstemp reserves the destination uniquely; CubeProgrammer writes a .bin path.
    descriptor, name = tempfile.mkstemp(prefix=f"flash-{stamp}-",suffix=".bin",dir=directory)
    os.close(descriptor)
    path = Path(name)
    try:
        content = programmer.upload(FLASH_BASE,capacity,path)
    except Failure:
        path.unlink(missing_ok=True)
        raise Failure("Existing Flash could not be fully backed up; write refused. Do not remove RDP: that can erase the device")
    sidecar = {"device_id":hex(DEVICE_ID),"flash_base":hex(FLASH_BASE),"flash_size":capacity,
               "image_size":len(content),"sha256":hashlib.sha256(content).hexdigest(),"filename":path.name,
               "created_utc":stamp,"scope":"internal Flash bytes only; no option bytes, external memory or RAM"}
    path.with_suffix(".json").write_text(json.dumps(sidecar,indent=2)+"\n")
    print(f"Read backup saved: {path} ({len(content)} bytes, SHA-256 {sidecar['sha256']})")
    return path

def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation",choices=("flash","verify"))
    parser.add_argument("--image",type=Path,default=ROOT/"build/stm32-debug/firmware/eurobot_next.bin")
    parser.add_argument("--manifest",type=Path)
    parser.add_argument("--programmer",help="Path to STM32_Programmer_CLI executable")
    parser.add_argument("--stlink-sn",help="Select a specific enumerated probe; mandatory when several are connected")
    parser.add_argument("--allow-flash",action="store_true",help="Explicitly permit backup and writing only this validated image")
    parser.add_argument("--confirm-no-custom-bootloader",action="store_true",help="Confirm application starts at 0x08000000 and no custom bootloader occupies that region")
    parser.add_argument("--allow-connect","--probe",dest="allow_connect",action="store_true",help="For verify: permit SWD connection and readback; never writes Flash")
    parser.add_argument("--backup-dir",type=Path,default=ROOT/"backups")
    parser.add_argument("--timeout",type=int,default=60,help="Deadline per programmer operation (5..180 seconds)")
    result=parser.parse_args()
    if not 5 <= result.timeout <= 180:
        parser.error("--timeout must be 5..180 seconds")
    if result.operation=="verify" and result.allow_flash:
        parser.error("verify never accepts --allow-flash; use --allow-connect")
    return result

def main():
    args=arguments()
    image=args.image.expanduser().resolve()
    manifest=args.manifest.expanduser().resolve() if args.manifest else image.with_suffix(".manifest.json")
    content,capacity,digest=image_info(image,manifest)
    check_source_revision(manifest)
    print(f"Local image validated: {len(content)} bytes, SHA-256 {digest}")
    permitted=args.allow_flash if args.operation=="flash" else args.allow_connect
    if not permitted:
        print("Offline check only; no programmer or target access. Add --allow-flash for flashing or --allow-connect for readback verification.")
        return 0
    if args.operation=="flash" and not args.confirm_no_custom_bootloader:
        raise Failure("Confirm application occupies 0x08000000 with --confirm-no-custom-bootloader; unknown/custom bootloader layouts require separate review")
    programmer=Programmer(locate_programmer(args.programmer),args.timeout)
    programmer.select(args.stlink_sn)
    with probe_lock(programmer.serial), tempfile.TemporaryDirectory(prefix="eurobot-verify-") as temporary_name:
        temporary=Path(temporary_name)
        programmer.inspect(capacity,temporary)
        if args.operation=="flash":
            backup(programmer,capacity,args.backup_dir.expanduser().resolve())
            # Revalidate and copy into a private directory to avoid a changed build file during programming.
            current,again,digest_again=image_info(image,manifest)
            check_source_revision(manifest)
            if current!=content or again!=capacity or digest_again!=digest:
                raise Failure("Image or manifest changed during preflight; write refused")
            snapshot=temporary/"validated_image.bin"
            snapshot.write_bytes(content)
            programmer.run([*programmer.connection(),"-w",str(snapshot),f"0x{FLASH_BASE:08x}","-v","-halt"])
        readback=programmer.upload(FLASH_BASE,len(content),temporary/"readback.bin")
        if readback!=content or hashlib.sha256(readback).hexdigest()!=digest:
            raise Failure("Readback differs from the validated image; hardware must remain isolated")
        print("Independent readback matches image. No start/reset or protection changes were requested.")
    return 0

if __name__=="__main__":
    try:
        raise SystemExit(main())
    except (Failure,OSError,ValueError) as error:
        print(f"FAILED: {error}",file=sys.stderr)
        raise SystemExit(1)
