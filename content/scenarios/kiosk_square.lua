-- Scripted scenario: an open square with three information kiosks. Tourists visit
-- kiosks before leaving through the east exit; commuters walk straight across.
--   crowdsim --script scenarios/kiosk_square.lua
-- Needs content/scripts/info_kiosk.lua (loaded automatically) for the kiosk type
-- and the VisitAttraction node used by the tourist behaviour.

scene.command("CreateScene", { name = "kiosk_square" })
scene.place{ name = "Square", type = "box", position = { 0, -0.2, 0 }, size = { 90, 0.2, 50 },
             color = "#5c626a", tags = { "walkable" } }
scene.place{ name = "Planter", type = "box", position = { 0, 0, -12 }, size = { 6, 0.6, 3 },
             color = "#4f6b45", tags = { "obstacle" } }

entities.spawn("info_kiosk", { position = { -12, 0, 4 } })
entities.spawn("info_kiosk", { position = { 6, 0, -6 } })
entities.spawn("info_kiosk", { position = { 18, 0, 10 } })

local exit = scene.create_goal{ name = "East exit", area = { center = { 42, 0 }, halfExtents = { 2, 20 }, elevation = 0 } }
scene.create_spawner{
  name = "West gate",
  area = { center = { -42, 0 }, halfExtents = { 2, 18 }, elevation = 0 },
  rate = 6, total = 600, goal = exit,
  agentMix = { { type = "tourist", weight = 0.6 }, { type = "commuter", weight = 0.4 } },
}

arrivals = 0
events.on("agent_arrived", function(id, goal) arrivals = arrivals + 1 end)
sim.play()
