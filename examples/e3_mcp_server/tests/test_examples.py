# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Offline tests: MCP framing and tool dispatch against a fake E3 session."""

import json
import subprocess
import sys
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))

import ocudu_mcp_server as srv  # noqa: E402

mgmt_display = srv.mgmt.display_identifier


class FakeSession:
    host, port = "127.0.0.1", 36423

    def __init__(self):
        self.status = {"connected": True, "dapp_id": 7, "ran_identifier": "fake", "catalog": [
            {"id": 1, "name": "pusch.symbols", "description": "", "default_floor_ms": 50, "hard_floor_ms": 0, "class_c": False}],
            "subscribed": ["pusch.symbols"], "indications": 0, "reconnects": 0, "last_error": ""}
        self.requests = []

    def management(self, request):
        self.requests.append(request)
        op, body = request["operation"]
        if op == "inventory":
            return {"code": "success", "payload": {"inventory": {"generation": 3, "packages": [], "instances": [], "connections": [
                {"associationId": 1, "peerAddress": "127.0.0.1", "setupComplete": True, "dappId": 1, "dappName": "dash", "subscriptions": []}]}}}
        if op == "configQuery" and body["kind"] == "schema":
            return {"code": "success", "payload": {"config": {"generation": 5, "descriptors": [
                {"key": "target_bler", "kind": "real"}, {"key": "max_mcs", "kind": "unsigned"}]}}}
        if op == "configQuery":
            return {"code": "success", "payload": {"config": {"generation": 5, "entries": []}}}
        if op == "jobs":
            job_id = body.get("jobId")
            record = {"jobId": mgmt_display(job_id) if job_id else "x", "state": "succeeded",
                      "result": {"code": "dryRunSuccess" if self.last_dry_run else "success"}}
            return {"code": "success", "payload": {"jobs": {"jobs": [record]}}}
        if op == "configSet":
            self.last_dry_run = bool(request["header"].get("dryRun"))
            return {"code": "accepted", "jobId": "22f98c37-c863-be02-0c00-000000000000", "payload": {}}
        if op == "cellInfo":
            return {"code": "success", "payload": {"cellInfo": {"cells": [{"pci": 2, "plmn": "00101", "band": 78}],
                                                              "capabilities": {"ueRelease": True, "handover": True, "txGain": True, "rxGain": True}}}}
        return {"code": "accepted" if not request["header"].get("dryRun") else "dryRunSuccess", "payload": {}}


def make_state():
    state = srv.SectorState()
    now = time.time()
    ue = state._ue(0x4601, now)
    ue.sinr_db, ue.modulation_bits, ue.nof_prb = 21.5, 6, 20
    for i in range(20):
        ue.crc_window.append((now - i * 0.01, i % 5 != 0))
    state._ingest_metrics({"ue_list": [{"ue_container": {"rnti": 0x4601, "pci": 2, "dl_brate": 1e6, "ul_brate": 2e5, "cqi": 12, "ul_mcs": 16}}],
                           "cells": [{"cell_metrics": {"pci": 2, "pusch_prbs_used_per_tdd_slot_idx": [10, 0, 0, 20]}}]}, now)
    return state


class McpTests(unittest.TestCase):
    def setUp(self):
        self.session = FakeSession()
        self.state = make_state()

    def server(self, read_only=False):
        return srv.McpServer(srv.Tools(self.session, self.state, read_only), read_only)

    def rpc(self, server, method, params=None, msg_id=1):
        return server.handle({"jsonrpc": "2.0", "id": msg_id, "method": method, "params": params or {}})

    def test_initialize_and_list_tools(self):
        server = self.server()
        init = self.rpc(server, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})
        self.assertEqual(init["result"]["protocolVersion"], "2025-06-18")
        self.assertIsNone(server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}))
        tools = self.rpc(server, "tools/list")["result"]["tools"]
        names = {t["name"] for t in tools}
        self.assertIn("get_cell_overview", names)
        self.assertIn("release_ue", names)
        self.assertTrue(all("inputSchema" in t for t in tools))

    def test_read_only_hides_mutations(self):
        server = self.server(read_only=True)
        names = {t["name"] for t in self.rpc(server, "tools/list")["result"]["tools"]}
        self.assertNotIn("release_ue", names)
        self.assertNotIn("set_dapp_config", names)
        self.assertIn("get_ue", names)
        err = self.rpc(server, "tools/call", {"name": "release_ue", "arguments": {"rnti": "0x4601"}})
        self.assertIn("error", err)

    def test_observation_tools_reflect_state(self):
        server = self.server()
        overview = json.loads(self.rpc(server, "tools/call", {"name": "get_cell_overview"})["result"]["content"][0]["text"])
        self.assertEqual(overview["ues_active_10s"], 1)
        self.assertAlmostEqual(overview["throughput_bps"]["dl"], 1e6)
        ue = self.rpc(server, "tools/call", {"name": "get_ue", "arguments": {"rnti": "0x4601"}})["result"]["structuredContent"]
        self.assertEqual(ue["phy"]["sinr_db"], 21.5)
        self.assertAlmostEqual(ue["ul_bler"]["bler"], 0.2, places=3)
        self.assertEqual(ue["mac"]["cqi"], 12)
        kpis = self.rpc(server, "tools/call", {"name": "get_kpis", "arguments": {"window_s": 30}})["result"]["structuredContent"]
        self.assertEqual(kpis["ues"][0]["rnti"], "0x4601")
        connections = self.rpc(server, "tools/call", {"name": "get_e3_connections"})["result"]["structuredContent"]
        self.assertEqual(connections["connections"][0]["dappName"], "dash")

    def test_mutations_build_correct_requests(self):
        server = self.server()
        result = self.rpc(server, "tools/call", {"name": "release_ue", "arguments": {"rnti": "0x4601"}})["result"]
        self.assertFalse(result["isError"], result)
        op, body = self.session.requests[-1]["operation"]
        self.assertEqual((op, body["action"], body["servingPci"], body["rnti"]), ("ranControl", "releaseUeToIdle", 2, 0x4601))
        self.assertTrue(self.session.requests[-1]["header"]["dryRun"])
        result = self.rpc(server, "tools/call", {"name": "set_dapp_config", "arguments": {
            "instance_id": "36f5c48e-72b1-03cb-0200-000000000000", "entries": {"target_bler": 0.05, "max_mcs": 20}}})["result"]
        self.assertFalse(result["isError"], result)
        self.assertEqual(result["structuredContent"]["outcome"], "dryRunSuccess")
        self.assertTrue(result["structuredContent"]["ok"])
        op, body = next(r["operation"] for r in reversed(self.session.requests) if r["operation"][0] == "configSet")
        self.assertEqual(op, "configSet")
        kinds = {e["key"].decode(): e["value"][0] for e in body["entries"]}
        self.assertEqual(kinds, {"target_bler": "realValue", "max_mcs": "unsignedValue"})
        self.assertEqual(int.from_bytes(body["expectedConfigGeneration"], "big"), 5)
        bad = self.rpc(server, "tools/call", {"name": "set_dapp_config", "arguments": {
            "instance_id": "36f5c48e-72b1-03cb-0200-000000000000", "entries": {"nope": 1}}})["result"]
        self.assertTrue(bad["isError"])

    def test_cell_info_handover_and_gain_requests(self):
        server = self.server()
        info = self.rpc(server, "tools/call", {"name": "get_cell_info"})["result"]["structuredContent"]
        self.assertEqual(info["cells"][0]["pci"], 2)
        self.assertTrue(info["capabilities"]["handover"])
        result = self.rpc(server, "tools/call", {"name": "handover_ue", "arguments": {"rnti": "0x4601", "target_pci": 5}})["result"]
        self.assertFalse(result["isError"], result)
        op, body = self.session.requests[-1]["operation"]
        self.assertEqual((op, body["action"], body["servingPci"], body["rnti"], body["targetPci"]), ("ranControl", "handoverUe", 2, 0x4601, 5))
        result = self.rpc(server, "tools/call", {"name": "set_rf_gain", "arguments": {"direction": "rx", "gain_db": 55.5, "dry_run": False}})["result"]
        self.assertFalse(result["isError"], result)
        op, body = self.session.requests[-1]["operation"]
        self.assertEqual((body["action"], body["portId"], body["gainDb"], body["rnti"]), ("setRxGain", 0, 55.5, 0))
        self.assertFalse(self.session.requests[-1]["header"].get("dryRun", False))

    def test_resources_and_prompts(self):
        server = self.server()
        uris = {r["uri"] for r in self.rpc(server, "resources/list")["result"]["resources"]}
        self.assertIn("ocudu://cell/overview", uris)
        read = self.rpc(server, "resources/read", {"uri": "ocudu://cell/ues"})["result"]
        self.assertEqual(json.loads(read["contents"][0]["text"])["ues"][0]["rnti"], "0x4601")
        prompt = self.rpc(server, "prompts/get", {"name": "triage_cell"})["result"]
        self.assertEqual(prompt["messages"][0]["role"], "user")

    def test_help(self):
        result = subprocess.run([sys.executable, str(HERE / "ocudu_mcp_server.py"), "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--read-only", result.stdout)


if __name__ == "__main__":
    unittest.main()
