"""Generate content/scenes/escalator_etiquette.scene.json.

Two identical halls side by side, each with one up escalator (13 m long, 7.5 m
rise, 0.5 m/s) to an upper concourse, fed by the same arrival rate:

  Hall A (west): "keep left, walk right" - 75% stand on the left, 25% walk on the right;
                 walkers are heavier (85-100 kg vs 55-80 kg), so they push past standers.
  Hall B (east): "stand on both sides"   - everyone stands, on the side they are already on.

Everything people decide - queue manners, side, standing or walking pace, boarding
gap, squeezing on, clearing the landing - comes from their behaviour trees
(content/behaviours/escalator_*.bt.json); the escalators only say that their steps move.

The numbers follow CNA's report on SMRT's 2026 trial at City Hall: a standing
ride of about 30 s, about 20 s for those who walk up.

Run from the repository root:  python tools/make_escalator_scene.py
"""

import json
from pathlib import Path

RISE = 7.5          # m, floor to floor
RUN = 13.0          # m, horizontal length of the escalator (30 degrees)
STEPS = 38
ESC_WIDTH = 2.4     # m between balustrades; the navmesh loses ~0.5 m per side, leaving two 0.7 m lanes
HALL_HALF = 7.0     # m, half width of each hall (halls span 0..14 m either side of the divider)
LOW_Z0, LOW_Z1 = -25.0, -RUN / 2      # lower concourse
UP_Z0, UP_Z1 = RUN / 2, 20.0          # upper concourse
WALL = 0.3

objects = []
next_id = [1]


def new_id():
    i = next_id[0]
    next_id[0] += 1
    return i


def box(name, x0, x1, y0, y1, z0, z1, tags, color):
    objects.append({
        "id": new_id(), "name": name, "type": "box",
        "position": [round((x0 + x1) / 2, 3), y0, round((z0 + z1) / 2, 3)],
        "rotationY": 0.0,
        "size": [round(x1 - x0, 3), round(y1 - y0, 3), round(z1 - z0, 3)],
        "tags": tags, "color": color,
    })


FLOOR, WALLC, RAIL, STEEL, GLASS = "#5c626a", "#9aa1a9", "#7d858f", "#3d4249", "#6f8796"

# Floors (shared slabs; the halls are split by a dividing wall).
box("Lower concourse", -14, 14, -0.2, 0.0, LOW_Z0, LOW_Z1, ["walkable"], FLOOR)
box("Upper concourse", -14, 14, RISE - 0.2, RISE, UP_Z0, UP_Z1, ["walkable"], FLOOR)

# Outer walls and the divider, both levels.
box("Wall south", -14 - WALL, 14 + WALL, 0, RISE, LOW_Z0 - WALL, LOW_Z0, ["obstacle"], WALLC)
box("Wall west (lower)", -14 - WALL, -14, 0, RISE, LOW_Z0, LOW_Z1, ["obstacle"], WALLC)
box("Wall east (lower)", 14, 14 + WALL, 0, RISE, LOW_Z0, LOW_Z1, ["obstacle"], WALLC)
box("Divider (lower)", -WALL / 2, WALL / 2, 0, RISE, LOW_Z0, LOW_Z1, ["obstacle"], WALLC)
box("Wall north", -14 - WALL, 14 + WALL, RISE, RISE + 3, UP_Z1, UP_Z1 + WALL, ["obstacle"], WALLC)
box("Wall west (upper)", -14 - WALL, -14, RISE, RISE + 3, UP_Z0, UP_Z1, ["obstacle"], WALLC)
box("Wall east (upper)", 14, 14 + WALL, RISE, RISE + 3, UP_Z0, UP_Z1, ["obstacle"], WALLC)
box("Divider (upper)", -WALL / 2, WALL / 2, RISE, RISE + 3, UP_Z0, UP_Z1, ["obstacle"], WALLC)

halls = [
    ("A", -7.0, "Hall A: keep left, walk right"),
    ("B", 7.0, "Hall B: stand on both sides"),
]
escalator_ids = {}
for key, cx, title in halls:
    half = ESC_WIDTH / 2
    # Lower hall's north wall, open only where the escalator starts.
    box(f"{key} wall left of escalator", cx - HALL_HALF, cx - half - 0.15, 0, RISE - 0.2,
        LOW_Z1 - WALL, LOW_Z1, ["obstacle"], WALLC)
    box(f"{key} wall right of escalator", cx + half + 0.15, cx + HALL_HALF, 0, RISE - 0.2,
        LOW_Z1 - WALL, LOW_Z1, ["obstacle"], WALLC)
    # Railing along the upper edge, open where the escalator arrives.
    # The landing opening is wider than the escalator so people stepping off at the
    # sides do not catch the railing ends.
    box(f"{key} railing left", cx - HALL_HALF, cx - half - 0.6, RISE, RISE + 1.1,
        UP_Z0, UP_Z0 + 0.15, ["obstacle"], RAIL)
    box(f"{key} railing right", cx + half + 0.6, cx + HALL_HALF, RISE, RISE + 1.1,
        UP_Z0, UP_Z0 + 0.15, ["obstacle"], RAIL)

    # The escalator: stairs that move (see sim/src/Escalators.cpp for the tags).
    eid = new_id()
    escalator_ids[key] = eid
    objects.append({
        "id": eid, "name": f"{title} (escalator)", "type": "stairs",
        "position": [cx, 0.0, 0.0], "rotationY": 0.0,
        "size": [ESC_WIDTH, RISE, RUN], "steps": STEPS,
        "tags": ["walkable", "escalator", "escalator:speed=0.5"],
        "color": STEEL,
    })
    # Balustrades: a skirt panel up to the handrail line on each side.
    for side, sx in (("left", cx - half - 0.08), ("right", cx + half + 0.08)):
        box(f"{key} balustrade {side} (base)", sx - 0.07, sx + 0.07, 0, 1.0, -RUN / 2, RUN / 2,
            ["obstacle"], GLASS)
        objects.append({
            "id": new_id(), "name": f"{key} balustrade {side}", "type": "ramp",
            "position": [sx, 1.0, 0.0], "rotationY": 0.0, "size": [0.14, RISE, RUN],
            "tags": ["obstacle"], "color": GLASS,
        })

goals = []
spawners = []
mixes = {
    "A": [{"type": "escalator_stander_left", "weight": 0.75}, {"type": "escalator_walker_right", "weight": 0.25}],
    "B": [{"type": "escalator_stander_either", "weight": 1.0}],
}
for key, cx, title in halls:
    gid = new_id()
    goals.append({
        "id": gid, "name": f"{key} exit (upper)",
        "area": {"center": [cx, UP_Z1 - 2.5], "halfExtents": [HALL_HALF - 1.0, 1.5], "elevation": RISE},
        "capacity": 0, "serviceTime": 0.0, "queueDirection": [0.0, -1.0], "queueSpacing": 0.8,
    })
    spawners.append({
        "id": new_id(), "name": f"{key} arrivals",
        "area": {"center": [cx, LOW_Z0 + 3.0], "halfExtents": [HALL_HALF - 1.0, 2.0], "elevation": 0.0},
        "rate": 1.2, "total": 360, "startTime": 0.0, "stopTime": None, "goal": gid,
        "groupSize": [1, 1],
        "agent": {"agentType": 0, "color": "#d98c40", "colorJitter": 0.1, "maxAcceleration": 3.0,
                  "maxSpeedFactor": 1.35, "preferredSpeed": [1.15, 1.55], "radius": [0.22, 0.26]},
        "agentMix": mixes[key],
    })

# Embed the agent types and their behaviour trees so the scene also loads without
# content/. How people ride the escalator is behaviour: each tree sets it with the
# SetEscalatorStyle node; the escalator objects only say that their steps move.
content = Path(__file__).resolve().parent.parent / "content"
agent_types = [json.loads((content / "agents" / f"{name}.agent.json").read_text(encoding="utf-8"))
               for name in ("escalator_stander_left", "escalator_walker_right", "escalator_stander_either")]
behaviours = [json.loads((content / "behaviours" / f"{t['behaviour']}.bt.json").read_text(encoding="utf-8"))
              for t in agent_types]

scene = {
    "format": "crowdsim.scene", "version": 1, "name": "escalator_etiquette",
    "behaviours": behaviours,
    "agentTypes": agent_types,
    "navigation": {"agentHeight": 1.9, "agentMaxClimb": 0.45, "agentMaxSlope": 45.0, "agentRadius": 0.3,
                   "cellHeight": 0.1, "cellSize": 0.2, "tileSize": 64},
    "nextId": next_id[0],
    "objects": objects, "goals": goals, "spawners": spawners, "links": [],
}
out = Path(__file__).resolve().parent.parent / "content" / "scenes" / "escalator_etiquette.scene.json"
out.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
print(f"wrote {out} ({len(objects)} objects)")
