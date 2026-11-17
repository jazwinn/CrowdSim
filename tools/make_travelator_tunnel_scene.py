"""Generate content/scenes/travelator_tunnel.scene.json and its agent types and behaviour
trees (content/agents/tunnel_*.agent.json, content/behaviours/tunnel_*.bt.json).

A long underground interchange tunnel (140 m x 16 m) between two station concourses,
like the link tunnels of Singapore's MRT:

  wall ads | travelator -> B |   walkway with ad stands down the middle   | travelator -> A | wall ads

Every ad (each face of each stand, each wall poster) is tagged `attention`, so the attention
model measures how well it is seen; scripts/attention_report.lua ranks them.

  - A moving walkway (travelator, 100 m, 0.75 m/s) along each wall, one each way. People
    keep left, so the one on the left as you face station B carries people towards B.
  - An open walkway between the travelators.
  - Down the middle of the walkway, a row of free-standing digital ad stands: a screen
    about 1.2 m wide and 2 m tall on a plinth, showing on both faces, one face towards
    each travelator. More ads line the tunnel walls behind the travelators.

People arrive at both concourses. Some ride the travelator in their direction, standing
on the left or walking past on the right; the rest walk the walkway, keeping to its left
half (waypoints on that side between the ad stands). Which they do is
behaviour: riders head for the start of their travelator first (a goal on its first
metres), walkers go straight for the far concourse. The travelators themselves only say
that their surface moves (a box tagged "escalator"), and routing never sends anyone
against them.

Variant `facing_flow` (travelator_tunnel_stands_facing_flow.scene.json) turns the stands so
their screens face up and down the tunnel, towards people walking at them.

Run from the repository root:  python tools/make_travelator_tunnel_scene.py
"""

import json
import subprocess
import sys
from pathlib import Path

# Variants (placement studies): how the ad stands are turned.
#   across       screens face across the walkway, towards the travelators (as built)
#   facing_flow  screens face up and down the tunnel, towards people walking at them
VARIANTS = {"across": "travelator_tunnel", "facing_flow": "travelator_tunnel_stands_facing_flow"}
if len(sys.argv) < 2:
    for v in VARIANTS:
        subprocess.run([sys.executable, __file__, v], check=True)
    sys.exit(0)
VARIANT = sys.argv[1]

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
STAND_Z0, STAND_Z1 = 26.0, 114.0  # ad stands down the middle, from .. to
STAND_EVERY = 11.0                # m between ad stands
STAND_W = 1.2                     # screen width (along the tunnel)
STAND_D = 0.36                    # thickness (across the tunnel)

FLOOR_C, WALL_C, BELT_C, RAIL_C, STAND_C, CONCOURSE_C = (
    "#b9b2a6", "#c9c4bc", "#3b3f45", "#9aa1a9", "#1e2024", "#a59d90")
ADS = ["#ff5a1f", "#ff5a1f", "#ff5a1f", "#f2f2f2"]  # mostly one campaign, the odd other one
SCREENS = ["#2563d9", "#ff5a1f", "#1aa37a", "#7b3fe4"]  # what the ad stands are showing
# How eye-catching each kind of ad is (attention model, see AttentionModel.hpp): the stands are
# bright moving screens; the wall posters are large but printed.
SCREEN_SALIENCE = 0.9
POSTER_SALIENCE = 0.6
# Attention by role: (share of the time on the phone, inclination to look around).
# Standing riders have time to kill and look at their phones; walking riders and walkers
# are on their way somewhere.
ATTENTION = {"stand": (0.45, 1.0), "walk": (0.15, 0.7), "foot": (0.3, 0.8)}

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
# Free-standing digital ad stands: plinth, dark body, a screen on both faces.
k = 0
z = STAND_Z0
while z <= STAND_Z1 + 1e-6:
    name = f"Ad stand {k + 1}"
    hw, hd = STAND_W / 2, STAND_D / 2
    tags = ["obstacle", "attention", f"attention:salience={SCREEN_SALIENCE}"]
    colour = SCREENS[k % len(SCREENS)]
    if VARIANT == "across":
        box(f"{name} plinth", -hd - 0.04, hd + 0.04, 0.0, 0.45, z - hw - 0.04, z + hw + 0.04, ["obstacle"], STAND_C)
        box(f"{name} body", -hd, hd, 0.45, 2.55, z - hw, z + hw, ["obstacle"], STAND_C)
        for sx, face in ((-1, "W"), (1, "E")):
            x = sx * hd
            box(f"Stand {k + 1} {face}", min(x, x + sx * 0.02), max(x, x + sx * 0.02), 0.6, 2.4,
                z - hw + 0.07, z + hw - 0.07, tags, colour)
    else:
        box(f"{name} plinth", -hw - 0.04, hw + 0.04, 0.0, 0.45, z - hd - 0.04, z + hd + 0.04, ["obstacle"], STAND_C)
        box(f"{name} body", -hw, hw, 0.45, 2.55, z - hd, z + hd, ["obstacle"], STAND_C)
        for sz, face in ((-1, "S"), (1, "N")):  # S faces people walking to B, N those walking to A
            zz = z + sz * hd
            box(f"Stand {k + 1} {face}", -hw + 0.07, hw - 0.07, 0.6, 2.4, min(zz, zz + sz * 0.02),
                max(zz, zz + sz * 0.02), tags, colour)
    z += STAND_EVERY
    k += 1
z = TRAV_Z0 + 2.0
k = 0
while z + 9.0 <= TRAV_Z1 - 2.0:
    colour = ADS[(k + 1) % len(ADS)]
    for sx in (-1, 1):
        x = sx * HALF
        box(f"Wall {'W' if sx < 0 else 'E'} {k + 1}", min(x, x - sx * 0.05),
            max(x, x - sx * 0.05), 1.0, 3.6, z, z + 9.0, ["obstacle", "attention", f"attention:salience={POSTER_SALIENCE}"], colour)
    z += 10.0
    k += 1

# --- Goals and arrivals ---------------------------------------------------------------------
current[0] = group("Goals and arrivals")
exit_a = goal("Station A", 0.0, 4.0, HALF + 4.0, 2.5)
exit_b = goal("Station B", 0.0, LENGTH - 4.0, HALF + 4.0, 2.5)
# The first metres of each travelator: riders head here first.
board_b = goal("Travelator to B (start)", -(TRAV_X0 + TRAV_X1) / 2, TRAV_Z0 + 1.5, 0.6, 1.0)
board_a = goal("Travelator to A (start)", (TRAV_X0 + TRAV_X1) / 2, TRAV_Z1 - 1.5, 0.6, 1.0)
# Walkers keep left too: past each ad stand on their left-hand side. A waypoint on that
# side of the walkway before the first stand, in every gap and after the last; each hop
# is then straightest along that side. (One waypoint is not enough: routes follow chains
# of navmesh polygons, which cross from side to side between the stands.)
WALK_SIDE = 2.6  # |x| of the middle of each half of the walkway
stand_zs = []
z = STAND_Z0
while z <= STAND_Z1 + 1e-6:
    stand_zs.append(z)
    z += STAND_EVERY
waypoint_zs = [stand_zs[0] - 4.0] + [(a + b) / 2 for a, b in zip(stand_zs, stand_zs[1:])] + [stand_zs[-1] + 4.0]
keep_left = {"b": [], "a": []}
for i, wz in enumerate(waypoint_zs):
    keep_left["b"].append(f"Walkway west {i + 1} (to B)")
    goal(keep_left["b"][-1], -WALK_SIDE, wz, 1.8, 0.8)
for i, wz in enumerate(reversed(waypoint_zs)):
    keep_left["a"].append(f"Walkway east {i + 1} (to A)")
    goal(keep_left["a"][-1], WALK_SIDE, wz, 1.8, 0.8)

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
        phone, interest = ATTENTION[role]
        steps = [node("SetState", "label", {"name": "walking" if role == "foot" else "to the travelator"}),
                 node("SetAttention", "attention", {"phone": phone, "interest": interest})]
        if role == "foot":
            steps += [node("MoveToGoal", f"keep_left_{i + 1}", {"goal": g}) for i, g in enumerate(keep_left[to])]
        else:
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
    "format": "crowdsim.scene", "version": 1, "name": VARIANTS[VARIANT],
    "behaviours": trees, "agentTypes": agent_json,
    "navigation": {"agentHeight": 1.9, "agentMaxClimb": 0.45, "agentMaxSlope": 45.0, "agentRadius": 0.3,
                   "cellHeight": 0.1, "cellSize": 0.2, "tileSize": 64},
    "nextId": next_id[0],
    "groups": groups, "objects": objects, "goals": goals, "spawners": spawners, "links": [],
}
out = CONTENT / "scenes" / f"{VARIANTS[VARIANT]}.scene.json"
out.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
print(f"wrote {out}: {len(objects)} objects, {len(goals)} goals")
