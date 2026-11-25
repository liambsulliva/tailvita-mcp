"""The MCP server's route: the local network first, the tailnet only as a fallback."""
import importlib.util
import os
import unittest
from unittest import mock

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools")


def load(**env):
    with mock.patch.dict(os.environ, env):
        for name in ("VITA_HOST", "VITA_LAN_HOST", "VITA_TAILNET_HOST"):
            if name not in env:
                os.environ.pop(name, None)
        spec = importlib.util.spec_from_file_location("mcp_server", os.path.join(TOOLS, "mcp_server.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    return module


def answer(host=None, swept=()):
    return {"host": host, "version": None, "network": "192.168.4.0/22" if host else None, "networks": ["192.168.4.0/22"], "swept": list(swept)}


class RouteTests(unittest.TestCase):
    def setUp(self):
        self.server = load()
        self.calls = []
        self.answers = []

        def find(candidates, skip):
            self.calls.append((list(candidates), list(skip)))
            return self.answers.pop(0)
        self.server.ROUTE.finder.find = find

    def test_lan_first_and_kept_for_a_while(self):
        self.answers = [answer("192.168.4.30", ["192.168.4.0/22"])]
        self.assertEqual(self.server.ROUTE.pick(), ("lan", "192.168.4.30"))
        self.assertEqual(self.server.vita_host(), "192.168.4.30")
        self.assertEqual(self.calls, [(["10.0.0.151"], [])])

    def test_tailnet_when_the_lan_has_no_vita_then_searched_again_by_status(self):
        self.answers = [answer(swept=["192.168.4.0/22"]), answer(), answer("192.168.4.30")]
        self.assertEqual(self.server.ROUTE.pick(), ("tailnet", "100.96.148.83"))
        self.server.ROUTE.until = 0
        self.assertEqual(self.server.ROUTE.pick(), ("tailnet", "100.96.148.83"))
        self.assertEqual(self.calls[1][1], ["192.168.4.0/22"])
        self.assertEqual(self.server.ROUTE.pick(fresh=True), ("lan", "192.168.4.30"))
        self.assertEqual(self.calls[2][1], [])

    def test_found_address_is_tried_first_next_time(self):
        self.answers = [answer("192.168.4.30"), answer("192.168.4.30")]
        self.server.ROUTE.pick()
        self.server.ROUTE.until = 0
        self.server.ROUTE.pick()
        self.assertEqual(self.calls[1][0], ["192.168.4.30", "10.0.0.151"])

    def test_vita_host_still_pins(self):
        self.assertEqual(load(VITA_HOST="192.0.2.7").ROUTE.pick(), ("forced", "192.0.2.7"))


if __name__ == "__main__":
    unittest.main()
