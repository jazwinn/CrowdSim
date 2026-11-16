"""Generate content/scenes/travelator_tunnel.scene.json and its agent types and behaviour
trees (content/agents/tunnel_*.agent.json, content/behaviours/tunnel_*.bt.json).

A long underground interchange tunnel (140 m x 16 m) between two station concourses,
like the link tunnels of Singapore's MRT:

  wall ads | travelator -> B | walkway | ad box (ads both sides) | walkway | travelator -> A | wall ads

  - A moving walkway (travelator, 100 m, 0.75 m/s) along each wall, one each way. People
    keep left, so the one on the left as you face station B carries people towards B.
  - An open walkway beside each travelator, between it and the ad box in the middle.
  - The ad box: a long rectangular unit down the centre with ad panels on both long
    sides, facing the travelators; more ads line the tunnel walls behind the travelators.

People arrive at both concourses. Some ride the travelator in their direction, standing
on the left or walking past on the right; the rest walk the walkway. Which they do is
behaviour: riders head for the start of their travelator first (a goal on its first
metres), walkers go straight for the far concourse. The travelators themselves only say
that their surface moves (a box tagged "escalator"), and routing never sends anyone
against them.

Run from the repository root:  python tools/make_travelator_tunnel_scene.py
"""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"

LENGTH = 140.0                 # m, concourse A at z = 0..12, concourse B at z = 128..140
HALF = 8.0                     # tunnel half width
WALL_H = 4.5
TRAV_Z0, TRAV_Z1 = 20.0, 120.0  # travelators (100 m)
TRAV_X0, TRAV_X1 = 5.2, 7.6     # |x| of each travelator
TRAV_SPEED = 0.75               # m/s
BALUSTRADE = 0.2
INSET = 0.6                     # balustrades stop short of the ends (see the mall)
BOX_HALF = 1.6                  # central ad box |x| < 1.6
BOX_Z0, BOX_Z1 = 24.0, 116.0
BOX_H = 2.6

FLOOR_C, WALL_C, BELT_C, RAIL_C, BOX_C, CONCOURSE_C = (
    "#b9b2a6", "#c9c4bc", "#3b3f45", "#9aa1a9", "#5d6066", "#a59d90")
ADS = ["#ff5a1f", "#ff5a1f", "#ff5a1f", "#f2f2f2"]  # mostly one campaign, the odd other one

objects, goals, spawners, groups = [], [], [], []
next_id = [1]


def new_id():
    next_id[0] += 1
    return next_id[0] - 1


def r3(v):
    return round(v, 3)


current = [None]  # group new entities go in


def group(name, parent=None):
    g = {"id": new_id(), "name": name}
    if parent is not None:
        g["parent"] = parent
    groups.append(g)
    return g["id"]


def box(name, x0, x1, y0, y1, z0, z1, tags, color):
    o = {"id": new_id(), "name": name, "type": "box",
         "position": [r3((x0 + x1) / 2), r3(y0), r3((z0 + z1) / 2)], "rotationY": 0.0,
         "size": [r3(x1 - x0), r3(y1 - y0), r3(z1 - z0)], "tags": tags, "color": color}
    if current[0] is not None:
        o["parent"] = current[0]
    objects.append(o)
    return o["id"]


def goal(name, cx, cz, hx, hz):
    g = {"id": new_id(), "name": name,
         "area": {"center": [r3(cx), r3(cz)], "halfExtents": [r3(hx), r3(hz)], "elevation": 0.0},
         "capacity": 0, "serviceTime": 0.0, "queueDirection": [0.0, -1.0], "queueSpacing": 0.8}
    if current[0] is not None:
        g["parent"] = current[0]
    goals.append(g)
    return g["id"]


# --- Structure ------------------------------------------------------------------------------
current[0] = group("Tunnel")
box("Concourse A floor", -HALF - 6, HALF + 6, -0.2, 0.0, 0.0, 12.0, ["walkable"], CONCOURSE_C)
box("Concourse B floor", -HALF - 6, HALF + 6, -0.2, 0.0, LENGTH - 12.0, LENGTH, ["walkable"], CONCOURSE_C)
box("Tunnel floor", -HALF, HALF, -0.2, 0.0, 12.0, LENGTH - 12.0, ["walkable"], FLOOR_C)
for sx, side in ((-1, "west"), (1, "east")):
    box(f"Tunnel wall {side}", sx * HALF - (0.3 if sx < 0 else 0.0), sx * HALF + (0.3 if sx > 0 else 0.0),
        0.0, WALL_H, 12.0, LENGTH - 12.0, ["obstacle"], WALL_C)
for z, name in ((0.0, "A"), (LENGTH, "B")):
    zz = z - 0.3 if z == 0.0 else z
    box(f"Concourse {name} end wall", -HALF - 6, HALF + 6, 0.0, WALL_H, zz, zz + 0.3, ["obstacle"], WALL_C)
for z0, z1, name in ((0.0, 12.0, "A"), (LENGTH - 12.0, LENGTH, "B")):
    for sx in (-1, 1):
        x = sx * (HALF + 6)
        box(f"Concourse {name} side wall", min(x, x + sx * 0.3), max(x, x + sx * 0.3), 0.0, WALL_H, z0, z1,
            ["obstacle"], WALL_C)
    # The concourse is wider than the tunnel: close off the corners beside the tunnel mouth.
    zc = 12.0 if name == "A" else LENGTH - 12.0
    zs = zc if name == "A" else zc - 0.3
    for sx in (-1, 1):
        box(f"Concourse {name} wall", min(sx * HALF, sx * (HALF + 6)), max(sx * HALF, sx * (HALF + 6)), 0.0, WALL_H,
            zs, zs + 0.3, ["obstacle"], WALL_C)

# --- Travelators ------------------------------------------------------------------------------
current[0] = group("Travelators")
for sx, label, down in ((-1, "to B", False), (1, "to A", True)):
    x0, x1 = sorted((sx * TRAV_X0, sx * TRAV_X1))
    tags = ["walkable", "escalator", f"escalator:speed={TRAV_SPEED}"] + (["escalator:down"] if down else [])
    # A plate 5 cm above the floor (also keeps it from flickering against the floor).
    box(f"Travelator {label}", x0, x1, -0.1, 0.05, TRAV_Z0, TRAV_Z1, tags, BELT_C)
    inner = sx * TRAV_X0
    for bx, name in ((inner - sx * BALUSTRADE / 2, "inner"), (sx * (TRAV_X1 + 0.1), "outer")):
        box(f"Travelator {label} handrail ({name})", bx - 0.1, bx + 0.1, 0.0, 1.0,
            TRAV_Z0 + INSET, TRAV_Z1 - INSET, ["obstacle"], RAIL_C)

# --- Ads ------------------------------------------------------------------------------------
current[0] = group("Ads")
box("Ad box", -BOX_HALF, BOX_HALF, 0.0, BOX_H, BOX_Z0, BOX_Z1, ["obstacle"], BOX_C)
panel_len, gap = 8.0, 1.5
z = BOX_Z0 + 1.0
k = 0
while z + panel_len <= BOX_Z1 - 1.0:
    colour = ADS[k % len(ADS)]
    for sx in (-1, 1):
        x = sx * BOX_HALF
        box(f"Ad panel (box, {'west' if sx < 0 else 'east'} {k + 1})", min(x, x + sx * 0.05),
            max(x, x + sx * 0.05), 0.3, BOX_H - 0.2, z, z + panel_len, ["obstacle"], colour)
    z += panel_len + gap
    k += 1
z = TRAV_Z0 + 2.0
k = 0
while z + 9.0 <= TRAV_Z1 - 2.0:
    colour = ADS[(k + 1) % len(ADS)]
    for sx in (-1, 1):
        x = sx * HALF
        box(f"Ad panel (wall, {'west' if sx < 0 else 'east'} {k + 1})", min(x, x - sx * 0.05),
            max(x, x - sx * 0.05), 1.0, 3.6, z, z + 9.0, ["obstacle"], colour)
    z += 10.0
    k += 1

# --- Goals and arrivals ---------------------------------------------------------------------
current[0] = group("Goals and arrivals")
exit_a = goal("Station A", 0.0, 4.0, HALF + 4.0, 2.5)
exit_b = goal("Station B", 0.0, LENGTH - 4.0, HALF + 4.0, 2.5)
# The first metres of each travelator: riders head here first.
board_b = goal("Travelator to B (start)", -(TRAV_X0 + TRAV_X1) / 2, TRAV_Z0 + 1.5, 0.6, 1.0)
board_a = goal("Travelator to A (start)", (TRAV_X0 + TRAV_X1) / 2, TRAV_Z1 - 1.5, 0.6, 1.0)

MIX = {  # role: weight
    "stand": 0.35,  # rides, standing on the left
    "walk": 0.20,   # rides, walking past on the right
    "foot": 0.45,   # walks the walkway
}
for frm, to, sgn, rate in (("A", "B", 1.0, 1.4), ("B", "A", -1.0, 1.1)):
    sid = new_id()
    s = {"id": sid, "name": f"Arrivals at {frm} (to {to})",
         "area": {"center": [0.0, 6.0 if frm == "A" else LENGTH - 6.0], "halfExtents": [HALF + 2.0, 1.5],
                  "elevation": 0.0},
         "rate": rate, "total": 0, "startTime": 0.0, "stopTime": None,
         "goal": exit_b if to == "B" else exit_a, "groupSize": [1, 1],
         "agent": {"agentType": 0, "color": "#4aa3df", "colorJitter": 0.1, "maxAcceleration": 3.0,
                   "maxSpeedFactor": 1.35, "preferredSpeed": [1.1, 1.5], "radius": [0.22, 0.27]},
         "agentMix": [{"type": f"tunnel_{role}_to_{to.lower()}", "weight": w} for role, w in MIX.items()],
         "parent": current[0]}
    spawners.append(s)
    # Spawn on the concourse, away from the exit goal there (which is for people arriving).
    s["area"]["center"][1] = 9.0 if frm == "A" else LENGTH - 9.0


# --- Behaviour trees ------------------------------------------------------------------------
def node(type_, id_, params=None, children=None):
    n = {"type": type_, "id": id_}
    if params:
        n["params"] = params
    if children:
        n["children"] = children
    return n


trees, agent_json = [], []
for to in ("b", "a"):
    start = "Travelator to B (start)" if to == "b" else "Travelator to A (start)"
    for role in MIX:
        name = f"tunnel_{role}_to_{to}"
        steps = [node("SetState", "label", {"name": "walking" if role == "foot" else "to the travelator"})]
        if role != "foot":
            steps += [
                node("SetEscalatorStyle", "style",
                     {"side": "left" if role == "stand" else "right", "walk": 0.0 if role == "stand" else 0.7,
                      "clear": 2.0}),
                node("SetQueueManners", "manners", {"gap": 0.6, "ease": 0.5}),
                node("MoveToGoal", "board", {"goal": start}),
                node("SetState", "riding", {"name": "on the travelator"}),
            ]
        steps += [node("MoveToGoal", "out"), node("Despawn", "gone")]
        trees.append({"format": "crowdsim.bt", "version": 1, "name": name, "root": node("Sequence", "root", None, steps)})
        agent_json.append({
            "format": "crowdsim.agent", "version": 1, "name": name, "behaviour": name,
            "radius": [0.22, 0.27], "preferredSpeed": [1.1, 1.5], "maxSpeedFactor": 1.35, "maxAcceleration": 3,
            "personalSpace": 0.08, "height": 1.72,
            "color": {"stand": "#e8b04a", "walk": "#d9534f", "foot": "#4aa3df"}[role],
            "colorJitter": 0.1, "tags": []})

for t in trees:
    (CONTENT / "behaviours" / f"{t['name']}.bt.json").write_text(json.dumps(t, indent=2) + "\n", encoding="utf-8")
for a in agent_json:
    (CONTENT / "agents" / f"{a['name']}.agent.json").write_text(json.dumps(a, indent=2) + "\n", encoding="utf-8")

scene = {
    "format": "crowdsim.scene", "version": 1, "name": "travelator_tunnel",
    "behaviours": trees, "agentTypes": agent_json,
    "navigation": {"agentHeight": 1.9, "agentMaxClimb": 0.45, "agentMaxSlope": 45.0, "agentRadius": 0.3,
                   "cellHeight": 0.1, "cellSize": 0.2, "tileSize": 64},
    "nextId": next_id[0],
    "groups": groups, "objects": objects, "goals": goals, "spawners": spawners, "links": [],
}
out = CONTENT / "scenes" / "travelator_tunnel.scene.json"
out.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
print(f"wrote {out}: {len(objects)} objects, {len(goals)} goals")
