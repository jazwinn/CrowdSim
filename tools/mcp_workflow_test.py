#!/usr/bin/env python3
"""End-to-end MCP check (M8): drive `crowdsim_headless --mcp-stdio` like an AI client.

From an empty scene, over JSON-RPC only: build an environment, define two agent
types with their own behaviour trees, run 60 simulated seconds headless and read
back metrics. Exits non-zero on any failure.

    python tools/mcp_workflow_test.py build/windows-msvc/bin/Release/crowdsim_headless.exe
"""

import json
import subprocess
import sys


class Client:
    def __init__(self, exe):
        self.proc = subprocess.Popen(
            [exe, "--mcp-stdio", "--no-content", "--log-level", "warn"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding="utf-8")
        self.next_id = 1

    def request(self, method, params=None):
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.next_id += 1
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(f"server closed while waiting for {method}")
        reply = json.loads(line)
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]

    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def tool(self, name, /, **arguments):
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(f"{name} failed: {result['content'][0]['text']}")
        return result.get("structuredContent") or json.loads(result["content"][0]["text"])

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=30)


def bt(name, root):
    return {"format": "crowdsim.bt", "version": 1, "name": name, "root": root}


def main():
    exe = sys.argv[1]
    c = Client(exe)
    init = c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "mcp_workflow_test", "version": "1"}})
    assert init["serverInfo"]["name"] == "crowdsim", init
    c.notify("notifications/initialized")
    tools = {t["name"] for t in c.request("tools/list")["tools"]}
    for required in ["scene_new", "object_place", "goal_create", "bt_set", "agent_type_define",
                     "spawner_create", "sim_run_for", "metrics_get", "agents_query", "bt_node_types"]:
        assert required in tools, f"missing tool {required}"

    # Environment: a 60 x 30 m hall with a wall that has a 6 m doorway.
    c.tool("scene_new", name="mcp_hall")
    c.tool("object_place", object={"name": "Floor", "type": "box", "position": [0, -0.2, 0],
                                   "size": [60, 0.2, 30], "tags": ["walkable"]})
    c.tool("object_place", object={"name": "Wall north", "type": "box", "position": [0, 0, 9],
                                   "size": [0.4, 3, 12], "tags": ["obstacle"]})
    c.tool("object_place", object={"name": "Wall south", "type": "box", "position": [0, 0, -9],
                                   "size": [0.4, 3, 12], "tags": ["obstacle"]})
    c.tool("goal_create", goal={"name": "Exit", "area": {"center": [27, 0], "halfExtents": [2, 12], "elevation": 0}})

    # Two agent types with different behaviour trees.
    c.tool("bt_set", tree=bt("rusher", {"type": "Sequence", "children": [
        {"type": "SetState", "params": {"name": "rushing"}},
        {"type": "SetSpeed", "params": {"multiplier": 1.3}},
        {"type": "MoveToGoal"}, {"type": "Despawn"}]}))
    c.tool("bt_set", tree=bt("dawdler", {"type": "Sequence", "children": [
        {"type": "SetState", "params": {"name": "dawdling"}},
        {"type": "Wander", "params": {"radius": 6}},
        {"type": "Wait", "params": {"seconds": 2}},
        {"type": "MoveToGoal"}, {"type": "Despawn"}]}))
    c.tool("agent_type_define", type={"name": "rusher", "behaviour": "rusher", "color": "#e05040"})
    c.tool("agent_type_define", type={"name": "dawdler", "behaviour": "dawdler", "color": "#40a0e0"})
    c.tool("spawner_create", spawner={"name": "Entrance", "rate": 10, "total": 300, "goal": "Exit",
                                      "area": {"center": [-27, 0], "halfExtents": [2, 12], "elevation": 0},
                                      "agentMix": [{"type": "rusher", "weight": 1}, {"type": "dawdler", "weight": 1}]})

    run = c.tool("sim_run_for", seconds=60)
    m = run["metrics"]
    assert run["steps"] == 1800, run
    assert m["spawned"] == 300, m
    assert m["arrived"] > 100, m
    assert m["goals"][0]["name"] == "Exit" and m["goals"][0]["arrivals"] == m["arrived"], m
    sample = c.tool("agents_query", limit=10)
    assert sample["listed"] <= 10, sample
    metrics = c.tool("metrics_get")
    assert metrics["time"] == 60, metrics

    resources = {r["uri"] for r in c.request("resources/list")["resources"]}
    assert "crowdsim://scene" in resources and "crowdsim://bt/rusher" in resources, resources
    scene = json.loads(c.request("resources/read", {"uri": "crowdsim://scene"})["contents"][0]["text"])
    assert len(scene["objects"]) == 3, scene

    c.close()
    print(json.dumps({"ok": True, "tools": len(tools), "steps": run["steps"], "wallMs": run["wallMs"],
                      "spawned": m["spawned"], "arrived": m["arrived"], "aliveByState": m["aliveByState"]}))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001 - report any failure as a test failure
        print(f"FAILED: {e}", file=sys.stderr)
        sys.exit(1)
