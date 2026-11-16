"""Generate the four-storey shopping mall: content/scenes/shopping_mall.scene.json plus
its agent types and behaviour trees (content/agents/mall_*.agent.json,
content/behaviours/mall_*.bt.json).

The building (132 m x 60 m, floors at 0, 5, 10 and 15 m):

  - A galleria: a central void (92 m x 14 m) on the upper floors with corridors
    along both sides and across the ends.
  - Two cascades of escalators rise through the void from the far ends towards the
    middle, two up escalators (one on each corridor's side) and two down escalators
    between them, with a bridge across the void at every landing; the top bridge in
    the middle joins both cascades.
  - Shops along the north and south walls (four per side per floor) and an anchor
    store at each end of every floor: supermarket and home store on the ground floor,
    department stores and electronics/sports above, a food court and a cinema on the
    top floor.
  - Main entrances on the south and north sides of the ground floor, from plazas.
  - Six enclosed dog-leg fire stairs: behind the north and south shops (reached by
    passages between them) and at both ends of the building (from the anchor stores),
    each with a ground-floor door outside. The ground-floor anchor stores have two more
    exit doors each. All ten emergency exits are goals ("Emergency exit (...)") on aprons
    outside. The stairs and aprons are tagged `emergency`: shoppers keep out of them, and
    only agents whose tree runs UseEmergencyRoutes (evacuees) go that way.

Shoppers come in through either entrance, visit a few shops on floors of their
choosing (more of them on the lower floors), browse, and leave by the entrance they
came in by. Diners go to the food court and cinema-goers to the cinema. Everything
they decide is in their behaviour trees; the escalators only move their steps, and
goal routing never sends anyone up a down escalator.

Walls are a little over head height (cut away) so the shops can be seen from above.

Run from the repository root:  python tools/make_mall_scene.py
"""

import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"

# --- Dimensions --------------------------------------------------------------------------------
FLOORS = 4
STOREY = 5.0                    # m, floor to floor
SLAB = 0.2
X0, X1 = -66.0, 66.0            # outer walls
Z0, Z1 = -30.0, 30.0
VOID_X, VOID_Z = 46.0, 7.0      # the galleria void spans |x| < 46, |z| < 7 on the upper floors
SHOP_Z = 16.0                   # shop fronts at |z| = 16; corridors 7..16 wide 9 m
ANCHOR_X = 54.0                 # anchor stores beyond |x| = 54
WALL_H = 3.4                    # cut-away walls
WALL = 0.3
RAIL_H = 1.1

ESC_WIDTH = 2.4                 # two lanes after the navmesh erodes ~0.5 m per side
ESC_RUN = STOREY / math.tan(math.radians(30.0))  # 8.66 m at 30 degrees
ESC_STEPS = 26
ESC_Z = (4.2, 1.4)              # up escalators at z = +-4.2 (outside), down at +-1.4 (middle)
BRIDGE = 6.0                    # length of the landing bridges along the void
CASCADE_START = 45.0            # ground-floor foot of each cascade at |x| = 45

STAIR_X = 30.0                  # fire stairs behind the north and south shops at x = +-30
STAIR_HALF = 2.5
PASSAGE = 3.0                   # passage between shops to the stairs
ENTRANCE = 8.0                  # ground-floor entrance passages at x = 0
EXIT_DOOR = 2.4                 # emergency exit doors from the anchor stores
ANCHOR_EXIT_Z = (-20.0, 20.0)

FLOOR_NAMES = ["G", "L1", "L2", "L3"]
ANCHORS = {  # (west, east) per floor
    0: ("Supermarket", "Home store"),
    1: ("Department store", "Electronics"),
    2: ("Department store (upper)", "Sports"),
    3: ("Food court", "Cinema"),
}
SHOP_KINDS = {
    0: ["Pharmacy", "Bank", "Florist", "Phones", "Bakery", "Optician", "Newsagent", "Shoes"],
    1: ["Fashion", "Fashion", "Shoes", "Jewellery", "Fashion", "Cosmetics", "Kidswear", "Bags"],
    2: ["Books", "Toys", "Homeware", "Music", "Outdoor", "Games", "Lighting", "Gifts"],
    3: ["Restaurant", "Restaurant", "Cafe", "Bar", "Restaurant", "Dessert", "Arcade", "Bowling"],
}

FLOOR_C, WALL_C, SHOP_C, RAIL_C, STEEL_C, ANCHOR_C, STAIR_C, PLAZA_C, TABLE_C = (
    "#6a6f76", "#a8aeb5", "#8e959d", "#7d858f", "#3d4249", "#7b8794", "#968a7d", "#5a6e5a", "#b08d5a")

objects = []
goals = []
spawners = []
next_id = [1]


def new_id():
    i = next_id[0]
    next_id[0] += 1
    return i


def r3(v):
    return round(v, 3)


def box(name, x0, x1, y0, y1, z0, z1, tags, color):
    if x1 - x0 < 1e-3 or z1 - z0 < 1e-3:
        return
    objects.append({
        "id": new_id(), "name": name, "type": "box",
        "position": [r3((x0 + x1) / 2), r3(y0), r3((z0 + z1) / 2)], "rotationY": 0.0,
        "size": [r3(x1 - x0), r3(y1 - y0), r3(z1 - z0)], "tags": tags, "color": color,
    })


def slab(name, y, x0, x1, z0, z1, color=FLOOR_C, extra_tags=()):
    box(name, x0, x1, y - SLAB, y, z0, z1, ["walkable", *extra_tags], color)


def wall(name, y, x0, x1, z0, z1, height=WALL_H, color=WALL_C):
    box(name, x0, x1, y, y + height, z0, z1, ["obstacle"], color)


def wall_x(name, y, z, x0, x1, gaps=(), height=WALL_H, color=WALL_C):
    """A wall along x at z from x0 to x1, with openings (gx0, gx1)."""
    edges = [x0]
    for g0, g1 in sorted(gaps):
        edges += [g0, g1]
    edges.append(x1)
    for k in range(0, len(edges), 2):
        wall(name, y, edges[k], edges[k + 1], z - WALL / 2, z + WALL / 2, height, color)


def wall_z(name, y, x, z0, z1, gaps=(), height=WALL_H, color=WALL_C):
    edges = [z0]
    for g0, g1 in sorted(gaps):
        edges += [g0, g1]
    edges.append(z1)
    for k in range(0, len(edges), 2):
        wall(name, y, x - WALL / 2, x + WALL / 2, edges[k], edges[k + 1], height, color)


def stairs(name, cx, y, cz, width, rise, run, rotation_deg, tags, color, steps):
    objects.append({
        "id": new_id(), "name": name, "type": "stairs",
        "position": [r3(cx), r3(y), r3(cz)], "rotationY": rotation_deg,
        "size": [r3(width), r3(rise), r3(run)], "steps": steps, "tags": tags, "color": color,
    })


def goal(name, cx, cz, hx, hz, y):
    gid = new_id()
    goals.append({
        "id": gid, "name": name,
        "area": {"center": [r3(cx), r3(cz)], "halfExtents": [r3(hx), r3(hz)], "elevation": r3(y)},
        "capacity": 0, "serviceTime": 0.0, "queueDirection": [0.0, -1.0], "queueSpacing": 0.8,
    })
    return gid


# --- Floors --------------------------------------------------------------------------------
slab("G floor", 0.0, X0, X1, Z0, Z1)
for k in range(1, FLOORS):
    y = k * STOREY
    n = FLOOR_NAMES[k]
    slab(f"{n} floor (north)", y, X0, X1, VOID_Z, Z1)
    slab(f"{n} floor (south)", y, X0, X1, Z0, -VOID_Z)
    slab(f"{n} floor (west)", y, X0, -VOID_X, -VOID_Z, VOID_Z)
    slab(f"{n} floor (east)", y, VOID_X, X1, -VOID_Z, VOID_Z)

# Plazas outside the two entrances.
slab("South plaza", 0.0, -22.0, 22.0, -46.0, Z0, PLAZA_C)
slab("North plaza", 0.0, -22.0, 22.0, Z1, 44.0, PLAZA_C)

# --- Escalator cascades and their bridges ---------------------------------------------------
# Each cascade climbs one storey per escalator pair towards the middle; landing k sits on a
# bridge across the void at floor k. bridge_x[k] = (near edge, far edge) seen from the end.
escalator_count = 0
for side, sgn in (("west", -1.0), ("east", 1.0)):
    foot = sgn * CASCADE_START                # where the ground-floor escalators start
    for k in range(FLOORS - 1):
        start = foot - sgn * k * (ESC_RUN + BRIDGE)       # foot of the k -> k+1 pair
        top = start - sgn * ESC_RUN                       # head of the pair, at floor k+1
        mid = (start + top) / 2
        y = k * STOREY
        rot = 90.0 if sgn < 0 else -90.0                  # local +z (up-slope) towards the middle
        up_tags = ["walkable", "escalator", "escalator:speed=0.5"]
        # Two up escalators on the outside (one for each corridor) and two down in the middle.
        for zs, half in ((-1.0, "south"), (1.0, "north")):
            stairs(f"Escalator {FLOOR_NAMES[k]}-{FLOOR_NAMES[k + 1]} {side} {half} (up)", mid, y,
                   zs * ESC_Z[0], ESC_WIDTH, STOREY, ESC_RUN, rot, up_tags, STEEL_C, ESC_STEPS)
            stairs(f"Escalator {FLOOR_NAMES[k + 1]}-{FLOOR_NAMES[k]} {side} {half} (down)", mid, y,
                   zs * ESC_Z[1], ESC_WIDTH, STOREY, ESC_RUN, rot, up_tags + ["escalator:down"], STEEL_C,
                   ESC_STEPS)
            escalator_count += 2
        # Balustrades: a low skirt along both sides of every escalator. They stop short of
        # the landings: the navmesh keeps an agent radius clear of obstacles, and a skirt
        # running right to the edge would close the landing in front of the steps to
        # people crossing from one escalator to the next.
        inset = 0.6
        outer = ESC_Z[0] + ESC_WIDTH / 2 + 0.08
        between = (ESC_Z[0] + ESC_Z[1]) / 2
        for bz in (-outer, -between, 0.0, between, outer):
            objects.append({
                "id": new_id(), "name": f"Balustrade {FLOOR_NAMES[k]}-{FLOOR_NAMES[k + 1]} {side}",
                "type": "ramp", "position": [r3(mid), r3(y + 0.9 + STOREY * inset / ESC_RUN), bz], "rotationY": rot,
                "size": [0.14, r3(STOREY * (ESC_RUN - 2 * inset) / ESC_RUN), r3(ESC_RUN - 2 * inset)],
                "tags": ["obstacle"], "color": RAIL_C,
            })
        # Landing bridge at floor k+1 (the top one in the middle joins both cascades).
        yb = (k + 1) * STOREY
        if k + 1 < FLOORS - 1:
            far = top - sgn * BRIDGE
            bx0, bx1 = min(top, far), max(top, far)
        else:
            bx0, bx1 = (top, 0.0) if sgn < 0 else (0.0, top)
        slab(f"{FLOOR_NAMES[k + 1]} bridge {side}", yb, bx0, bx1, -VOID_Z, VOID_Z)
        # Railings on the bridge edges, open where escalators land or leave.
        opening = ESC_Z[0] + ESC_WIDTH / 2 + 0.6
        for ex in (bx0, bx1):
            if abs(ex) < 1e-6:
                continue  # the two halves of the top bridge meet here
            for z0, z1 in ((-VOID_Z, -opening), (opening, VOID_Z)):
                box(f"{FLOOR_NAMES[k + 1]} bridge railing", ex - 0.075, ex + 0.075, yb, yb + RAIL_H, z0, z1,
                    ["obstacle"], RAIL_C)

# Void railings along the corridor edges, open where bridges join.
bridge_spans = {k: [] for k in range(1, FLOORS)}
for o in objects:
    if " bridge " in o["name"] and o["type"] == "box" and "railing" not in o["name"]:
        k = FLOOR_NAMES.index(o["name"].split()[0])
        bridge_spans[k].append((o["position"][0] - o["size"][0] / 2, o["position"][0] + o["size"][0] / 2))
for k in range(1, FLOORS):
    y = k * STOREY
    gaps = sorted(bridge_spans[k])
    merged = []
    for g in gaps:
        if merged and g[0] <= merged[-1][1] + 1e-3:
            merged[-1] = (merged[-1][0], max(merged[-1][1], g[1]))
        else:
            merged.append(g)
    for zs in (-VOID_Z, VOID_Z):
        wall_x(f"{FLOOR_NAMES[k]} void railing", y, zs, -VOID_X, VOID_X, merged, RAIL_H, RAIL_C)
    for xs in (-VOID_X, VOID_X):
        wall_z(f"{FLOOR_NAMES[k]} void railing", y, xs, -VOID_Z, VOID_Z, (), RAIL_H, RAIL_C)

# --- Shops, anchors and outer walls --------------------------------------------------------
shop_goals = {k: [] for k in range(FLOORS)}
anchor_goals = {}
for k in range(FLOORS):
    y = k * STOREY
    fl = FLOOR_NAMES[k]
    kinds = iter(SHOP_KINDS[k])
    for side, sgn in (("north", 1.0), ("south", -1.0)):
        # Gaps in the shop row: the entrance passage (ground floor) and the passages to the
        # fire stairs behind the shops.
        gaps = [(-STAIR_X - PASSAGE / 2, -STAIR_X + PASSAGE / 2), (STAIR_X - PASSAGE / 2, STAIR_X + PASSAGE / 2)]
        if k == 0:
            gaps.append((-ENTRANCE / 2, ENTRANCE / 2))
        gaps.sort()
        segments = []
        edge = -ANCHOR_X
        for g0, g1 in gaps:
            segments.append((edge, g0))
            edge = g1
        segments.append((edge, ANCHOR_X))
        # Four shops per side, shared out over the segments by length.
        total = sum(b - a for a, b in segments)
        counts = [max(1, round(4 * (b - a) / total)) for a, b in segments]
        while sum(counts) > 4:
            counts[counts.index(max(counts))] -= 1
        while sum(counts) < 4:
            counts[counts.index(max(counts))] += 1
        front = sgn * SHOP_Z
        back = sgn * Z1
        zmin, zmax = min(front, back), max(front, back)
        for (a, b), c in zip(segments, counts):
            # Partitions at both segment ends (passage walls) and between units.
            for x in (a, b):
                if abs(abs(x) - ANCHOR_X) > 1e-6:
                    wall_z(f"{fl} {side} passage wall", y, x, zmin, zmax)
            width = (b - a) / c
            for u in range(c):
                ux0 = a + u * width
                ux1 = ux0 + width
                if u > 0:
                    wall_z(f"{fl} {side} shop partition", y, ux0, zmin, zmax)
                kind = next(kinds)
                name = f"{fl} {kind} ({side} {len(shop_goals[k]) + 1})"
                # Shop front with a wide opening in the middle.
                cx = (ux0 + ux1) / 2
                open_half = min(4.0, width / 2 - 1.5)
                wall_x(f"{name} front", y, front, ux0, ux1, [(cx - open_half, cx + open_half)])
                # Where shoppers browse: the back two-thirds of the unit.
                shop_goals[k].append(name)
                goal(name, cx, sgn * (SHOP_Z + 9.0), width / 2 - 2.0, 3.5, y)
                # A couple of display counters inside.
                for dx in (-0.25, 0.25):
                    box(f"{name} display", cx + dx * width - 1.0, cx + dx * width + 1.0, y, y + 1.0,
                        sgn * (SHOP_Z + 6.0) - 0.6, sgn * (SHOP_Z + 6.0) + 0.6, ["obstacle"], SHOP_C)

    # Anchor stores at both ends, opening onto the end corridors.
    for (name, sgn) in ((ANCHORS[k][0], -1.0), (ANCHORS[k][1], 1.0)):
        full = f"{fl} {name}"
        open_half = 14.0 if name == "Food court" else (4.0 if name == "Cinema" else 10.0)
        wall_z(f"{full} front", y, sgn * ANCHOR_X, Z0, Z1, [(-open_half, open_half)])
        cx = sgn * (ANCHOR_X + X1) / 2
        anchor_goals[(k, sgn)] = full
        goal(full, cx, 0.0, 3.5, 18.0, y)
        if name == "Food court":
            # Tables across the food court.
            for tx in (-62.0, -58.5):
                for tz in range(-24, 25, 4):
                    box("Food court table", tx - 0.6, tx + 0.6, y, y + 0.75, tz - 0.6, tz + 0.6, ["obstacle"], TABLE_C)
        elif name == "Cinema":
            wall_z("Cinema screens wall", y, 62.0, Z0, Z1, [(-3.0, 3.0)])
        else:
            for tz in range(-22, 23, 8):  # aisles of shelving
                box(f"{full} shelving", cx - 3.0, cx + 3.0, y, y + 1.8, tz - 0.5, tz + 0.5, ["obstacle"], SHOP_C)

    # Outer walls of this storey: entrances (ground floor), doors into the fire stairs (every
    # floor) and emergency exits straight outside from the anchor stores (ground floor).
    side_gaps = [(-STAIR_X - PASSAGE / 2, -STAIR_X + PASSAGE / 2), (STAIR_X - PASSAGE / 2, STAIR_X + PASSAGE / 2)]
    end_gaps = [(-PASSAGE / 2, PASSAGE / 2)]
    if k == 0:
        side_gaps.append((-ENTRANCE / 2, ENTRANCE / 2))
        end_gaps += [(z - EXIT_DOOR / 2, z + EXIT_DOOR / 2) for z in ANCHOR_EXIT_Z]
    wall_x(f"{fl} outer wall south", y, Z0 - WALL / 2, X0 - WALL, X1 + WALL, side_gaps)
    wall_x(f"{fl} outer wall north", y, Z1 + WALL / 2, X0 - WALL, X1 + WALL, side_gaps)
    wall_z(f"{fl} outer wall west", y, X0 - WALL / 2, Z0, Z1, end_gaps)
    wall_z(f"{fl} outer wall east", y, X1 + WALL / 2, Z0, Z1, end_gaps)

# --- Fire stairs and emergency exits -------------------------------------------------------
# Six enclosed dog-leg stair cores outside the building: behind the north and south shops
# (reached by passages between shops) and at both ends (opening into the anchor stores).
# On every floor a door in the outer wall (which stands on the landing's edge) leads onto it; a flight climbs
# away from the building to a half landing and the other flight comes back to the next
# floor's landing. At ground level a side door opens outside, onto an apron where the
# emergency exit is. The anchor stores also have two exit doors straight outside.
emergency_exits = []
# Emergency routes: everyday routing keeps out of them; evacuees (UseEmergencyRoutes) use them.
EMERGENCY = ["emergency"]


def apron_exit(name, x0, x1, z0, z1):
    slab(f"{name} apron", 0.0, x0, x1, z0, z1, PLAZA_C, EMERGENCY)
    emergency_exits.append(goal(f"Emergency exit ({name})", (x0 + x1) / 2, (z0 + z1) / 2,
                                (x1 - x0) / 2 - 0.5, (z1 - z0) / 2 - 0.5, 0.0))


def stair_core(name, px, pz, out, door_side):
    """A core on the building face at (px, pz), extending along `out` (an axis direction).
    `door_side` (+1/-1) is which side of it the ground-floor door to the outside is on."""
    ox, oz = out
    lx, lz = oz, ox  # lateral axis

    def rect(a0, a1, b0, b1):
        xs = [px + ox * b + lx * a for a in (a0, a1) for b in (b0, b1)]
        zs = [pz + oz * b + lz * a for a in (a0, a1) for b in (b0, b1)]
        return min(xs), max(xs), min(zs), max(zs)

    def centre(a, b):
        return px + ox * b + lx * a, pz + oz * b + lz * a

    hw = STAIR_HALF
    fw = hw - 0.15
    run = 5.0                       # flights from b = 2 to b = 7
    rot = math.degrees(math.atan2(ox, oz))
    top = (FLOORS - 1) * STOREY + WALL_H
    for k in range(FLOORS):
        y = k * STOREY
        x0, x1, z0, z1 = rect(-hw, hw, 0.0, 2.0)
        slab(f"{name} landing {FLOOR_NAMES[k]}", y, x0, x1, z0, z1, STAIR_C, EMERGENCY)
        if k + 1 < FLOORS:
            half = STOREY / 2
            cx, cz = centre(-0.15 - fw / 2, 2.0 + run / 2)
            stairs(f"{name} {FLOOR_NAMES[k]} flight 1", cx, y, cz, fw, half, run, rot, ["walkable"] + EMERGENCY, STAIR_C, 14)
            x0, x1, z0, z1 = rect(-hw, hw, 2.0 + run, 4.0 + run)
            slab(f"{name} half landing {FLOOR_NAMES[k]}", y + half, x0, x1, z0, z1, STAIR_C, EMERGENCY)
            cx, cz = centre(0.15 + fw / 2, 2.0 + run / 2)
            stairs(f"{name} {FLOOR_NAMES[k]} flight 2", cx, y + half, cz, fw, half, run, rot + 180.0,
                   ["walkable"] + EMERGENCY, STAIR_C, 14)
    x0, x1, z0, z1 = rect(-0.15, 0.15, 2.0, 2.0 + run)
    box(f"{name} core wall", x0, x1, 0.0, (FLOORS - 1) * STOREY + 1.0, z0, z1, ["obstacle"], WALL_C)
    back = 4.0 + run
    x0, x1, z0, z1 = rect(-hw - WALL, hw + WALL, back, back + WALL)
    box(f"{name} wall", x0, x1, 0.0, top, z0, z1, ["obstacle"], WALL_C)
    for sd in (-1.0, 1.0):
        a0, a1 = (hw, hw + WALL) if sd > 0 else (-hw - WALL, -hw)
        if sd == door_side:
            # Door to the outside at ground level, beside the landing.
            for b0, b1, y0 in ((0.0, 0.2, 0.0), (0.2, 1.8, 2.2), (1.8, back, 0.0)):
                x0, x1, z0, z1 = rect(a0, a1, b0, b1)
                box(f"{name} wall", x0, x1, y0, top, z0, z1, ["obstacle"], WALL_C)
        else:
            x0, x1, z0, z1 = rect(a0, a1, 0.0, back)
            box(f"{name} wall", x0, x1, 0.0, top, z0, z1, ["obstacle"], WALL_C)
    # The apron runs under the side wall, so the doorway has floor.
    a0 = hw if door_side > 0 else -hw - WALL - 6.0
    x0, x1, z0, z1 = rect(a0, a0 + 6.0 + WALL, 0.0, 4.0)
    apron_exit(name, x0, x1, z0, z1)


for sx in (-STAIR_X, STAIR_X):
    away = 1.0 if sx > 0 else -1.0
    ew = "east" if sx > 0 else "west"
    # North cores: lateral +x, so `away` puts the door on the side away from the middle.
    # South cores: lateral -x.
    stair_core(f"Fire stairs north-{ew}", sx, Z1, (0.0, 1.0), away)
    stair_core(f"Fire stairs south-{ew}", sx, Z0, (0.0, -1.0), -away)
stair_core("Fire stairs west", X0, 0.0, (-1.0, 0.0), 1.0)
stair_core("Fire stairs east", X1, 0.0, (1.0, 0.0), -1.0)

# Exit doors straight outside from the ground-floor anchor stores.
for sgn, ew in ((-1.0, "west"), (1.0, "east")):
    for z in ANCHOR_EXIT_Z:
        ns = "north" if z > 0 else "south"
        xa = sgn * X1  # from the inside of the wall, so the doorway has floor
        apron_exit(f"{ew} {ns} door", min(xa, xa + sgn * 5.3), max(xa, xa + sgn * 5.3), z - 3.0, z + 3.0)

# --- Entrances, exits and arrivals ---------------------------------------------------------
exits = {
    "south": goal("South entrance", 0.0, -34.0, 6.0, 2.0, 0.0),
    "north": goal("North entrance", 0.0, 34.0, 6.0, 2.0, 0.0),
}
ARRIVALS = {"south": 1.2, "north": 0.8}  # people per second; the south side faces the street
MIX = [
    {"type": "mall_shopper", "weight": 0.72},
    {"type": "mall_diner", "weight": 0.18},
    {"type": "mall_moviegoer", "weight": 0.10},
]
for side, sgn in (("south", -1.0), ("north", 1.0)):
    spawners.append({
        "id": new_id(), "name": f"Arrivals ({side})",
        "area": {"center": [0.0, sgn * 41.0], "halfExtents": [16.0, 2.0], "elevation": 0.0},
        "rate": ARRIVALS[side], "total": 0, "startTime": 0.0, "stopTime": None, "goal": exits[side],
        "groupSize": [1, 1],
        "agent": {"agentType": 0, "color": "#4aa3df", "colorJitter": 0.1, "maxAcceleration": 3.0,
                  "maxSpeedFactor": 1.35, "preferredSpeed": [1.0, 1.4], "radius": [0.22, 0.27]},
        "agentMix": MIX,
    })


# --- Behaviour trees -----------------------------------------------------------------------
def node(type_, id_, params=None, children=None):
    n = {"type": type_, "id": id_}
    if params:
        n["params"] = params
    if children:
        n["children"] = children
    return n


def browse(prefix, seconds, jitter):
    return [
        node("SetState", f"{prefix}_state", {"name": "browsing"}),
        node("Wander", f"{prefix}_look1", {"radius": 4}),
        node("Wait", f"{prefix}_wait1", {"seconds": seconds, "jitter": jitter}),
        node("Wander", f"{prefix}_look2", {"radius": 4}),
        node("Wait", f"{prefix}_wait2", {"seconds": seconds / 2, "jitter": jitter / 2}),
    ]


def leave():
    return [
        node("SetState", "leaving_state", {"name": "leaving"}),
        node("MoveToGoal", "exit"),
        node("Despawn", "gone"),
    ]


def floor_choice(id_, weights):
    """Pick a floor by weight, then any shop or anchor on it."""
    per_floor = []
    for k in range(FLOORS):
        places = shop_goals[k] + [anchor_goals[(k, -1.0)], anchor_goals[(k, 1.0)]]
        per_floor.append(node("RandomSelector", f"{id_}_{FLOOR_NAMES[k]}", None,
                              [node("MoveToGoal", f"{id_}_{FLOOR_NAMES[k]}_{i}", {"goal": g})
                               for i, g in enumerate(places)]))
    return node("WeightedRandom", id_, {"weights": ",".join(str(w) for w in weights)}, per_floor)


def tree(name, root):
    return {"format": "crowdsim.bt", "version": 1, "name": name, "root": root}


trees = {
    # Visits three shops, more often on the lower floors, browsing in each.
    "mall_shopper": tree("mall_shopper", node("Sequence", "root", None, [
        node("SetState", "start", {"name": "shopping"}),
        node("Repeat", "visits", {"count": 3}, [
            node("Sequence", "visit", None, [
                node("SetState", "walk_state", {"name": "shopping"}),
                floor_choice("where", [4, 3, 2, 1]),
                *browse("shop", 30, 60),
            ]),
        ]),
        *leave(),
    ])),
    # Goes up to the food court for a meal, sometimes looks into one more shop, then leaves.
    "mall_diner": tree("mall_diner", node("Sequence", "root", None, [
        node("SetState", "start", {"name": "dining"}),
        node("MoveToGoal", "food", {"goal": anchor_goals[(3, -1.0)]}),
        node("Wander", "find_table", {"radius": 6}),
        node("Wait", "eat", {"seconds": 420, "jitter": 600}),
        node("Succeeder", "maybe", None, [
            node("Sequence", "after", None, [
                node("RandomChance", "half", {"probability": 0.5}),
                floor_choice("then", [1, 1, 1, 1]),
                *browse("after", 20, 40),
            ]),
        ]),
        *leave(),
    ])),
    # Straight to the cinema, a film, then out.
    "mall_moviegoer": tree("mall_moviegoer", node("Sequence", "root", None, [
        node("SetState", "start", {"name": "to the cinema"}),
        node("MoveToGoal", "cinema", {"goal": anchor_goals[(3, 1.0)]}),
        node("SetState", "film", {"name": "watching"}),
        node("Wander", "seat", {"radius": 5}),
        node("Wait", "watch", {"seconds": 900, "jitter": 900}),
        *leave(),
    ])),
}

agent_types = {
    "mall_shopper": {"radius": [0.22, 0.27], "preferredSpeed": [1.0, 1.4], "color": "#4aa3df"},
    "mall_diner": {"radius": [0.22, 0.27], "preferredSpeed": [1.0, 1.35], "color": "#e8b04a"},
    "mall_moviegoer": {"radius": [0.22, 0.27], "preferredSpeed": [1.1, 1.45], "color": "#c46ad6"},
}
agent_json = []
for name, p in agent_types.items():
    agent_json.append({
        "format": "crowdsim.agent", "version": 1, "name": name, "behaviour": name,
        "radius": p["radius"], "preferredSpeed": p["preferredSpeed"], "maxSpeedFactor": 1.35,
        "maxAcceleration": 3, "personalSpace": 0.1, "height": 1.75, "color": p["color"],
        "colorJitter": 0.1, "tags": [],
    })

for name, t in trees.items():
    (CONTENT / "behaviours" / f"{name}.bt.json").write_text(json.dumps(t, indent=2) + "\n", encoding="utf-8")
for a in agent_json:
    (CONTENT / "agents" / f"{a['name']}.agent.json").write_text(json.dumps(a, indent=2) + "\n", encoding="utf-8")

scene = {
    "format": "crowdsim.scene", "version": 1, "name": "shopping_mall",
    # Embedded so the scene also loads without content/.
    "behaviours": list(trees.values()),
    "agentTypes": agent_json,
    "navigation": {"agentHeight": 1.9, "agentMaxClimb": 0.45, "agentMaxSlope": 45.0, "agentRadius": 0.3,
                   "cellHeight": 0.1, "cellSize": 0.2, "tileSize": 64},
    "nextId": next_id[0],
    "objects": objects, "goals": goals, "spawners": spawners, "links": [],
}
out = CONTENT / "scenes" / "shopping_mall.scene.json"
out.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
print(f"wrote {out}: {len(objects)} objects, {len(goals)} goals, {escalator_count} escalators")
