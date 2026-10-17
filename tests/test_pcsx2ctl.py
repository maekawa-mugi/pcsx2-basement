# SPDX-License-Identifier: GPL-3.0+
"""Run with: python -m unittest discover -s tests -p test_pcsx2ctl.py"""
import importlib.util
import io
import json
from pathlib import Path
import socketserver
import tempfile
import threading
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("pcsx2ctl", Path(__file__).parents[1] / "tools/pcsx2ctl.py")
ctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ctl)


class ProtocolTests(unittest.TestCase):
    def exchange(self, response, data=b"", split=False):
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                request = json.loads(self.rfile.readline())
                self.server.received = self.rfile.read(request["payload_size"])
                wire = json.dumps(response).encode() + b"\n" + data
                if split:
                    for value in wire:
                        self.wfile.write(bytes([value]))
                        self.wfile.flush()
                else:
                    self.wfile.write(wire)
        server = socketserver.TCPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        Path(directory.name, "run.json").write_text(json.dumps(dict(
            protocol=1, host="127.0.0.1", port=server.server_address[1], run_id="test", token="token")))
        return ctl.Client(directory.name), server

    def test_binary_round_trip_and_fragmented_transport(self):
        data = b"\x00\r\n\xff\xe3\x81\x82# "
        client, server = self.exchange(dict(ok=True, run_id="test", payload_size=len(data)), data, split=True)
        _, received = client.call("send", data)
        self.assertEqual(received, data)
        self.assertEqual(server.received, data)

    def test_truncated_payload_is_observation_error(self):
        client, _ = self.exchange(dict(ok=True, run_id="test", payload_size=5), b"ab")
        with self.assertRaises(ctl.ConsoleError) as context:
            client.call("read")
        self.assertEqual(context.exception.result["reason"], "truncated_response")

    def test_stale_session_cannot_look_successful(self):
        client, _ = self.exchange(dict(ok=True, run_id="old-run", payload_size=0))
        with self.assertRaises(ctl.ConsoleError) as context:
            client.status()
        self.assertEqual(context.exception.result["reason"], "session_mismatch")

    def test_observation_fault_has_distinct_exit_code(self):
        client, _ = self.exchange(dict(ok=False, run_id="test", payload_size=0, error="observation_fault"))
        with self.assertRaises(ctl.ConsoleError) as context:
            client.call("read")
        self.assertEqual(context.exception.code, 4)

    def test_expect_matches_across_chunks_without_newline(self):
        args = ctl.parser().parse_args(["--run", "unused", "expect", "--from", "0", "--text", "login:"])
        class Fake:
            manifest = {"run_id": "test"}
            chunks = iter([b"log", b"in:"])
            offset = 0
            def status(self):
                return dict(observation="ready", startup_error="", epoch="1", vm_state="running")
            def call(self, *args, **kwargs):
                data = next(self.chunks)
                self.offset += len(data)
                return dict(next_offset=str(self.offset)), data
        with patch.object(ctl, "Client", return_value=Fake()), patch("sys.stdout", new_callable=io.StringIO) as output:
            ctl.execute(args)
            self.assertEqual(json.loads(output.getvalue())["next_offset"], 6)

    def test_expect_timeout_is_unknown_not_guest_failure(self):
        args = ctl.parser().parse_args(["--run", "unused", "expect", "--from", "0", "--text", "x", "--timeout", "0"])
        class Fake:
            manifest = {"run_id": "test"}
            def status(self):
                return dict(observation="ready", startup_error="", epoch="1", vm_state="running")
        with patch.object(ctl, "Client", return_value=Fake()):
            with self.assertRaises(ctl.ConsoleError) as context:
                ctl.execute(args)
        self.assertEqual(context.exception.code, 3)
        self.assertEqual(context.exception.result["outcome"], "unknown")

    def test_action_timeout_retains_id_and_does_not_retry_mutation(self):
        class Fake:
            calls = []
            def call(self, command, **kwargs):
                self.calls.append(command)
                return dict(action_state="queued"), b""
        client = Fake()
        with patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(ctl.ConsoleError) as context:
                ctl.action(client, "run-events", "1", "advance-1", timeout=0, until="100")
        self.assertEqual(context.exception.code, 3)
        self.assertEqual(context.exception.result["request_id"], "advance-1")
        self.assertEqual(client.calls, ["run-events"])

    def test_staging_isolates_assets_and_replay_rejects_changed_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data/PCSX2"
            (data / "inis").mkdir(parents=True)
            (data / "bios").mkdir()
            bios = data / "bios/test.bin"
            bios.write_bytes(b"original BIOS fixture")
            (data / "inis/PCSX2.ini").write_text("[Folders]\nBios=bios\n[Filenames]\nBIOS=test.bin\n")
            elf, exe = root / "kernel.elf", root / "emulator.exe"
            elf.write_bytes(b"ELF fixture")
            exe.write_bytes(b"executable fixture")
            run = root / "record"
            run.mkdir()
            environment, staged = ctl.prepare_deterministic(run, root / "data", elf, exe)
            bios.write_bytes(b"changed outside session")
            elf.write_bytes(b"changed outside session")
            self.assertEqual((environment / "PCSX2/bios/test.bin").read_bytes(), b"original BIOS fixture")
            self.assertEqual(staged.read_bytes(), b"ELF fixture")
            exe.write_bytes(b"changed build")
            replay = root / "replay"
            replay.mkdir()
            with self.assertRaises(ctl.ConsoleError) as context:
                ctl.prepare_deterministic(replay, environment, staged, exe, str(run))
            self.assertEqual(context.exception.result["reason"], "replay_inputs_or_build_mismatch")

    def test_empty_trace_cannot_be_verified_as_replay(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "inputs-manifest.json").write_text(json.dumps(dict(files={})))
            (root / "result.json").write_text(json.dumps(dict(observation="ready", startup_error="")))
            (root / "events.jsonl").write_text("")
            args = ctl.parser().parse_args(["--run", str(root / "new"), "replay", "--source", str(root)])
            with self.assertRaises(ctl.ConsoleError) as context:
                ctl.execute(args)
            self.assertEqual(context.exception.result["reason"], "recorded_checkpoints_incomplete")

    def test_replay_divergence_stops_and_saves_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, target = root / "source", root / "target"
            source.mkdir()
            target.mkdir()
            (source / "inputs-manifest.json").write_text(json.dumps(dict(files={}, executable="fixture")))
            (source / "result.json").write_text(json.dumps(dict(
                observation="ready", startup_error="", event="0", vm_state="shutdown")))
            (source / "events.jsonl").write_text("\n".join(json.dumps(item) for item in [
                dict(event="action_requested", request_id="initial", action=dict(command="snapshot")),
                dict(event="checkpoint", checkpoint=dict(request_id="initial", event="0", state_sha256="expected")),
            ]))
            (source / "serial.ee-sio.rx.bin").write_bytes(b"")
            (target / "result.json").write_text("{}")
            class Fake:
                directory = target
                stopped = False
                def status(self):
                    return dict(epoch="1")
                def call(self, command, **kwargs):
                    if command == "stop":
                        self.stopped = True
                    return dict(state_sha256="diverged", event="0", snapshot_file="snapshot.json"), b""
            client = Fake()
            args = ctl.parser().parse_args(["--run", str(target), "replay", "--source", str(source)])
            with patch.object(ctl, "start"), patch.object(ctl, "Client", return_value=client):
                with self.assertRaises(ctl.ConsoleError) as context:
                    ctl.execute(args)
            self.assertEqual(context.exception.result["reason"], "replay_diverged")
            self.assertTrue(client.stopped)
            report = json.loads((target / "replay-verification.json").read_text())
            self.assertFalse(report["ok"])
            self.assertEqual(report["request_id"], "initial")


if __name__ == "__main__":
    unittest.main()
