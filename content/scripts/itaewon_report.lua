-- Itaewon 2022 crush study: reports crowd conditions in the alley beside the Hamilton
-- Hotel for scenes/itaewon_2022*.scene.json (made by tools/make_itaewon_scene.py).
-- Does nothing in other scenes. Every 30 s of simulated time it logs the density,
-- pressure and falls in the narrow section where the crush happened and in the alley as
-- a whole, plus when danger thresholds were first crossed.
--
-- From the Script console:  return itaewon_report()

-- Alley: x -2..2, z 14..59. Narrow section (the hotel's wall): z 24..30.
local NARROW = { -2.0, 24.0, 2.0, 30.0 }
local ALLEY = { -2.0, 14.0, 2.0, 59.0 }
local THRESHOLDS = { 4, 6, 8, 10 }  -- people/m2: dense, contact, dangerous, extreme

local worst, first_over, last_report, last_sample = {}, {}, 0, -1

local function reset()
  worst = { density = 0, density_t = 0, pressure = 0, pressure_t = 0, fallen = 0, fallen_t = 0 }
  first_over, last_report, last_sample = {}, 0, -1
end
reset()

local function active()
  return scene.find("Station Exit 1") ~= nil and scene.find("World Food Street west") ~= nil
end

local function region(r)
  return agents.region(r[1], r[2], r[3], r[4])
end

function itaewon_report()
  local t = sim.time()
  local narrow, alley = region(NARROW), region(ALLEY)
  local m = metrics.get()
  local lines = { string.format("Itaewon alley at t = %d s (%s)", math.floor(t), scene.name and scene.name() or "") }
  lines[#lines + 1] = string.format(
    "  narrow section: %d people, %.1f /m2, pressure peak %.0f N/m (mean %.0f), %d on the ground",
    narrow.count, narrow.density, narrow.peak_pressure, narrow.mean_pressure, narrow.fallen)
  lines[#lines + 1] = string.format("  whole alley:    %d people, %.1f /m2, %d on the ground",
    alley.count, alley.density, alley.fallen)
  lines[#lines + 1] = string.format("  falls so far: %d; highest pressure anywhere now %.0f N/m",
    m.falls or 0, m.peak_pressure or 0)
  local over = {}
  for _, d in ipairs(THRESHOLDS) do
    over[#over + 1] = first_over[d] and string.format("%d/m2 at %d s", d, first_over[d]) or string.format("%d/m2 never", d)
  end
  lines[#lines + 1] = "  narrow section first reached " .. table.concat(over, ", ")
  lines[#lines + 1] = string.format(
    "  worst so far: %.1f /m2 (t %d s), %.0f N/m (t %d s), %d on the ground (t %d s)",
    worst.density, worst.density_t, worst.pressure, worst.pressure_t, worst.fallen, worst.fallen_t)
  local ok, summary = pcall(sim.command, "GetMetrics", {})
  if ok and type(summary) == "table" and summary.pressure then
    lines[#lines + 1] = string.format("  people above %d N/m for %d s or more: %d",
      summary.pressure.criticalLevel, summary.pressure.criticalSeconds, summary.pressure.criticallyExposed)
  end
  local text = table.concat(lines, "\n")
  log.info(text)
  return text
end

events.on("step", function(_, time)
  if not active() then
    return
  end
  if time < last_sample then
    reset()  -- the simulation was reset
  end
  if time - last_sample < 0.5 then
    return
  end
  last_sample = time
  local narrow = region(NARROW)
  for _, d in ipairs(THRESHOLDS) do
    if not first_over[d] and narrow.density >= d then
      first_over[d] = math.floor(time)
    end
  end
  if narrow.density > worst.density then
    worst.density, worst.density_t = narrow.density, math.floor(time)
  end
  if narrow.peak_pressure > worst.pressure then
    worst.pressure, worst.pressure_t = narrow.peak_pressure, math.floor(time)
  end
  local alley = region(ALLEY)
  if alley.fallen > worst.fallen then
    worst.fallen, worst.fallen_t = alley.fallen, math.floor(time)
  end
  if time - last_report >= 30 then
    last_report = time
    itaewon_report()
  end
end)
