"""The store's report, as the Monitor's Conversation cache card reads it: no store, no GPU, no engine needed.

The shapes exercised here are the ones a live 0.5.7 store answers (tools/lmcache/http_probe.py prints a real body);
the numbers are small stand-ins.
"""
import json
import os
import threading
import time
import unittest
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from serve import lmcache_status
from serve.lmcache_status import http_from_rpc, model_name, reduce_status, store_url

STATUS = {
    "is_healthy": True,
    "engine_type": "EngineDriven",
    "chunk_size": 256,
    "storage_manager": {"l1_managers": {"_default": {"total_object_count": 24,
                                                     "memory_used_bytes": 12884901888,
                                                     "memory_total_bytes": 51539607552,
                                                     "backend": "LocalCPUBackend"}}},
    "non_cuda_context_meta": {"4242": {"model_name": "qwen3.8-flash-next", "block_size": 256,
                                       "world_size": 1, "use_mla": True}},
}


class ReduceTest(unittest.TestCase):
    def test_a_store_that_is_down_is_unavailable(self):
        for body in (None, {}, [], "nope"):
            self.assertFalse(reduce_status(body)["available"])

    def test_the_pool_comes_from_storage_manager(self):
        view = reduce_status(STATUS, "qwen3.8-flash-next")
        self.assertTrue(view["available"] and view["healthy"])
        self.assertEqual(view["chunk_size"], 256)
        self.assertEqual(view["store"]["objects"], 24)
        self.assertEqual(view["store"]["backend"], "LocalCPUBackend")
        self.assertAlmostEqual(view["store"]["ratio"], 12884901888 / 51539607552)

    def test_the_pool_also_comes_from_l1_usage(self):
        view = reduce_status({"l1_usage": [1024, 4096]})
        self.assertEqual((view["store"]["used_bytes"], view["store"]["total_bytes"]), (1024, 4096))
        self.assertEqual(view["store"]["ratio"], 0.25)

    def test_alone_in_the_pool_the_totals_are_this_models(self):
        mine = reduce_status(STATUS, "qwen3.8-flash-next")["mine"]
        self.assertTrue(mine["exclusive"] and mine["registered"])
        self.assertEqual(mine["instances"], 1)
        self.assertEqual((mine["objects"], mine["bytes"]), (24, 12884901888))

    def test_a_shared_pool_reports_no_per_model_share(self):
        status = json.loads(json.dumps(STATUS))
        status["non_cuda_context_meta"]["5150"] = {"model_name": "other-model", "block_size": 256}
        view = reduce_status(status, "qwen3.8-flash-next")
        self.assertEqual(view["model_count"], 2)
        self.assertTrue(view["mine"]["shared"])
        self.assertFalse(view["mine"]["exclusive"])
        self.assertIsNone(view["mine"]["bytes"])       # not inventing a share of a shared pool
        self.assertIsNone(view["mine"]["objects"])

    def test_a_model_that_never_registered_says_so(self):
        mine = reduce_status(STATUS, "a-different-model")["mine"]
        self.assertFalse(mine["registered"])
        self.assertEqual(mine["instances"], 0)
        self.assertIsNone(mine["block_size"])

    def test_a_boolean_is_not_a_measurement(self):
        view = reduce_status({"chunk_size": True, "l1_usage": [True, "1024"]})
        self.assertIsNone(view["chunk_size"])
        self.assertIsNone(view["store"]["used_bytes"])
        self.assertIsNone(view["store"]["ratio"])


class SettingsTest(unittest.TestCase):
    def test_the_report_plane_follows_the_data_plane(self):
        self.assertEqual(http_from_rpc("tcp://nas:5555"), "http://nas:8080")
        self.assertEqual(http_from_rpc("192.168.1.9"), "http://192.168.1.9:8080")
        self.assertEqual(http_from_rpc("tcp://[::1]:5555"), "http://[::1]:8080")
        self.assertEqual(http_from_rpc(""), "")
        self.assertEqual(http_from_rpc(None), "")

    def test_the_http_setting_wins(self):
        self.assertEqual(store_url({"STRATA_LMCACHE_HTTP": "http://nas:8081/", "STRATA_LMCACHE": "tcp://x:5555"}),
                         "http://nas:8081")
        self.assertEqual(store_url({"STRATA_LMCACHE": "tcp://nas:5555"}), "http://nas:8080")
        self.assertEqual(store_url({}), "")            # off: nothing is polled

    def test_the_namespace_defaults_to_the_engines_own(self):
        self.assertEqual(model_name({"STRATA_LMCACHE_NAMESPACE": "qwen"}), "qwen")
        self.assertEqual(model_name({}), "strata")


class _Status(BaseHTTPRequestHandler):
    body = json.dumps(STATUS).encode()

    def do_GET(self):                                  # noqa: N802 (http.server's spelling)
        if self.path != "/status":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(self.body)))
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, *args):
        pass


class LiveTest(unittest.TestCase):
    """One real request through urllib, against a stand-in for the store's report plane."""

    def setUp(self):
        lmcache_status.reset()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), _Status)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        lmcache_status.reset()

    def test_a_reachable_store_is_reported_without_blocking_the_caller(self):
        env = {"STRATA_LMCACHE_HTTP": f"http://127.0.0.1:{self.port}",
               "STRATA_LMCACHE_NAMESPACE": "qwen3.8-flash-next",
               # a proxy in the environment must not be used for a store on this host
               "HTTP_PROXY": "http://192.0.2.1:9", "http_proxy": "http://192.0.2.1:9"}
        started = time.monotonic()
        first = lmcache_status.view(env)               # starts the poller; the fetch is on its thread
        self.assertLess(time.monotonic() - started, 1.0)
        self.assertTrue(first["configured"])
        deadline = time.monotonic() + 3.0
        got = first
        while time.monotonic() < deadline:
            got = lmcache_status.view(env)
            if got.get("available"):
                break
            time.sleep(0.05)
        self.assertTrue(got.get("available"), got)
        self.assertEqual(got["mine"]["objects"], 24)
        self.assertEqual(got["url"], f"http://127.0.0.1:{self.port}")

    def test_an_unreachable_store_is_unavailable_not_an_exception(self):
        env = {"STRATA_LMCACHE_HTTP": "http://127.0.0.1:9"}       # nothing listens there
        lmcache_status.view(env)
        deadline = time.monotonic() + 4.0
        got = {}
        while time.monotonic() < deadline:
            got = lmcache_status.view(env)
            if got.get("error"):
                break
            time.sleep(0.05)
        self.assertFalse(got.get("available"))
        self.assertTrue(got.get("error"))

    def test_a_store_that_is_switched_off_is_not_polled(self):
        got = lmcache_status.view({})
        self.assertEqual((got["available"], got["configured"]), (False, False))
        self.assertIsNone(lmcache_status._thread)      # no thread was even started


if __name__ == "__main__":
    unittest.main()
