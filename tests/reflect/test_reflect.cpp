// Reflection (reflect/Reflect.hpp): a described type saves and reads every kind of field,
// keeps defaults for absent keys, and reports bad values with the path to them. The scene
// types' descriptors (assets/SceneReflection.hpp) keep the file format: every content scene
// saves, reloads and saves again to the same document.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/assets/BehaviorJson.hpp"
#include "crowdsim/assets/SceneReflection.hpp"
#include "crowdsim/assets/SceneSerializer.hpp"
#include "crowdsim/assets/TagCatalog.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/metrics/AttentionModel.hpp"
#include "crowdsim/reflect/Reflect.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace sample {

enum class Shape : std::uint8_t { Round, Square };

struct Inner {
    float width = 1.0f;
    std::string label = "inner";
};

struct Thing {
    std::string name = "thing";
    Shape shape = Shape::Round;
    float angle = 0.0f;  // radians
    glm::vec2 offset{0.0f};
    glm::vec3 tint{1.0f, 0.5f, 0.0f};
    float lo = 1.0f;
    float hi = 2.0f;
    float until = std::numeric_limits<float>::infinity();
    std::uint32_t corners = 0;  // only for squares
    Inner inner;
    std::vector<Inner> extras;
    std::vector<std::string> tags;
};

}  // namespace sample

CROWDSIM_REFLECT(sample::Inner,
    field("width", &T::width).above(0),
    field("label", &T::label))

CROWDSIM_REFLECT(sample::Thing,
    field("name", &T::name),
    field("shape", &T::shape).lowercase(),
    field("angle", &T::angle).degrees(),
    field("offset", &T::offset),
    field("tint", &T::tint).color(),
    range("size", &T::lo, &T::hi).above(0),
    field("until", &T::until).infinityAsNull(),
    field("corners", &T::corners).when([](const T& t) { return t.shape == sample::Shape::Square; }),
    field("inner", &T::inner).required(),
    field("extras", &T::extras),
    field("tags", &T::tags),
    check<T>([](const T& t) {
        if (t.name.empty()) {
            throw crowdsim::reflect::FieldError("name", "must not be empty");
        }
    }))

using namespace crowdsim;
using reflect::Json;

TEST_CASE("A described type saves every kind of field") {
    sample::Thing t;
    t.angle = 3.14159265f / 2.0f;
    t.extras.push_back({2.0f, "x"});
    t.tags = {"a", "b"};
    const Json j = reflect::toJson(t);
    CHECK(j["name"] == "thing");
    CHECK(j["shape"] == "round");
    CHECK(j["angle"] == 90.0);
    CHECK(j["offset"] == Json::array({0.0, 0.0}));
    CHECK(j["tint"] == "#ff8000");
    CHECK(j["size"] == Json::array({1.0, 2.0}));
    CHECK(j["until"].is_null());
    CHECK_FALSE(j.contains("corners"));  // only for squares
    CHECK(j["inner"] == Json{{"width", 1.0}, {"label", "inner"}});
    CHECK(j["extras"][0]["label"] == "x");
    CHECK(j["tags"] == Json::array({"a", "b"}));
}

TEST_CASE("Reading merges over the value and round-trips") {
    sample::Thing t;
    reflect::fromJson(Json{{"inner", Json::object()}, {"shape", "SQUARE"}, {"corners", 4}, {"size", 3}, {"until", 9}},
                      t);
    CHECK(t.name == "thing");  // absent: unchanged
    CHECK(t.shape == sample::Shape::Square);
    CHECK(t.corners == 4);
    CHECK(t.lo == 3.0f);
    CHECK(t.hi == 3.0f);
    CHECK(t.until == 9.0f);
    const sample::Thing back = reflect::fromJson<sample::Thing>(reflect::toJson(t));
    CHECK(reflect::toJson(back) == reflect::toJson(t));
}

TEST_CASE("Bad values are reported with the path to them") {
    auto error = [](const Json& j) {
        sample::Thing t;
        try {
            reflect::fromJson(j, t);
        } catch (const reflect::FieldError& e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    CHECK(error(Json::object()) == "inner: is required");
    const Json inner{{"inner", Json::object()}};
    CHECK(error(Json{{"inner", {{"width", 0}}}}) == "inner.width: must be above 0");
    CHECK(error(Json{{"inner", Json::object()}, {"extras", {{{"width", -1}}}}}) == "extras.0.width: must be above 0");
    CHECK(error(Json{{"inner", Json::object()}, {"size", {3, 1}}}) == "size: must be [min, max] with min <= max");
    CHECK(error(Json{{"inner", Json::object()}, {"shape", "hex"}}) ==
          "shape: expected one of round, square, got \"hex\"");
    CHECK(error(Json{{"inner", Json::object()}, {"offset", {1, 2, 3}}}) == "offset: expected [x, z], got [1,2,3]");
    CHECK(error(Json{{"inner", Json::object()}, {"tint", "red"}}) == "tint: expected colour \"#rrggbb\", got \"red\"");
    CHECK(error(Json{{"inner", Json::object()}, {"name", ""}}) == "name: must not be empty");
    CHECK(error(inner) == "no error");
}

TEST_CASE("Fields are visited in order with their options") {
    std::vector<std::string> keys;
    reflect::forEachField<sample::Thing>([&](const auto& f) { keys.emplace_back(f.key); });
    CHECK(keys == std::vector<std::string>{"name", "shape", "angle", "offset", "tint", "size", "until", "corners",
                                           "inner", "extras", "tags"});
}

TEST_CASE("Every content scene saves, reloads and saves again to the same document") {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{2};
    assets::AssetLibrary library{logs.logger(), CROWDSIM_CONTENT_DIR};
    int scenes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(library.contentRoot() / "scenes")) {
        if (!entry.path().string().ends_with(".scene.json")) {
            continue;
        }
        CAPTURE(entry.path().filename().string());
        sim::Simulation first{{logs.logger(), jobs}, sim::SimConfig{}};
        const std::string name = assets::loadSceneFile(entry.path(), first, library);
        const Json saved = assets::saveScene(first, name);
        sim::Simulation second{{logs.logger(), jobs}, sim::SimConfig{}};
        (void)assets::loadScene(saved, second, library);
        CHECK(assets::saveScene(second, name) == saved);
        ++scenes;
    }
    CHECK(scenes > 5);
}

TEST_CASE("Every content agent type saves, reloads and saves again to the same document") {
    const std::filesystem::path root = CROWDSIM_CONTENT_DIR;
    int types = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "agents")) {
        CAPTURE(entry.path().filename().string());
        std::ifstream in(entry.path());
        const sim::AgentType type = assets::agentTypeFromJson(Json::parse(in));
        const Json saved = assets::toJson(type);
        CHECK(saved["format"] == "crowdsim.agent");
        CHECK_FALSE(saved.contains("agentType"));  // a spawner setting, left out of agent files
        CHECK(saved.contains("radius"));           // body parameters sit beside the type's fields
        CHECK(assets::toJson(assets::agentTypeFromJson(saved)) == saved);
        ++types;
    }
    CHECK(types > 5);
    CHECK_THROWS_WITH((void)assets::agentTypeFromJson(Json{{"name", "x"}, {"height", 0.2}}),
                      "height: must be above 0.5");
}

TEST_CASE("Attention settings are described and checked") {
    metrics::AttentionParams p;
    const Json j = reflect::toJson(p);
    CHECK(j["fieldHalfAngle"] == 60.0);
    CHECK(j.size() == 19);
    reflect::fromJson(Json{{"glanceRate", 1.2}}, p);
    CHECK(p.glanceRate == doctest::Approx(1.2));
    CHECK(p.fieldHalfAngle == 60.0f);  // untouched
    CHECK_THROWS_WITH(reflect::fromJson(Json{{"glanceMin", 3}, {"glanceMax", 1}}, p), "glanceMax: must be at least glanceMin");
    CHECK_THROWS_WITH(reflect::fromJson(Json{{"fieldHalfAngle", 120}}, p),
                      "fieldHalfAngle: must be at least 10 and at most 90");
}

TEST_CASE("The tag catalogue explains tags and their values from content") {
    assets::TagCatalog catalog;
    const std::filesystem::path root = CROWDSIM_CONTENT_DIR;
    for (const auto& entry : std::filesystem::directory_iterator(root / "tags")) {
        CAPTURE(entry.path().filename().string());
        std::ifstream in(entry.path());
        CHECK_NOTHROW(catalog.define(entry.path().stem().string(), Json::parse(in)));
    }
    // Every tag the content scenes use is explained.
    for (const auto& entry : std::filesystem::directory_iterator(root / "scenes")) {
        std::ifstream in(entry.path());
        for (const Json& o : Json::parse(in).value("objects", Json::array())) {
            for (const Json& t : o.value("tags", Json::array())) {
                CAPTURE(entry.path().filename().string());
                CAPTURE(t.get<std::string>());
                CHECK(catalog.match(t.get<std::string>()).spec != nullptr);
            }
        }
    }
    const assets::TagMatch salience = catalog.match("attention:salience=0.6");
    REQUIRE(salience.spec != nullptr);
    CHECK(salience.spec->kind == assets::TagKind::Number);
    CHECK(salience.spec->requiredTag == "attention");
    CHECK(salience.value == "0.6");
    const assets::TagMatch area = catalog.match("area:alley");
    REQUIRE(area.spec != nullptr);
    CHECK(area.value == "alley");
    CHECK(assets::TagCatalog::compose(*area.spec, "plaza") == "area:plaza");
    CHECK(catalog.match("walkable").spec->kind == assets::TagKind::Flag);
    CHECK(catalog.match("made_up").spec == nullptr);

    // A source replaces its own tags; a bad document names the field.
    catalog.define("extra", Json{{"tags", {{{"name", "made_up"}, {"group", "Test"}}}}});
    CHECK(catalog.match("made_up").spec != nullptr);
    catalog.define("extra", Json{{"tags", Json::array()}});
    CHECK(catalog.match("made_up").spec == nullptr);
    CHECK_THROWS_WITH(catalog.define("bad", Json{{"tags", {{{"name", "x"}, {"kind", "colour"}}}}}),
                      "tags.0.kind: expected one of flag, number, text, got \"colour\"");
}
