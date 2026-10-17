#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Cold-boot a Linux ELF, exercise paused UART input, then verify replay."""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import pcsx2ctl as ctl


def smoke(args):
    start_args = argparse.Namespace(run=args.run, exe=args.exe, elf=args.elf,
        data_dir=args.data_dir, pcsx2_args=[], deterministic=True, timeout=30, quiet=True)
    ctl.start(start_args)
    client = ctl.Client(args.run)
    epoch = client.status()["epoch"]
    try:
        before = client.status()
        time.sleep(.2)
        after = client.status()
        assert before["event"] == after["event"] == "0"
        assert before["cycle"] == after["cycle"] == "0"
        ctl.action(client, "run-events", epoch, "boot", until="8000000")
        state = client.status()
        assert state["vm_state"] == "paused" and state["event"] == "8000000"
        snapshot = ctl.action(client, "snapshot", epoch, "registers")
        document = json.loads((client.directory / snapshot["snapshot_file"]).read_text(encoding="utf-8"))
        banks = document["registers"]
        assert {"ee.control", "iop", "vu0", "vu1", "tlb"}.issubset(banks)
        assert len(banks["tlb"]) == 48 * 4
        assert banks["vu1"]["vf31"]["bits"] == 128
        assert "PCCR" in banks["ee.control"]
        assert set(document["raw_register_files"]) == {"ee.mmio", "iop.mmio", "ee.sio.rx"}
        for region, filename in document["raw_register_files"].items():
            assert ctl.sha256(client.directory / filename) == document["memory"][region]["sha256"]
        nonce = uuid.uuid4().hex
        marker = ("PCSX2_DETERMINISTIC_" + nonce).encode()
        command = ("uname -a; printf '\\101\\000\\102\\377'; "
                   "printf '%s%s\\n' 'PCSX2_DETERMINISTIC_' '" + nonce + "'\n").encode()
        accepted, _ = client.call("send", command, epoch=epoch, request_id="command")
        deadline = time.monotonic() + 10
        while accepted["input_state"] == "queued":
            if time.monotonic() >= deadline:
                raise ctl.ConsoleError("input_acceptance_timeout", code=3)
            time.sleep(.02)
            accepted, _ = client.call("input-status", epoch=epoch, request_id="command")
        assert accepted["at_event"] == "8000000"
        # Snapshot with pending RX bytes catches accidental omission of FIFO state.
        pending = ctl.action(client, "snapshot", epoch, "pending-input")
        pending_doc = json.loads((client.directory / pending["snapshot_file"]).read_text(encoding="utf-8"))
        fifo_file = pending_doc["raw_register_files"]["ee.sio.rx"]
        assert (client.directory / fifo_file).read_bytes() == command
        ctl.action(client, "run-events", epoch, "response", until="9000000")
        _, response = client.call("read", epoch=epoch, channel="ee.sio", offset=accepted["output_offset"], limit=65536)
        assert b"mips GNU/Linux" in response and b"A\x00B\xff" in response and marker in response
        duplicate, _ = client.call("send", command, epoch=epoch, request_id="command")
        assert duplicate == accepted
        for operation, fields, payload in (
            ("send", dict(request_id="command"), b"different\n"),
            ("run-events", dict(request_id="response", until="9000001"), b""),
            ("run-events", dict(request_id="past", until="8999999"), b""),
        ):
            try:
                client.call(operation, payload, epoch=epoch, **fields)
            except ctl.ConsoleError as error:
                assert error.code == 6
            else:
                raise AssertionError("invalid mutation accepted")
        result = dict(ok=True, paused_clock_stable=True, exact_event_budget=True,
            register_banks={name: len(values) for name, values in banks.items()},
            fifo_snapshot=True, raw_mmio_files=True, uname_response=True, raw_nul_ff=True,
            duplicate_suppressed=True, conflicting_mutations_rejected=True)
    finally:
        stop_args = ctl.parser().parse_args(["--run", args.run, "stop"])
        ctl.execute(stop_args)
    replay_args = ctl.parser().parse_args(["--run", args.replay_run, "replay", "--source", args.run, "--timeout", "120"])
    ctl.execute(replay_args)
    verification = json.loads(Path(args.replay_run, "replay-verification.json").read_text(encoding="utf-8"))
    assert verification["result"] == "replay_verified" and verification["inputs"] == 1
    result["matched_checkpoints"] = len(verification["checkpoints"])
    Path(args.run, "determinism-smoke-result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("run", "replay-run", "exe", "elf", "data-dir"):
        parser.add_argument("--" + name, required=True)
    smoke(parser.parse_args())
