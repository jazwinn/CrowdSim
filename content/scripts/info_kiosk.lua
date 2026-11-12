-- Information kiosk: a scripted entity type and a behaviour-tree node that sends
-- curious agents to the nearest attraction, lets them browse, then lets them go.
-- Loaded automatically from content/scripts; edit and save to hot-reload.

-- Visitor statistics, readable from the console: return kiosk_stats
kiosk_stats = kiosk_stats or { visits = 0, browsing_seconds = 0 }

entities.register_type{
  name = "info_kiosk",
  type = "box",
  size = { 1.2, 2.2, 1.2 },
  color = "#3a8fd8",
  tags = { "attraction" },
  tick_hz = 1,
  on_spawn = function(e)
    log.info("info kiosk #" .. e.id .. " open at " .. e.position[1] .. ", " .. e.position[3])
  end,
  on_tick = function(e, dt)
    -- Agents browsing within 3 m count towards the kiosk's usage.
    local p = e.position
    kiosk_stats.browsing_seconds = kiosk_stats.browsing_seconds + agents.count_near(p[1], p[2], p[3], 3.0) * dt
  end,
}

-- Per-agent visit state, keyed by agent id (never iterated, so order does not matter).
local visits = {}

local function attractions(tag)
  local list = {}
  for _, e in ipairs(entities.list()) do
    for _, t in ipairs(e.tags or {}) do
      if t == tag then
        list[#list + 1] = e
        break
      end
    end
  end
  return list
end

bt.register_node{
  name = "VisitAttraction",
  kind = "action",
  description = "Walk to the nearest object tagged `tag` within `radius`, browse for `seconds`, succeed. "
    .. "Fails if there is none or it cannot be reached within `timeout` seconds.",
  params = {
    tag = { type = "string", default = "attraction", description = "object tag to visit" },
    radius = { type = "number", default = 30, description = "search radius (m)" },
    seconds = { type = "number", default = 4, description = "browsing time (s)" },
    timeout = { type = "number", default = 60, description = "give up after this long (s)" },
  },
  tick = function(ctx, agents)
    local p = ctx.params
    local now = ctx:time()
    local targets = attractions(p.tag)
    local results = {}
    for k, a in ipairs(agents) do
      local id = ctx:id(a)
      local v = visits[id]
      local x, y, z = ctx:position(a)
      if not v then
        local best, bestD = nil, p.radius * p.radius
        for _, e in ipairs(targets) do
          local dx, dz = e.position[1] - x, e.position[3] - z
          local d = dx * dx + dz * dz
          if d < bestD then best, bestD = e, d end
        end
        if not best then
          results[k] = "failure"
        else
          -- A spot around the kiosk, from the agent's own random stream (deterministic).
          local angle = ctx:random(a) * 2 * math.pi
          v = { x = best.position[1] + math.cos(angle) * 1.5, y = best.position[2],
                z = best.position[3] + math.sin(angle) * 1.5, giveUp = now + p.timeout }
          visits[id] = v
          ctx:set_state(a, "visiting")
          ctx:move_to(a, v.x, v.y, v.z)
          results[k] = "running"
        end
      elseif v.leaveAt then
        if now >= v.leaveAt then
          visits[id] = nil
          kiosk_stats.visits = kiosk_stats.visits + 1
          results[k] = "success"
        else
          results[k] = "running"
        end
      elseif (x - v.x) ^ 2 + (z - v.z) ^ 2 < 0.8 ^ 2 then
        v.leaveAt = now + p.seconds
        ctx:stop(a)
        ctx:set_state(a, "browsing")
        results[k] = "running"
      elseif now > v.giveUp then
        visits[id] = nil
        results[k] = "failure"
      else
        ctx:move_to(a, v.x, v.y, v.z)
        results[k] = "running"
      end
    end
    return results
  end,
}

-- Agents that leave the simulation drop their visit state (handlers are replaced on reload).
events.on("agent_despawned", function(id) visits[id] = nil end)
