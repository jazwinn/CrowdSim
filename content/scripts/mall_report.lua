-- Shopping mall study: reports where people are in scenes/shopping_mall.scene.json (made by
-- tools/make_mall_scene.py). Does nothing in other scenes. Every 60 s of simulated time it
-- logs how many people are on each floor and riding between floors, how many are at the
-- foot of the ground-floor escalators, and the most of each so far.
--
-- From the Script console:  return mall_report()

local STOREY = 5.0
local FLOORS = { "G", "L1", "L2", "L3" }
local MALL = { -66.0, -30.0, 66.0, 30.0 }
-- In front of the ground-floor escalators (the cascades start at x = -45 and +45), where
-- people line up for the up escalators and step off the down ones.
local FEET = { { -55.0, -6.0, -45.0, 6.0 }, { 45.0, -6.0, 55.0, 6.0 } }

local last_report, peak = -1, {}

local function active()
  return scene.find("South entrance") ~= nil and scene.find("G Supermarket") ~= nil
end

local function on_floor(k)
  local y = (k - 1) * STOREY
  return agents.region(MALL[1], MALL[2], MALL[3], MALL[4], y - 0.6, y + 0.6).count
end

local function waiting()
  local n = 0
  for _, r in ipairs(FEET) do
    n = n + agents.region(r[1], r[2], r[3], r[4], -0.6, 0.6).count
  end
  return n
end

function mall_report()
  local t = sim.time()
  local m = metrics.get()
  local counts, inside = {}, 0
  for k = 1, #FLOORS do
    counts[k] = on_floor(k)
    inside = inside + counts[k]
  end
  local everyone = agents.region(-200, -200, 200, 200).count
  local outside = agents.region(-22, -46, 22, -30.2, -0.6, 0.6).count + agents.region(-22, 30.2, 22, 44, -0.6, 0.6).count
  local parts = {}
  for k, name in ipairs(FLOORS) do
    parts[#parts + 1] = string.format("%s %d", name, counts[k])
  end
  local feet = waiting()
  for k = 1, #FLOORS do
    peak[k] = math.max(peak[k] or 0, counts[k])
  end
  peak.feet = math.max(peak.feet or 0, feet)
  local lines = {
    string.format("Mall at t = %d s: %d people here, %d have come so far", math.floor(t), m.alive, m.spawned),
    "  by floor: " .. table.concat(parts, ", ") ..
      string.format("; %d on escalators or stairs; %d on the plazas", math.max(0, everyone - inside - outside), outside),
    string.format("  at the foot of the ground-floor escalators: %d (most so far %d)", feet, peak.feet),
    string.format("  most on each floor so far: G %d, L1 %d, L2 %d, L3 %d", peak[1], peak[2], peak[3], peak[4]),
  }
  local text = table.concat(lines, "\n")
  log.info(text)
  return text
end

events.on("step", function(_, time)
  if not active() then
    return
  end
  if time < last_report then
    last_report, peak = -1, {}  -- the simulation was reset
  end
  if last_report < 0 or time - last_report >= 60 then
    last_report = time
    mall_report()
  end
end)
