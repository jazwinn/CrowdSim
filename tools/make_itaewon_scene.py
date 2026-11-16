"""Generate the Itaewon 2022 crowd-crush study scenes in content/scenes/.

On 29 October 2022 a crowd crush killed 159 people in a sloped alley beside the
Hamilton Hotel in Itaewon, Seoul. This is a simplified model of the place built for
safety analysis: what made the alley dangerous, and what would have changed that. It
is not a forensic reconstruction. Where sources disagree, the values below are
middle values, noted with their range.

The place (x = east, z = north, metres; Itaewon-ro is at the bottom of the slope):
  - Itaewon-ro, the main road, z 0..14. Opened to pedestrians from 18:30 that night.
    Station Exit 1 is on its north side, just east of the alley.
  - The alley: 4 m wide, 45 m long (sources: 40-50 m), rising 10% (5.7 degrees;
    sources: 5.7-11 degrees) from Itaewon-ro up to World Food Street.
  - The Hamilton Hotel's temporary iron wall narrowed it to 3.2 m over about 6 m,
    10-16 m up the alley. The crush happened there: over 300 people trapped in about
    5.7 m x 3.2 m (18.24 m2), peak density about 10.7 people/m2 at 22:25.
  - World Food Street (Itaewon-ro 27ga-gil), the bar street above, 6 m wide, at the
    top of the alley (a T-junction).
  - Other routes between the two streets are much further away; one stands in for
    them at the east edge.

Flows: people from the station and the main road go up to the bars; people from
World Food Street go down to the station. The two flows meet head-on in the alley.
A minority push hard (witnesses describe a group pushing from the top).

Variants (one file each, same crowd):
  itaewon_2022            as it was
  itaewon_2022_one_way    alley up only; people going down are sent round the east
                          (Seoul's one-way rule at every Halloween since)
  itaewon_2022_metered    marshals at both ends let about 1.7 people/s through each way
  itaewon_2022_no_wall    the hotel's wall removed: 4 m all the way

Sources: Wikipedia "Seoul Halloween crowd crush"; Korea Times on the police
investigation (13 Jan 2023); Liang et al., PLOS ONE 19(7) e0306764 (2024);
Hwang & Kim, Computer Animation and Virtual Worlds (2025).

Run from the repository root:  python tools/make_itaewon_scene.py
"""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"

X0, X1 = -50.0, 50.0            # extent of both streets
ROAD_Z0, ROAD_Z1 = 0.0, 14.0     # Itaewon-ro (pedestrian space incl. the roadway)
ALLEY_X0, ALLEY_X1 = -2.0, 2.0   # 4 m
ALLEY_Z0, ALLEY_Z1 = ROAD_Z1, ROAD_Z1 + 45.0
RISE = 4.5                       # 10% over 45 m
TOP_Z0, TOP_Z1 = ALLEY_Z1, ALLEY_Z1 + 6.0  # World Food Street
WALL_Z0, WALL_Z1 = ALLEY_Z0 + 10.0, ALLEY_Z0 + 16.0  # the hotel's iron wall
WALL_X0 = ALLEY_X1 - 0.8         # leaves 3.2 m
EAST_X0, EAST_X1 = 44.0, 50.0    # the other routes, at the east edge
EXIT1_X = 20.0                   # Itaewon Station Exit 1
H = 7.0                          # building height (kept low so the alley can be seen from above)
# Arrivals per second are multiplied by this. About 100,000 people were in the district;
# what matters here is that more people reached both ends of the alley than it could pass.
DEMAND = 2.5

ROAD, STREET, BUILDING, HOTEL, IRON = "#4d535b", "#5c626a", "#7b736a", "#8d8f95", "#3a3f46"


def build(variant):
    objects, goals, spawners = [], [], []
    next_id = [1]

    def new_id():
        i = next_id[0]
        next_id[0] += 1
        return i

    def box(name, x0, x1, y0, y1, z0, z1, tags, color):
        objects.append({
            "id": new_id(), "name": name, "type": "box",
            "position": [round((x0 + x1) / 2, 3), y0, round((z0 + z1) / 2, 3)], "rotationY": 0.0,
            "size": [round(x1 - x0, 3), round(y1 - y0, 3), round(z1 - z0, 3)],
            "tags": tags, "color": color,
        })

    def ramp(name, x0, x1, z0, z1, tags, color):
        objects.append({
            "id": new_id(), "name": name, "type": "ramp",
            "position": [round((x0 + x1) / 2, 3), 0.0, round((z0 + z1) / 2, 3)], "rotationY": 0.0,
            "size": [round(x1 - x0, 3), RISE, round(z1 - z0, 3)], "tags": tags, "color": color,
        })

    # Walking surfaces.
    box("Itaewon-ro", X0, X1, -0.2, 0.0, ROAD_Z0, ROAD_Z1, ["walkable"], ROAD)
    ramp("Alley beside the Hamilton Hotel (10% slope)", ALLEY_X0, ALLEY_X1, ALLEY_Z0, ALLEY_Z1,
         ["walkable", "area:alley"], STREET)
    ramp("Other routes (east)", EAST_X0, EAST_X1, ALLEY_Z0, ALLEY_Z1, ["walkable"], STREET)
    box("World Food Street", X0, X1, RISE - 0.2, RISE, TOP_Z0, TOP_Z1, ["walkable"], STREET)

    # Buildings between the streets, and the street edges.
    box("Buildings west of the alley", X0 - 0.5, ALLEY_X0, 0.0, H, ALLEY_Z0, ALLEY_Z1, ["obstacle"], BUILDING)
    box("Hamilton Hotel", ALLEY_X1, EAST_X0, 0.0, H, ALLEY_Z0, ALLEY_Z1, ["obstacle"], HOTEL)
    box("Buildings south of Itaewon-ro", X0 - 0.5, X1 + 0.5, 0.0, H, ROAD_Z0 - 1.0, ROAD_Z0, ["obstacle"],
        BUILDING)
    box("Buildings north of World Food Street", X0 - 0.5, X1 + 0.5, RISE, RISE + H, TOP_Z1, TOP_Z1 + 1.0,
        ["obstacle"], BUILDING)
    for side, x0, x1 in (("west", X0 - 1.0, X0), ("east", X1, X1 + 1.0)):
        box(f"Itaewon-ro ({side} edge)", x0, x1, 0.0, 3.0, ROAD_Z0, ROAD_Z1, ["obstacle"], BUILDING)
        box(f"World Food Street ({side} edge)", x0, x1, RISE, RISE + 3.0, TOP_Z0, TOP_Z1, ["obstacle"], BUILDING)
    if variant != "no_wall":
        # Tall enough to stand on the sloped floor along its whole length.
        box("Hotel's temporary iron wall (alley 3.2 m)", WALL_X0, ALLEY_X1, 0.0, 3.0 + RISE * WALL_Z1 / 45.0,
            WALL_Z0, WALL_Z1, ["obstacle"], IRON)

    def area(cx, cz, hx, hz, elevation):
        return {"center": [cx, cz], "halfExtents": [hx, hz], "elevation": elevation}

    def goal(name, a, capacity=0, service=0.0, direction=(0.0, -1.0)):
        gid = new_id()
        goals.append({"id": gid, "name": name, "area": a, "capacity": capacity, "serviceTime": service,
                      "queueDirection": list(direction), "queueSpacing": 0.6})
        return gid

    exit1 = goal("Station Exit 1", area(EXIT1_X, ROAD_Z1 - 1.5, 1.5, 1.0, 0.0))
    bars_west = goal("Bars west (World Food Street)", area(-35.0, (TOP_Z0 + TOP_Z1) / 2, 8.0, 2.5, RISE))
    bars_east = goal("Bars east (World Food Street)", area(25.0, (TOP_Z0 + TOP_Z1) / 2, 8.0, 2.5, RISE))
    road_west = goal("Itaewon-ro west", area(X0 + 2.0, 7.0, 1.5, 6.0, 0.0))
    road_east = goal("Itaewon-ro east", area(X1 - 2.0, 7.0, 1.5, 6.0, 0.0))
    top_west = goal("World Food Street west", area(X0 + 2.0, (TOP_Z0 + TOP_Z1) / 2, 1.5, 2.5, RISE))
    top_east = goal("World Food Street east", area(X1 - 2.0, (TOP_Z0 + TOP_Z1) / 2, 1.5, 2.5, RISE))
    if variant == "metered":
        # Marshals: two people at a time, 1.2 s each, about 1.7 people/s per direction.
        goal("Marshal (alley foot)", area(0.0, ALLEY_Z0 - 1.0, 1.5, 0.8, 0.0), 2, 1.2, (0.0, -1.0))
        goal("Marshal (alley top)", area(0.0, TOP_Z0 + 1.0, 1.5, 0.8, RISE), 2, 1.2, (-1.0, 0.0))

    up_type = "reveller_metered_up" if variant == "metered" else "reveller"
    down_type = "reveller_metered_down" if variant == "metered" else "reveller"
    # In the one-way variant people heading down are sent round the east.
    down_goal = top_east if variant == "one_way" else exit1

    def spawner(name, a, rate, goal_id, mix, start=0.0, stop=600.0):
        spawners.append({
            "id": new_id(), "name": name, "area": a, "rate": round(rate * DEMAND, 3), "total": 0,
            "startTime": start, "stopTime": stop, "goal": goal_id, "groupSize": [1, 1],
            "agent": {"agentType": 0, "color": "#d98c40", "colorJitter": 0.1, "maxAcceleration": 3.0,
                      "maxSpeedFactor": 1.35, "preferredSpeed": [1.0, 1.4], "radius": [0.2, 0.25]},
            "agentMix": mix,
        })

    def mix(kind, pushing=0.0):
        m = [{"type": kind, "weight": round(1.0 - pushing, 3)}]
        if pushing > 0.0:
            m.append({"type": "reveller_pushing", "weight": pushing})
        return m

    exit_area = area(EXIT1_X, 3.0, 2.0, 1.5, 0.0)
    road_w = area(X0 + 3.0, 7.0, 1.5, 5.5, 0.0)
    road_e = area(X1 - 3.0, 7.0, 1.5, 5.5, 0.0)
    top_w = area(X0 + 3.0, (TOP_Z0 + TOP_Z1) / 2, 1.5, 2.2, RISE)
    top_e = area(X1 - 3.0, (TOP_Z0 + TOP_Z1) / 2, 1.5, 2.2, RISE)
    bars_mid = area(-10.0, (TOP_Z0 + TOP_Z1) / 2, 6.0, 2.2, RISE)
    bars_mid_e = area(10.0, (TOP_Z0 + TOP_Z1) / 2, 6.0, 2.2, RISE)

    # Up the alley: arrivals by subway and along the main road, heading for the bars.
    # The crowd builds over the first two minutes, then stays heavy.
    spawner("Exit 1 arrivals to bars west", exit_area, 1.0, bars_west, mix(up_type), 0.0, 120.0)
    spawner("Exit 1 arrivals to bars east", exit_area, 0.6, bars_east, mix(up_type), 0.0, 120.0)
    spawner("Exit 1 crowd to bars west", exit_area, 1.6, bars_west, mix(up_type), 120.0)
    spawner("Exit 1 crowd to bars east", exit_area, 0.9, bars_east, mix(up_type), 120.0)
    spawner("Itaewon-ro west to the bars", road_w, 0.8, bars_west, mix(up_type), 60.0)
    # Down the alley: from World Food Street to the station, with a pushing minority.
    spawner("World Food Street west, leaving", top_w, 1.2, down_goal, mix(down_type, 0.12), 30.0)
    spawner("Bars west of the alley, leaving", bars_mid, 1.3, down_goal, mix(down_type, 0.12), 90.0)
    spawner("Bars east of the alley, leaving", bars_mid_e, 0.9, down_goal, mix(down_type, 0.12), 90.0)
    # Through traffic on each street (does not use the alley).
    spawner("Itaewon-ro eastbound", road_w, 1.2, road_east, mix("reveller"))
    spawner("Itaewon-ro westbound", road_e, 1.2, road_west, mix("reveller"))
    spawner("World Food Street eastbound", top_w, 0.6, top_east, mix("reveller"))
    spawner("World Food Street westbound", top_e, 0.6, top_west, mix("reveller"))

    types = ["reveller", "reveller_pushing"]
    if variant == "metered":
        types += ["reveller_metered_up", "reveller_metered_down"]
    agent_types = [json.loads((CONTENT / "agents" / f"{t}.agent.json").read_text(encoding="utf-8")) for t in types]
    behaviours = [json.loads((CONTENT / "behaviours" / f"{t['behaviour']}.bt.json").read_text(encoding="utf-8"))
                  for t in agent_types]

    name = "itaewon_2022" if variant == "as_it_was" else f"itaewon_2022_{variant}"
    return name, {
        "format": "crowdsim.scene", "version": 1, "name": name,
        "behaviours": behaviours, "agentTypes": agent_types,
        "navigation": {"agentHeight": 1.8, "agentMaxClimb": 0.3, "agentMaxSlope": 30.0, "agentRadius": 0.22,
                       "cellHeight": 0.1, "cellSize": 0.15, "tileSize": 64},
        # Bodies touch, press and fall in the packed alley (BodyParams defaults).
        "bodies": {"enabled": True},
        "nextId": next_id[0],
        "objects": objects, "goals": goals, "spawners": spawners, "links": [],
    }


for variant in ("as_it_was", "one_way", "metered", "no_wall"):
    name, scene = build(variant)
    out = CONTENT / "scenes" / f"{name}.scene.json"
    out.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out.relative_to(ROOT)}")
