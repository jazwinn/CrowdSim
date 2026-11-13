-- Escalator etiquette trial: compares "keep left, walk right" (hall A) with
-- "stand on both sides" (hall B) in scenes/escalator_etiquette.scene.json.
-- Does nothing in other scenes. Adds a counting gate at the top of each
-- escalator and prints a comparison to the Log every 30 s of simulated time.
--
-- From the Script console:  return escalator_report()

local HALLS = {
  { key = "A", label = "keep left, walk right", x = -7.0 },
  { key = "B", label = "stand on both sides", x = 7.0 },
}
local RISE, TOP_Z, FOOT_Z = 7.5, 7.5, -9.0

local stats, spawned_at, type_of, last_report, gates_added = {}, {}, {}, 0, false

local function reset()
  stats, spawned_at, type_of, last_report = {}, {}, {}, 0
  for _, h in ipairs(HALLS) do
    stats[h.key] = { arrived = 0, journey = 0, by_type = {}, window = 0 }
  end
end
reset()

local function active()
  return scene.find("A exit (upper)") ~= nil and scene.find("B exit (upper)") ~= nil
end

local function add_gates()
  for _, h in ipairs(HALLS) do
    -- Ignore "already exists" after a reload: gates live in the metrics collector.
    pcall(scene.command, "AddGate", {
      name = h.key .. " escalator top",
      a = { h.x - 1.5, TOP_Z }, b = { h.x + 1.5, TOP_Z }, elevation = RISE,
    })
  end
  gates_added = true
end

function escalator_report()
  local lines = { string.format("Escalator trial at t = %.0f s", sim.time()) }
  for _, h in ipairs(HALLS) do
    local s = stats[h.key]
    local mean = s.arrived > 0 and s.journey / s.arrived or 0
    local waiting = agents.count_near(h.x, 0, FOOT_Z, 4.0)
    local parts = {}
    for name, t in pairs(s.by_type) do
      parts[#parts + 1] = string.format("%s %.1f s (%d)", name, t.total / t.n, t.n)
    end
    table.sort(parts)
    lines[#lines + 1] = string.format(
      "  Hall %s (%s): %d up (%d per min lately), mean journey %.1f s, %d near the foot%s",
      h.key, h.label, s.arrived, (s.arrived - s.window) * 2, mean, waiting,
      #parts > 0 and ("; " .. table.concat(parts, ", ")) or "")
    s.window = s.arrived
  end
  local text = table.concat(lines, "\n")
  log.info(text)
  return text
end

events.on("agent_spawned", function(id)
  if active() then
    spawned_at[id] = sim.time()
    local a = agents.get(id)
    type_of[id] = a and a.type or "?"
  end
end)

events.on("agent_arrived", function(id, goal)
  local t0 = spawned_at[id]
  if t0 == nil then
    return
  end
  spawned_at[id] = nil
  local type_name = type_of[id] or "?"
  type_of[id] = nil
  local key = goal:sub(1, 1)
  local s = stats[key]
  if s == nil then
    return
  end
  local journey = sim.time() - t0
  s.arrived = s.arrived + 1
  s.journey = s.journey + journey
  local bt = s.by_type[type_name] or { n = 0, total = 0 }
  bt.n, bt.total = bt.n + 1, bt.total + journey
  s.by_type[type_name] = bt
end)

events.on("step", function(_, time)
  if not active() then
    gates_added = false
    return
  end
  if time < last_report then
    reset()  -- the simulation was reset
  end
  if not gates_added then
    add_gates()
  end
  if time - last_report >= 30 then
    last_report = time
    escalator_report()
  end
end)
