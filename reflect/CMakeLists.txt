# reflect/: static reflection for plain data types. A descriptor beside each type lists its
# fields (JSON key, limits, unit, tooltip); JSON conversion, validation and the editor's
# property widgets are generated from it. Depends on no other project module.
crowdsim_add_library(reflect
  SOURCES
    src/Reflect.cpp
  DEPS
    glm::glm
    nlohmann_json::nlohmann_json
    magic_enum::magic_enum
)
