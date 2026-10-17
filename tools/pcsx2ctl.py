#!/usr/bin/env python3
"""Bounded, reconnectable client for PCSX2's RAW agent console (protocol 1)."""
# SPDX-License-Identifier: GPL-3.0+

import argparse
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import uuid
import hashlib
import shutil
import configparser


class ConsoleError(Exception):
    def __init__(self, reason, code=2, **details):
        super().__init__(reason)
        self.result = dict(ok=False, reason=reason, **details)
        self.code = code


def emit(value, stream=None):
    print(json.dumps(value, ensure_ascii=True), file=stream or sys.stdout, flush=True)


def exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise ConsoleError("truncated_response", expected=size, received=len(data))
    return data


class Client:
    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        self.manifest = json.loads((self.directory / "run.json").read_text(encoding="utf-8"))
        if self.manifest.get("protocol") != 1 or self.manifest.get("host") != "127.0.0.1":
            raise ConsoleError("unsupported_manifest")

    def call(self, command, payload=b"", timeout=5, **fields):
        request = dict(command=command, token=self.manifest["token"],
                       run_id=self.manifest["run_id"], payload_size=len(payload), **fields)
        with socket.create_connection(("127.0.0.1", self.manifest["port"]), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(json.dumps(request).encode("ascii") + b"\n" + payload)
            with sock.makefile("rb") as stream:
                header = stream.readline(65537)
                if len(header) > 65536 or not header.endswith(b"\n"):
                    raise ConsoleError("invalid_response_header")
                response = json.loads(header)
                size = response.get("payload_size")
                if not isinstance(size, int) or not 0 <= size <= 65536:
                    raise ConsoleError("invalid_response_size")
                data = exact(stream, size)
        if response.get("run_id") != self.manifest["run_id"]:
            raise ConsoleError("session_mismatch")
        if not response.get("ok"):
            raise ConsoleError(response.get("error", response.get("input_state", "request_failed")),
                               code=4 if response.get("error") == "observation_fault" else 6,
                               response=response)
        return response, data

    def status(self, timeout=5):
        status, _ = self.call("status", timeout=timeout)
        return status


def healthy(status):
    if status["observation"] != "ready":
        raise ConsoleError("observation_fault", code=4, status=status)
    if status["startup_error"]:
        raise ConsoleError("startup_failed", code=5, status=status)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def prepare_deterministic(directory, data_dir, elf, executable, replay_source=None):
    """Snapshot boot assets; all mutable emulator data belongs to this run."""
    inputs = directory / "inputs"
    inputs.mkdir()
    source_root = data_dir / "PCSX2"
    config_source = Path(replay_source) / "inputs/config.ini" if replay_source else source_root / "inis/PCSX2.ini"
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    config.read(config_source, encoding="utf-8")
    bios_dir = Path(replay_source) / "inputs/bios" if replay_source else Path(config.get("Folders", "Bios", fallback="bios"))
    if not bios_dir.is_absolute():
        bios_dir = source_root / bios_dir
    bios_name = config.get("Filenames", "BIOS")
    if replay_source:
        bios_name = Path(bios_name).name
    bios_path = (bios_dir / bios_name).resolve(strict=True)
    # Only filenames are accepted in the staged config, regardless of the source spelling.
    bios_name = bios_path.name
    shutil.copyfile(config_source, inputs / "config.ini")
    shutil.copyfile(elf, inputs / "guest.elf")
    (inputs / "bios").mkdir()
    shutil.copyfile(bios_path, inputs / "bios" / bios_name)
    for extension in (".rom1", ".rom2", ".erom"):
        companion = bios_path.with_suffix(extension)
        if companion.is_file():
            shutil.copyfile(companion, inputs / "bios" / companion.name)
    environment = directory / "environment"
    root = environment / "PCSX2"
    (root / "inis").mkdir(parents=True)
    shutil.copytree(inputs / "bios", root / "bios")
    if not config.has_section("Folders"):
        config.add_section("Folders")
    for key, name in list(config.items("Folders")):
        config.set("Folders", key, "bios" if key == "Bios" else key.lower())
    config.set("Filenames", "BIOS", bios_name)
    for section, key, value in (("UI", "StartPaused", "true"), ("UI", "SetupWizardIncomplete", "false"),
                                ("UI", "PauseOnFocusLoss", "false"), ("Logging", "EnableLogWindow", "false")):
        if not config.has_section(section): config.add_section(section)
        config.set(section, key, value)
    with (root / "inis/PCSX2.ini").open("w", encoding="utf-8") as target:
        config.write(target)
    files = {str(path.relative_to(directory)).replace("\\", "/"): sha256(path)
             for path in inputs.rglob("*") if path.is_file()}
    binaries = {str(executable): sha256(executable)}
    binaries.update({str(path): sha256(path) for path in executable.parent.glob("*.dll")})
    manifest = dict(profile="isolated-elf-v1-same-build", files=files, binaries=binaries,
                    executable=str(executable), external_devices="disabled", rtc="2020-01-01T00:00:00Z",
                    nvram="fresh BIOS defaults per cold boot", schedule="EE event-test boundaries")
    if replay_source:
        previous = json.loads(Path(replay_source, "inputs-manifest.json").read_text(encoding="utf-8"))
        if previous["files"] != files or previous["binaries"] != binaries:
            raise ConsoleError("replay_inputs_or_build_mismatch", code=4)
    (directory / "inputs-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return environment, inputs / "guest.elf"


def action(client, command, epoch, request_id=None, timeout=60, **fields):
    request_id = request_id or str(uuid.uuid4())
    emit(dict(request_id=request_id, operation=command), stream=sys.stderr)
    result, _ = client.call(command, epoch=epoch, request_id=request_id, **fields)
    deadline = time.monotonic() + timeout
    while result.get("action_state") != "done":
        if time.monotonic() >= deadline:
            raise ConsoleError("action_timeout", code=3, request_id=request_id, outcome="unknown")
        time.sleep(.02)
        result, _ = client.call("action-status", epoch=epoch, request_id=request_id)
    return result


def start(args):
    directory = Path(args.run).resolve()
    executable = Path(args.exe).resolve(strict=True)
    elf = Path(args.elf).resolve(strict=True)
    data_dir = Path(args.data_dir).resolve(strict=True)
    if not (data_dir / "PCSX2" / "inis" / "PCSX2.ini").is_file():
        raise ConsoleError("explicit_configuration_required", data_dir=str(data_dir))
    extra = args.pcsx2_args
    if extra[:1] == ["--"]:
        extra = extra[1:]
    if any(arg in ("-portable", "-datapath", "-agent-console", "-elf") for arg in extra):
        raise ConsoleError("conflicting_boot_arguments")
    directory.mkdir(parents=True, exist_ok=False)
    if args.deterministic:
        if extra:
            raise ConsoleError("deterministic_extra_boot_arguments_not_supported")
        data_dir, elf = prepare_deterministic(directory, data_dir, elf, executable, getattr(args, "replay_source", None))
    command = [str(executable), "-agent-console", str(directory), "-datapath", str(data_dir),
               "-elf", str(elf), *extra]
    if args.deterministic:
        command.append("-agent-deterministic")
    options = dict(stdin=subprocess.DEVNULL, cwd=str(executable.parent))
    if os.name == "nt":
        options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    else:
        options["start_new_session"] = True
    with (directory / "launcher.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, **options)
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        if (directory / "run.json").exists():
            client = Client(directory)
            status = client.status(timeout=min(2, max(.1, deadline - time.monotonic())))
            healthy(status)
            if status["vm_state"] in ("running", "paused"):
                if args.deterministic:
                    initial = action(client, "snapshot", status["epoch"], "initial", timeout=args.timeout)
                    status = client.status()
                    status["initial_checkpoint"] = initial
                if not getattr(args, "quiet", False):
                    emit(dict(ok=True, run=str(directory), pid=process.pid, status=status))
                return
        if process.poll() is not None:
            raise ConsoleError("startup_process_exited", code=5, exit_code=process.returncode,
                               log=str(directory / "launcher.log"))
        time.sleep(.05)
    # The process is deliberately retained for diagnosis; a timeout is not a guest failure.
    raise ConsoleError("startup_timeout", code=3, pid=process.pid, run=str(directory))


def execute(args):
    if args.command == "start":
        return start(args)
    if args.command == "replay":
        return replay(args)
    client = Client(args.run)
    status = client.status()
    if args.command == "status":
        emit(status)
        healthy(status)
        return
    if args.command == "stop":
        client.call("stop")
        deadline = time.monotonic() + args.timeout
        result_path = client.directory / "result.json"
        while time.monotonic() < deadline:
            if result_path.exists():
                result = json.loads(result_path.read_text(encoding="utf-8"))
                emit(result)
                healthy(result)
                return
            time.sleep(.05)
        raise ConsoleError("stop_timeout", code=3)
    healthy(status)
    epoch = args.epoch if args.epoch is not None else status["epoch"]
    if str(epoch) != status["epoch"]:
        raise ConsoleError("epoch_mismatch", expected=str(epoch), actual=status["epoch"])
    if args.command in ("registers", "run-events"):
        fields = dict(until=str(args.until)) if args.command == "run-events" else {}
        result = action(client, "run-events" if args.command == "run-events" else "snapshot", epoch,
                        args.request_id, timeout=args.timeout, **fields)
        if args.command == "registers":
            document = json.loads((client.directory / result["snapshot_file"]).read_text(encoding="utf-8"))
            if args.out:
                Path(args.out).write_text(json.dumps(document, indent=2), encoding="utf-8")
                result["output_file"] = str(Path(args.out).resolve())
            else:
                return emit(document)
        emit(result)
        return
    if args.command in ("send", "input-status"):
        request_id = args.request_id or str(uuid.uuid4())
        if args.command == "send":
            if args.text is not None:
                payload = args.text.encode("utf-8")
            elif args.file is not None:
                with Path(args.file).open("rb") as source:
                    payload = source.read(1025)
            else:
                payload = bytes.fromhex(args.hex)
            payload += {"none": b"", "lf": b"\n", "crlf": b"\r\n", "cr": b"\r"}[args.newline]
            if not 1 <= len(payload) <= 1024:
                raise ConsoleError("invalid_input_size", size=len(payload))
            # Never retry this mutation after a transport failure. Query the printed ID instead.
            emit(dict(request_id=request_id, operation="send"), stream=sys.stderr)
            result, _ = client.call("send", payload, epoch=epoch, request_id=request_id)
        else:
            result, _ = client.call("input-status", epoch=epoch, request_id=request_id)
        deadline = time.monotonic() + args.timeout
        while result["input_state"] == "queued":
            if time.monotonic() >= deadline:
                raise ConsoleError("input_acceptance_timeout", code=3, request_id=request_id,
                                   outcome="unknown_do_not_resend")
            time.sleep(.02)
            result, _ = client.call("input-status", epoch=epoch, request_id=request_id)
        emit(result)
        return
    offset = args.offset
    if args.command == "read":
        result, data = client.call("read", epoch=epoch, channel=args.channel,
                                   offset=str(offset), limit=args.max_bytes)
        if args.out:
            Path(args.out).write_bytes(data)
            result["output_file"] = str(Path(args.out).resolve())
        elif args.raw:
            if os.name == "nt":
                import msvcrt
                msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            emit(result, stream=sys.stderr)
            return
        else:
            result["text"] = data.decode("utf-8", errors="backslashreplace")
        emit(result)
        return
    if args.command == "expect":
        needle = args.text.encode("utf-8") if args.text is not None else bytes.fromhex(args.hex)
        if not 1 <= len(needle) <= 4096:
            raise ConsoleError("invalid_pattern_size")
        deadline = time.monotonic() + args.timeout
        tail = b""
        while time.monotonic() < deadline:
            result, data = client.call("read", timeout=min(2, max(.1, deadline-time.monotonic())),
                                       epoch=epoch, channel=args.channel, offset=str(offset), limit=65536)
            combined = tail + data
            match = combined.find(needle)
            if match >= 0:
                emit(dict(ok=True, run_id=client.manifest["run_id"], epoch=epoch, channel=args.channel,
                          match_offset=offset-len(tail)+match, next_offset=offset-len(tail)+match+len(needle)))
                return
            offset = int(result["next_offset"])
            tail = combined[-(len(needle)-1):] if len(needle) > 1 else b""
            if not data:
                state = client.status()
                healthy(state)
                if state["vm_state"] in ("shutdown", "stopping"):
                    raise ConsoleError("vm_stopped_before_match", code=3, next_offset=offset)
                time.sleep(.05)
        raise ConsoleError("expect_timeout", code=3, outcome="unknown", epoch=epoch,
                           next_offset=offset, status=client.status())


def replay(args):
    source = Path(args.source).resolve(strict=True)
    manifest = json.loads((source / "inputs-manifest.json").read_text(encoding="utf-8"))
    final = json.loads((source / "result.json").read_text(encoding="utf-8"))
    healthy(final)
    for filename, digest in manifest["files"].items():
        path = (source / filename).resolve(strict=True)
        if not path.is_relative_to(source / "inputs") or sha256(path) != digest:
            raise ConsoleError("recorded_input_modified", code=4, file=filename)
    entries = [json.loads(line) for line in (source / "events.jsonl").read_text(encoding="utf-8").splitlines()]
    checkpoints = {item["checkpoint"]["request_id"]: item["checkpoint"] for item in entries if item["event"] == "checkpoint"}
    accepted = {item["input"]["request_id"]: item["input"] for item in entries
                if item["event"] == "input_result" and item["input"]["input_state"] == "accepted"}
    actions = [item for item in entries if item["event"] == "action_requested"]
    if not actions or actions[0]["request_id"] != "initial" or set(checkpoints) != {item["request_id"] for item in actions}:
        raise ConsoleError("recorded_checkpoints_incomplete", code=4)
    last = checkpoints[actions[-1]["request_id"]]
    if last["event"] != final["event"] or final["vm_state"] != "shutdown":
        raise ConsoleError("recording_not_finished_at_checkpoint", code=4)
    source_bytes = (source / "serial.ee-sio.rx.bin").read_bytes()
    deadline = time.monotonic() + args.timeout
    start_args = argparse.Namespace(run=args.run, exe=args.exe or manifest["executable"],
        elf=str(source / "inputs/guest.elf"), data_dir=str(source / "environment"),
        pcsx2_args=[], deterministic=True, replay_source=str(source), timeout=args.timeout, quiet=True)
    start(start_args)
    client = Client(args.run)
    epoch = client.status()["epoch"]
    matched = []
    seen_inputs = set()
    result = {}
    try:
        for item in entries:
            remaining = deadline-time.monotonic()
            if remaining <= 0: raise ConsoleError("replay_timeout", code=3, outcome="unknown")
            if item["event"] == "input_result" and item["input"]["input_state"] == "queued":
                record = item["input"]
                identifier = record["request_id"]
                if identifier in seen_inputs: continue
                seen_inputs.add(identifier)
                expected = accepted.get(identifier)
                if expected is None: raise ConsoleError("recorded_input_incomplete", code=4, request_id=identifier)
                status = client.status()
                if status["event"] != expected["at_event"]:
                    raise ConsoleError("input_boundary_diverged", code=4, expected=expected["at_event"], actual=status["event"])
                offset = int(record["input_file_offset"])
                payload = source_bytes[offset:offset+record["size"]]
                if len(payload) != record["size"]: raise ConsoleError("recorded_input_truncated", code=4)
                response, _ = client.call("send", payload, epoch=epoch, request_id=identifier)
                while response["input_state"] == "queued":
                    if time.monotonic() >= deadline: raise ConsoleError("replay_timeout", code=3)
                    time.sleep(.01)
                    response, _ = client.call("input-status", epoch=epoch, request_id=identifier)
            elif item["event"] == "action_requested":
                identifier = item["request_id"]
                expected = checkpoints.get(identifier)
                if expected is None: raise ConsoleError("recorded_checkpoint_incomplete", code=4, request_id=identifier)
                operation = item["action"]
                if identifier == "initial":
                    actual, _ = client.call("action-status", epoch=epoch, request_id="initial")
                else:
                    fields = dict(until=operation["until"]) if operation["command"] == "run-events" else {}
                    actual = action(client, operation["command"], epoch, identifier, timeout=remaining, **fields)
                if actual["state_sha256"] != expected["state_sha256"]:
                    raise ConsoleError("replay_diverged", code=4, request_id=identifier, event=actual["event"],
                                       expected=expected["state_sha256"], actual=actual["state_sha256"],
                                       snapshot_file=actual["snapshot_file"])
                matched.append(dict(request_id=identifier, event=actual["event"], state_sha256=actual["state_sha256"]))
        if seen_inputs != set(accepted):
            raise ConsoleError("recorded_input_ledger_incomplete", code=4)
        result = dict(ok=True, result="replay_verified", profile=manifest["profile"],
                      source=str(source), checkpoints=matched, inputs=len(seen_inputs))
    except ConsoleError as error:
        result = error.result
        raise
    finally:
        if result:
            (client.directory / "replay-verification.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        try:
            client.call("stop")
            until = time.monotonic()+15
            while not (client.directory / "result.json").exists() and time.monotonic()<until: time.sleep(.05)
        except (OSError, ConsoleError):
            pass
    emit(result)


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run", required=True, help="Unique run directory; never reused")
    subs = p.add_subparsers(dest="command", required=True)
    s = subs.add_parser("start")
    s.add_argument("--exe", required=True)
    s.add_argument("--elf", required=True)
    s.add_argument("--data-dir", required=True, help="Parent of PCSX2/inis/PCSX2.ini (the existing -datapath convention)")
    s.add_argument("--timeout", type=float, default=30)
    s.add_argument("--deterministic", action="store_true", help="Isolated ELF profile; starts paused")
    s.add_argument("pcsx2_args", nargs=argparse.REMAINDER)
    subs.add_parser("status")
    s = subs.add_parser("replay")
    s.add_argument("--source", required=True)
    s.add_argument("--exe", help="Must match recorded executable and sibling DLL hashes")
    s.add_argument("--timeout", type=float, default=120)
    for name in ("registers", "run-events"):
        s = subs.add_parser(name)
        s.add_argument("--epoch")
        s.add_argument("--request-id")
        s.add_argument("--timeout", type=float, default=60)
        if name == "registers": s.add_argument("--out")
        else: s.add_argument("--until", type=int, required=True, help="Absolute EE event-test index")
    s = subs.add_parser("stop")
    s.add_argument("--timeout", type=float, default=15)
    for command in ("send", "input-status", "read", "expect"):
        s = subs.add_parser(command)
        s.add_argument("--epoch", help="Reject reset/load races using a previously observed epoch")
        if command in ("send", "input-status"):
            s.add_argument("--request-id", required=command == "input-status")
            s.add_argument("--timeout", type=float, default=5)
        if command in ("send", "expect"):
            group = s.add_mutually_exclusive_group(required=True)
            group.add_argument("--text")
            group.add_argument("--hex")
            if command == "send":
                group.add_argument("--file")
                s.add_argument("--newline", choices=("none", "lf", "crlf", "cr"), default="none")
        if command in ("read", "expect"):
            s.add_argument("--channel", choices=("ee.sio", "iop.stdout", "iop.hle"), default="ee.sio")
            s.add_argument("--from", dest="offset", type=int, required=command == "expect", default=0)
        if command == "read":
            s.add_argument("--max-bytes", type=int, default=8192)
            group = s.add_mutually_exclusive_group()
            group.add_argument("--raw", action="store_true")
            group.add_argument("--out")
        if command == "expect":
            s.add_argument("--timeout", type=float, default=10)
    return p


def main():
    try:
        args = parser().parse_args()
        if hasattr(args, "timeout") and (not math.isfinite(args.timeout) or not 0 <= args.timeout <= 3600):
            raise ConsoleError("invalid_timeout", allowed="0..3600 seconds")
        execute(args)
    except ConsoleError as error:
        emit(error.result, stream=sys.stderr)
        return error.code
    except (OSError, ValueError, KeyError) as error:
        emit(dict(ok=False, reason="client_or_transport_error", detail=str(error)), stream=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
