# 0014: Reflection descriptors for data types

## Context

Each scene type (objects, links, goals, spawners, agent parameters, navigation settings,
bodies) had a hand-written `toJson` and `...FromJson`, with its own validation and error
text, and the Inspector edited the raw JSON with guesses from key names (a key called
`rotationY` drags in degrees, `color` shows a colour picker). Adding a field meant editing
the struct, both converters and sometimes the Inspector, and the Inspector could not show
units, limits or explanations.

## Decision

A `reflect` module (no project dependencies; glm, nlohmann_json and magic_enum) describes a
type once, beside the code that owns its format:

```cpp
CROWDSIM_REFLECT(crowdsim::sim::Goal,
    field("name", &T::name),
    field("area", &T::area).required(),
    field("serviceTime", &T::serviceTime).atLeast(0).unit("s").tip("How long each person is served"))
```

- Descriptors are **outside the structs** (`assets/SceneReflection.hpp` for the scene types),
  so `sim` stays plain data with no JSON or editor knowledge, and the JSON key need not be
  the member name (`agentMaxSlope` is `agentMaxSlopeDegrees`). `property(key, get, set)`
  reaches values saved in other units or shapes (rotation in degrees, flags merged into
  tags); `range` saves a min/max pair; `when` makes a field conditional (an object's `steps`
  only for stairs); `check` adds rules across fields.
- From a descriptor come `reflect::toJson`, `reflect::fromJson` (merging over defaults,
  errors naming the field path: `area.halfExtents: must be above 0`) and
  `ui::inspect` (one property row per field with its label, unit, limits and tooltip; enums
  as dropdowns via magic_enum). Fields that need scene knowledge (pick a goal, an agent
  type) name an editor-supplied widget with `.widget(name)`.
- `assets/JsonTypes.cpp` keeps its functions for callers and adds only what is not about one
  value: default names, mesh loading, goal references by name, the entity envelope.

## Consequences

- The file format did not change: every content scene saves byte-for-byte as before (checked
  against the previous serializer), and a test round-trips every scene.
- A new field is one line in the descriptor; it is saved, read, validated and shown.
- Behaviour tree node parameters keep their own table (`kNodeTypes`), which already drives
  validation and the node editor; agent type files and the attention parameters are not yet
  described and can move over the same way.
