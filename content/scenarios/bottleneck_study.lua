-- Bottleneck study: the corridor scene with a counting gate across the doorway and
-- trajectory recording, ready for the Metrics panel, the heatmap overlay or MCP.
--   crowdsim --script scenarios/bottleneck_study.lua --overlays heatmap
scene.command("LoadScene", { path = "scenes/corridor_bottleneck.scene.json" })
scene.command("AddGate", { name = "doorway", a = { 0.4, -2.5 }, b = { 0.4, 2.5 } })
scene.command("StartRecording", { hz = 6 })
sim.play()
