#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Exercise an already running Linux console; saves evidence in its run directory."""
import argparse
import json
from pathlib import Path
import sys
import subprocess
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from pcsx2ctl import Client, ConsoleError, healthy


def smoke(run, timeout):
    client = Client(run)
    deadline = time.monotonic() + timeout
    status = client.status()
    healthy(status)
    epoch = status["epoch"]

    def wait(offset, needle):
        received = bytearray()
        start = offset
        while time.monotonic() < deadline:
            response, data = client.call("read", epoch=epoch, channel="ee.sio", offset=str(offset), limit=65536)
            received.extend(data)
            offset = int(response["next_offset"])
            if needle in received:
                return start, bytes(received)
            if len(received) > 1024 * 1024:
                raise ConsoleError("smoke_output_limit", code=4)
            if not data:
                healthy(client.status())
                time.sleep(.05)
        raise ConsoleError("smoke_timeout", code=3, outcome="unknown", next_offset=offset)

    wait(int(status["channels"]["ee.sio"]["epoch_start"]), b"~ # ")
    nonce = uuid.uuid4().hex
    marker = ("PCSX2_SMOKE_" + nonce).encode()
    # The complete marker does not occur in the command echo. No newline is
    # printed by the binary printf, so this also checks incomplete-line capture.
    command = ("uname -a; printf '\\101\\000\\102\\377'; "
               "printf '%s%s\\n' 'PCSX2_SMOKE_' '" + nonce + "'\n").encode()
    request_id = "smoke-" + nonce
    accepted, _ = client.call("send", command, epoch=epoch, request_id=request_id)
    while accepted["input_state"] == "queued":
        if time.monotonic() >= deadline:
            raise ConsoleError("input_acceptance_timeout", code=3, request_id=request_id)
        time.sleep(.02)
        accepted, _ = client.call("input-status", epoch=epoch, request_id=request_id)
    offset, response = wait(int(accepted["output_offset"]), marker)
    if b"mips GNU/Linux" not in response or b"A\x00B\xff" not in response:
        raise ConsoleError("smoke_response_mismatch", code=6)
    before = client.status()["rx_consumed"]
    duplicate, _ = client.call("send", command, epoch=epoch, request_id=request_id)
    if duplicate["output_offset"] != accepted["output_offset"] or client.status()["rx_consumed"] != before:
        raise ConsoleError("duplicate_input_replayed", code=6)
    try:
        client.call("send", b"different\n", epoch=epoch, request_id=request_id)
    except ConsoleError as error:
        if error.result["reason"] != "request_id_conflict":
            raise
    else:
        raise ConsoleError("conflicting_id_accepted", code=6)
    final = client.status()
    healthy(final)
    archive = (client.directory / "serial.ee-sio.bin").read_bytes()
    if archive[offset:offset + len(response)] != response:
        raise ConsoleError("archive_transport_mismatch", code=4)
    raw = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / "tools/pcsx2ctl.py"),
                          "--run", str(client.directory), "read", "--epoch", epoch, "--from", str(offset),
                          "--max-bytes", str(min(len(response), 65536)), "--raw"], capture_output=True, timeout=10)
    if raw.returncode != 0 or raw.stdout != response[:65536]:
        raise ConsoleError("raw_stdout_mismatch", code=4)
    (client.directory / "smoke-response.bin").write_bytes(response)
    return dict(ok=True, scope="Linux console smoke, not general kernel correctness", run_id=client.manifest["run_id"],
                epoch=epoch, request_id=request_id, offset=offset, response_bytes=len(response),
                uname_response=True, raw_nul_ff=True, duplicate_not_replayed=True,
                conflicting_id_rejected=True, archive_matches_transport=True, raw_stdout_matches=True, status=final)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True)
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if not 0 < args.timeout <= 600:
        parser.error("timeout must be in (0, 600]")
    code = 0
    try:
        result = smoke(args.run, args.timeout)
    except ConsoleError as error:
        result, code = error.result, error.code
    except (OSError, ValueError, KeyError) as error:
        result, code = dict(ok=False, reason="smoke_transport_error", detail=str(error)), 2
    Path(args.run, "smoke-result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))
    return code


if __name__ == "__main__":
    sys.exit(main())
