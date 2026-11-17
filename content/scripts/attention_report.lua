-- Attention report for any scene with attention targets (objects tagged `attention`: targets,
-- signs, exits..., see the Attention panel). Every 60 s of simulated time it logs the targets
-- ranked by how often they were noticed, with their opportunity to see, viewability and
-- dwell. Does nothing in scenes without targets.
--
-- From the Script console:  return attention_report()      (top 10)
--                           return attention_report(50)    (top 50)

local INTERVAL = 60
local last = -1

local function fetch()
  local ok, r = pcall(sim.command, "GetAttentionMetrics", {})
  if not ok or type(r) ~= "table" or type(r.targets) ~= "table" or #r.targets == 0 then
    return nil
  end
  return r
end

function attention_report(top)
  top = top or 10
  local r = fetch()
  if not r then
    return "No attention targets in this scene (tag a thin box `attention`)."
  end
  local targets = r.targets
  table.sort(targets, function(a, b)
    if a.noticeRate ~= b.noticeRate then
      return a.noticeRate > b.noticeRate
    end
    return a.name < b.name
  end)
  local ots, noticed = 0, 0
  for _, a in ipairs(targets) do
    ots = ots + a.ots
    noticed = noticed + a.noticed
  end
  local lines = {
    string.format("Attention at t = %d s: %d targets, %d people seen, %.1f%% of opportunities noticed",
      math.floor(sim.time()), #targets, r.agentsSeen, ots > 0 and 100 * noticed / ots or 0),
    "  rank  target                  OTS   viewable  noticed  dwell/viewer  angle  size",
  }
  for i = 1, math.min(top, #targets) do
    local a = targets[i]
    lines[#lines + 1] = string.format("  %4d  %-22s %5d  %7.1f%%  %6.1f%%  %10.2f s  %4.0f   %4.1f%%",
      i, a.name, a.ots, a.viewabilityRate, a.noticeRate, a.meanDwellPerViewer, a.meanViewingAngle,
      a.meanAngularSize)
  end
  local text = table.concat(lines, "\n")
  log.info(text)
  return text
end

events.on("step", function(_, time)
  if time < last then
    last = -1  -- the simulation was reset
  end
  if last >= 0 and time - last < INTERVAL then
    return
  end
  last = time
  if time > 0 and fetch() then
    attention_report()
  end
end)
