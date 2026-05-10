#!/usr/bin/env python3
"""Unit tests for scripts/uq-publish.py.

Stdlib-only — no pytest. Run:
    python3 scripts/test_uq_publish.py
"""
from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

_SCRIPT = Path(__file__).resolve().parent / "uq-publish.py"
_spec = importlib.util.spec_from_file_location("uq_publish", _SCRIPT)
uq = importlib.util.module_from_spec(_spec)
sys.modules["uq_publish"] = uq
_spec.loader.exec_module(uq)


class StampMetadataTests(unittest.TestCase):
    def test_required_fields_set(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            subprocess.run(["git", "init", "-q", "-b", "main"], cwd=tmp, check=True)
            subprocess.run(["git", "config", "user.email", "t@t"], cwd=tmp, check=True)
            subprocess.run(["git", "config", "user.name", "t"], cwd=tmp, check=True)
            (tmp / "f").write_text("x")
            subprocess.run(["git", "add", "."], cwd=tmp, check=True)
            subprocess.run(["git", "commit", "-q", "-m", "i", "--no-gpg-sign"],
                           cwd=tmp, check=True)
            sha = subprocess.run(["git", "rev-parse", "HEAD"], cwd=tmp,
                                 capture_output=True, text=True, check=True
                                 ).stdout.strip()

            graph = {"schema": "gitnexus@1", "nodes": [], "links": []}
            md = uq.stamp_metadata(graph, repo_dir=tmp, tool_version="0.0.0-test")
            self.assertEqual(md["tool"], "codebase-memory-mcp")
            self.assertEqual(md["tool_version"], "0.0.0-test")
            self.assertEqual(md["commit"], sha)
            self.assertTrue(md["generated_at"].endswith("Z"))


class BuildGraphTests(unittest.TestCase):
    def test_projects_architecture_response_to_gitnexus_at_1(self) -> None:
        fake_arch = {
            "nodes": [{"id": 1, "name": "AuthService", "kind": "module"}],
            "edges": [{"source": 1, "target": 2, "kind": "depends_on"}],
        }
        with mock.patch.object(uq, "_mcp_call", return_value=fake_arch):
            graph = uq.build_graph("/fake/binary", "demo")
        self.assertEqual(graph["schema"], "gitnexus@1")
        self.assertEqual(len(graph["nodes"]), 1)
        self.assertEqual(graph["nodes"][0]["label"], "AuthService")
        self.assertEqual(len(graph["links"]), 1)
        self.assertEqual(graph["links"][0]["source"], "1")


if __name__ == "__main__":
    unittest.main()
