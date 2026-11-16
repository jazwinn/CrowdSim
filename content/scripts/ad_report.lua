-- Ad effectiveness report for any scene with ads (objects tagged `ad`, see the Ads panel).
-- Every 60 s of simulated time it logs the ads ranked by how often they were noticed, with
-- their opportunity to see, viewability and dwell. Does nothing in scenes without ads.
--
-- From the Script console:  return ad_report()      (top 10)
--                           return ad_report(50)    (top 50)

local INTERVAL = 60
local last = -1

local function fetch()
  local ok, r = pcall(sim.command, "GetAdMetrics", {})
  if not ok or type(r) ~= "table" or type(r.ads) ~= "table" or #r.ads == 0 then
    return nil
  end
  return r
end

function ad_report(top)
  top = top or 10
  local r = fetch()
  if not r then
    return "No ads in this scene (tag a thin box `ad`)."
  end
  local ads = r.ads
  table.sort(ads, function(a, b)
    if a.noticeRate ~= b.noticeRate then
      return a.noticeRate > b.noticeRate
    end
    return a.name < b.name
  end)
  local ots, noticed = 0, 0
  for _, a in ipairs(ads) do
    ots = ots + a.ots
    noticed = noticed + a.noticed
  end
  local lines = {
    string.format("Ads at t = %d s: %d ads, %d people seen, %.1f%% of opportunities noticed",
      math.floor(sim.time()), #ads, r.agentsSeen, ots > 0 and 100 * noticed / ots or 0),
    "  rank  ad                      OTS   viewable  noticed  dwell/viewer  angle  size",
  }
  for i = 1, math.min(top, #ads) do
    local a = ads[i]
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
    ad_report()
  end
end)
