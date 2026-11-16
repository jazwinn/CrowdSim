# Lua scripting

CrowdSim embeds **LuaJIT** (through sol2) to add behaviour-tree nodes, entity types and scenario
logic without recompiling. Scripts are plain text in the content directory, reload when saved,
and act on the simulation only through the same **commands** as the editor, MCP and replay.

- `content/scripts/*.lua` load at start-up, before behaviour trees (so trees can use the nodes
  they define), and **hot-reload** when saved: a script re-runs top to bottom and replaces what
  its previous version registered (nodes, entity types and event handlers, matched by name).
- `content/scenarios/*.lua` are scenario scripts. Run one with `--script scenarios/<file>.lua`
  (GUI or headless), or from the console with `scene.command("LoadScript", {path = ...})`.
- The editor's **Script** panel lists the loaded scripts (a red dot marks one that failed;
  hover for the error). Click one to open it in a tab: **Save** (Ctrl+S) writes the file and runs
  it, **Run without saving** tries the text without touching the file, **Revert** drops edits,
  **Reload from file** re-reads it. **New script** creates `content/scripts/<name>.lua`; **Load a
  file** runs any other script by path. The **Console** tab evaluates snippets (Ctrl+Enter).
  Errors appear there, in the Log panel and through the `ScriptErrors` command; they never stop
  the simulation.

## Sandbox and limits

One Lua state per process, owned by the script module (no globals in C++). Available: Lua 5.1
base functions, `string`, `table`, `math`, `bit`. **Not available:** `os`, `io`, `debug`, `ffi`,
`jit`, `package`/`require`, `dofile`, `loadfile`, `string.dump`, `getfenv`/`setfenv`, and binary
(bytecode) chunks: `load` accepts text only and returns `nil, message` on failure.

Each call into Lua (a script load, console snippet, node tick, entity callback or event) gets a
budget: **20 million instructions** and **0.5 s**. Allocations are capped at **256 MB** for the
whole state; a single oversized allocation fails with `not enough memory`. The JIT compiler is
off so the instruction limit covers all code. Exceeding a budget raises a Lua error.

**Determinism:** `math.random` / `sim.random()` come from a world-seeded generator, and nodes
get per-agent random streams (`ctx:random(a)`). Node ticks run serially in agent order. Do not
let `pairs()` order drive simulation decisions (iterate arrays with `ipairs`, or key tables by
id), and do not read wall-clock time.

## API

### `scene` — build and query the scene (through commands)

| Function | Description |
|---|---|
| `scene.command(name, params)` | Run any command (see `docs/formats.md`); returns its result table. |
| `scene.place(object)` | `PlaceObject`; returns the id. `object` uses the scene-file object fields. |
| `scene.create_goal(goal)` / `scene.create_spawner(spawner)` | Returns the id. |
| `scene.update(id, patch)` / `scene.delete(id)` | `UpdateEntity` / `DeleteEntity`. |
| `scene.get(id)` | Entity as a table (scene-file fields; scripted objects also get `type`). |
| `scene.find(name)` | Id of the first object, goal or spawner with that name, or `nil`. |
| `scene.objects()` | All static objects as tables. |

Commands issued from Lua are applied immediately but **not logged** individually: the command
that ran the script (`LoadScript`, `EvalLua`, `SpawnEntity`) is logged with its source, and
replaying it regenerates them.

### `entities` — scripted object types

```lua
entities.register_type{
  name = "info_kiosk",                 -- required, no ':'
  type = "box", size = {1.2, 2.2, 1.2}, color = "#3a8fd8",   -- or object = {...} (PlaceObject fields)
  tags = {"attraction"},               -- added to "obstacle" and "type:info_kiosk"
  tick_hz = 1,                         -- on_tick rate (0.1 .. 30 Hz)
  on_spawn = function(entity) end,     -- after SpawnEntity placed it
  on_tick = function(entity, dt) end,  -- for every instance, after sim steps
}
entities.spawn("info_kiosk", {position = {x, y, z}, rotationY = 0, name = "Kiosk 1"})  --> id
entities.list("info_kiosk")  --> array of entity tables (all objects if no type is given)
```

Scripted types appear in the editor under *Add… → Scripted (Lua)*. Instances are ordinary
objects (saved in scenes, undoable); `type:<name>` tags link them to their type.

### `bt` — behaviour-tree nodes

```lua
bt.register_node{
  name = "VisitAttraction",            -- must not clash with a built-in node
  kind = "action",                     -- "action" or "condition" (editor category)
  description = "...",
  params = {                           -- map (sorted by name) or array of {name = ..., ...}
    radius = {type = "number", default = 30, description = "search radius (m)"},
    tag    = {type = "string", default = "attraction"},
    -- type: "number" | "string" | "vec3" (default {x, y, z})
  },
  tick = function(ctx, agents)
    local results = {}
    for k, a in ipairs(agents) do       -- every agent that reached this node instance this step
      results[k] = "running"            -- "success" | "failure" | "running" | true | false
    end
    return results                      -- or a single status for all agents
  end,
}
bt.set{name = "tourist", root = {type = "Sequence", children = {...}}}   -- SetBehaviorTree
bt.node_types()                                                           -- built-in + Lua nodes
```

**How script nodes run.** Lua never runs inside the parallel decision tick. An agent reaching a
script node is queued and the node reports *running*. After the tick, `tick` is called once per
node instance with all queued agents (batched to amortise the call), serially and in agent
order. The returned status is what the node reports on that agent's **next** decision tick (100 ms
later at the default 10 Hz). Script nodes are leaves; parameters are validated against
`params` when a tree is compiled, and missing ones take their defaults.

`ctx` (valid during the call; `a` is an agent handle from `agents`):

| Read | Act |
|---|---|
| `ctx.params` — this node's parameters | `ctx:move_to(a, x, y, z)` — walk there along a navmesh path |
| `ctx:position(a)` → `x, y, z` | `ctx:move_to_goal(a)` — follow the flow field to its goal |
| `ctx:velocity(a)` → `vx, vz`; `ctx:speed(a)` | `ctx:stop(a)` |
| `ctx:id(a)` — stable agent id | `ctx:set_speed(a, multiplier)` |
| `ctx:state(a)`, `ctx:type(a)` | `ctx:set_state(a, "name")` |
| `ctx:time()` — simulated seconds | |
| `ctx:random(a)` — agent's own stream, [0, 1) | |
| `ctx:count_near(a, radius)` | |

Keep per-agent state in a Lua table keyed by `ctx:id(a)`, and clear it on completion and on
`agent_despawned`.

### `agents`

| Function | Description |
|---|---|
| `agents.register_type{name, behaviour, radius, preferredSpeed, color, ...}` | `DefineAgentType` (fields as in `*.agent.json`). |
| `agents.count()` | Agents alive. |
| `agents.count_near(x, y, z, radius)` | Agents within `radius` on that floor. |
| `agents.region(x0, z0, x1, z1 [, y0, y1])` | Crowd in a box on the ground (only those standing between heights `y0` and `y1` when given, e.g. one storey): `{count, density, peak_pressure, mean_pressure, fallen}` (pressure in N/m with scene bodies). |
| `agents.get(id)` | `{id, type, behaviour, state, goal, position, speed}` or `nil`. |

### `sim`, `metrics`, `events`, `log`

| Function | Description |
|---|---|
| `sim.time()`, `sim.step_index()`, `sim.seed()` | Clock and seed. |
| `sim.random()` | World-seeded random number in [0, 1). |
| `sim.play()`, `sim.pause()`, `sim.step(n)`, `sim.command(name, params)` | Playback through commands. |
| `metrics.get()` | `{alive, spawned, arrived, despawned, average_speed, fallen, falls, peak_pressure, time}`. |
| `sim.command("GetAdMetrics")` | Ad effectiveness per ad (see `docs/formats.md`, Ads); `scripts/ad_report.lua` logs a ranking every 60 s. |
| `events.on(name, fn)` | `agent_spawned(id)`, `agent_arrived(id, goal)`, `agent_despawned(id)`, `step(index, time)`. Handlers belong to the registering script and are replaced when it reloads. |
| `events.clear([name])` | Remove handlers. |
| `print(...)`, `log.info/warn/error(msg)` | To the Log panel (and the console output). |

## Commands

| Command | Logged | Description |
|---|---|---|
| `LoadScript {name, source \| path}` | yes (with source) | Run / hot-reload a script. |
| `ReloadScript {name}` | yes (as `LoadScript`) | Re-run `content/scripts/<name>.lua`. |
| `SaveScript {name, source}` | yes (as `LoadScript`) | Write `content/scripts/<name>.lua` and run it (nothing is written while replaying). |
| `EvalLua {source}` | yes | Evaluate a snippet; returns `{output, value}`. |
| `SpawnEntity {type, position, rotationY?, name?}` | yes, undoable | Place a scripted entity. |
| `ListScripts`, `GetScript {name}`, `ScriptErrors`, `ListEntityTypes` | no | Queries. |

Command logs start with a `LoadScript` for every loaded script, then the scene snapshot, so a log
replays in a fresh process even without the script files.

## Examples

- `content/scripts/info_kiosk.lua`: the `info_kiosk` entity type (tracks browsing time) and the
  `VisitAttraction` node; `content/behaviours/tourist.bt.json` and
  `content/agents/tourist.agent.json` use them.
- `content/scenarios/kiosk_square.lua`: a scripted scenario (square, planter, three kiosks,
  tourists and commuters, an arrival counter).
