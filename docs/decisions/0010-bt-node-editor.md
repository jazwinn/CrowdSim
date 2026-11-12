# 0010 — Behaviour-tree node editor: a UI-free graph model, layout in the asset

Status: accepted (M7)

## Context

Behaviour trees must be editable visually (imgui-node-editor): create, connect and delete nodes,
edit parameters, see validation errors and the selected agent's live state. Trees must
round-trip to identical JSON so editor saves and hand edits diff cleanly, and so an AI can edit
the same files.

## Decision

- **Graph model outside the UI.** `assets::BtGraph` holds nodes (graph handle, tree node id,
  type, parameters as raw JSON, position) and parent→child links. It owns the rules: leaves take
  no children, decorators one; one parent; no cycles. It also does per-node validation and
  conversion to and from `*.bt.json`. The ImGui panel (`ui::BtEditor`) only draws and edits a
  BtGraph, so everything except drawing is unit-tested without a window.
- **Order by position.** Trees grow left to right and a node's children run top to bottom, so
  reordering children is dragging. Loading a file without layout auto-arranges it in JSON order.
- **Layout lives in the asset.** Positions are stored in the document's `editor.positions` block
  (whole pixels, keyed by node id), which the runtime ignores. imgui-node-editor's own settings
  file is disabled. Parameters stay raw JSON, so values the user did not touch keep their exact
  text. Consequently `toJson(fromJson(doc)) == doc` for any document the editor wrote.
- **Commands only.** Opening is `GetBehaviorTree`, which returns the file (with layout) when it
  matches the tree in use, else the live tree. Applying is `SetBehaviorTree` (hot reload; running
  agents restart the tree). Saving is `SaveBehaviorTree` (writes `content/behaviours`, not
  logged; the file watcher then reloads it, which changes nothing). Validation runs every frame:
  graph rules first, then the real compiler, so the editor never offers to apply a tree the
  runtime would reject.
- **Live highlighting** maps the selected agent's compiled node flags (running / visited) onto
  graph nodes through the stable node ids.
- **Script nodes are first-class**: the create menu lists Lua nodes with their parameter
  schemas next to the built-in ones.

## Consequences

- Node ids must be unique within a tree; the editor generates them and validation flags duplicates.
- Child order follows y position, so laying nodes out deliberately is part of editing; Layout
  re-tidies without changing the order.
- Edited numbers are stored with four decimals to keep files readable.
