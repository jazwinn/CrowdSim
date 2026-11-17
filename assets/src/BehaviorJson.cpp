#include "crowdsim/assets/BehaviorJson.hpp"

#include "crowdsim/assets/SceneReflection.hpp"

#include <format>
#include <stdexcept>

#include "crowdsim/assets/JsonTypes.hpp"

namespace crowdsim::assets {

namespace {

const char* kindName(sim::BtParamKind k) {
    switch (k) {
        case sim::BtParamKind::Number:
            return "number";
        case sim::BtParamKind::Text:
            return "string";
        case sim::BtParamKind::Vec3:
            return "vec3";
    }
    return "number";
}

const char* categoryName(sim::BtCategory c) {
    switch (c) {
        case sim::BtCategory::Composite:
            return "composite";
        case sim::BtCategory::Decorator:
            return "decorator";
        case sim::BtCategory::Action:
            return "action";
        case sim::BtCategory::Condition:
            return "condition";
    }
    return "action";
}

}  // namespace

sim::BtNodeDesc btNodeFromJson(const Json& node, const std::string& path) {
    if (!node.is_object()) {
        throw std::invalid_argument(path + ": node must be an object");
    }
    sim::BtNodeDesc d;
    d.type = node.value("type", std::string());
    if (d.type.empty()) {
        throw std::invalid_argument(path + ": node has no \"type\"");
    }
    d.id = node.value("id", std::string());
    if (node.contains("params")) {
        const Json& params = node["params"];
        if (!params.is_object()) {
            throw std::invalid_argument(path + ": \"params\" must be an object");
        }
        for (auto it = params.begin(); it != params.end(); ++it) {
            sim::BtParamValue v;
            const Json& value = it.value();
            if (value.is_number()) {
                v.kind = sim::BtParamKind::Number;
                v.number = value.get<float>();
            } else if (value.is_string()) {
                v.kind = sim::BtParamKind::Text;
                v.text = value.get<std::string>();
            } else if (value.is_array() && value.size() == 3) {
                v.kind = sim::BtParamKind::Vec3;
                v.vec = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
            } else if (value.is_boolean()) {
                v.kind = sim::BtParamKind::Number;
                v.number = value.get<bool>() ? 1.0f : 0.0f;
            } else {
                throw std::invalid_argument(
                    std::format("{}: parameter '{}' must be a number, string or [x, y, z]", path, it.key()));
            }
            d.params.emplace(it.key(), std::move(v));
        }
    }
    if (node.contains("children")) {
        if (!node["children"].is_array()) {
            throw std::invalid_argument(path + ": \"children\" must be an array");
        }
        std::size_t index = 0;
        for (const Json& child : node["children"]) {
            d.children.push_back(
                btNodeFromJson(child, std::format("{}/{}[{}]", path, child.value("type", "?"), index++)));
        }
    }
    return d;
}

Json btNodeToJson(const sim::BtNodeDesc& node) {
    Json j{{"type", node.type}};
    if (!node.id.empty()) {
        j["id"] = node.id;
    }
    if (!node.params.empty()) {
        Json params = Json::object();
        for (const auto& [key, v] : node.params) {
            switch (v.kind) {
                case sim::BtParamKind::Number:
                    params[key] = roundFloat(v.number);
                    break;
                case sim::BtParamKind::Text:
                    params[key] = v.text;
                    break;
                case sim::BtParamKind::Vec3:
                    params[key] = Json::array({roundFloat(v.vec.x), roundFloat(v.vec.y), roundFloat(v.vec.z)});
                    break;
            }
        }
        j["params"] = params;
    }
    if (!node.children.empty()) {
        Json children = Json::array();
        for (const sim::BtNodeDesc& c : node.children) {
            children.push_back(btNodeToJson(c));
        }
        j["children"] = children;
    }
    return j;
}

sim::CompiledTree compileBehaviorJson(const Json& doc, std::span<const sim::CustomNodeSpec> customNodes) {
    if (!doc.is_object()) {
        throw std::invalid_argument("behaviour tree must be a JSON object");
    }
    if (doc.value("format", std::string("crowdsim.bt")) != "crowdsim.bt") {
        throw std::invalid_argument("not a crowdsim.bt document");
    }
    if (doc.value("version", 1) > 1) {
        throw std::invalid_argument("behaviour tree version is newer than this build supports");
    }
    const std::string name = doc.value("name", std::string());
    if (name.empty()) {
        throw std::invalid_argument("behaviour tree needs a \"name\"");
    }
    if (!doc.contains("root")) {
        throw std::invalid_argument("behaviour tree needs a \"root\" node");
    }
    return sim::compileBehaviorTree(btNodeFromJson(doc["root"]), name, customNodes);
}

Json behaviorToJson(const sim::CompiledTree& tree) {
    return Json{{"format", "crowdsim.bt"},
                {"version", 1},
                {"name", tree.name},
                {"root", btNodeToJson(sim::decompileBehaviorTree(tree))}};
}

sim::AgentType agentTypeFromJson(const Json& doc) {
    if (!doc.is_object()) {
        throw std::invalid_argument("agent type must be a JSON object");
    }
    if (doc.value("format", std::string("crowdsim.agent")) != "crowdsim.agent") {
        throw std::invalid_argument("not a crowdsim.agent document");
    }
    sim::AgentType t;
    reflect::fromJson(doc, t);  // fields: assets/SceneReflection.hpp
    return t;
}

Json toJson(const sim::AgentType& type) {
    Json j = reflect::toJson(type);
    j["format"] = "crowdsim.agent";
    j["version"] = 1;
    return j;
}

Json btNodeTypesJson(std::span<const sim::CustomNodeSpec> customNodes) {
    Json list = Json::array();
    for (const sim::BtNodeTypeInfo& info : sim::btNodeTypes()) {
        if (info.type == sim::BtNodeType::Custom) {
            continue;
        }
        Json params = Json::array();
        for (std::uint8_t p = 0; p < info.paramCount; ++p) {
            const sim::BtParamSpec& spec = info.params[p];
            Json param{{"name", spec.name}, {"type", kindName(spec.kind)}, {"description", spec.description}};
            if (spec.kind == sim::BtParamKind::Number) {
                param["default"] = spec.defaultNumber;
            } else if (spec.kind == sim::BtParamKind::Text) {
                param["default"] = spec.defaultText;
            }
            params.push_back(param);
        }
        list.push_back(Json{{"type", info.name},
                            {"category", categoryName(info.category)},
                            {"minChildren", info.minChildren},
                            {"maxChildren", info.maxChildren},
                            {"params", params},
                            {"description", info.description}});
    }
    for (const sim::CustomNodeSpec& spec : customNodes) {
        Json params = Json::array();
        for (const sim::CustomParamSpec& p : spec.params) {
            Json param{{"name", p.name}, {"type", kindName(p.kind)}, {"description", p.description}};
            if (p.kind == sim::BtParamKind::Number) {
                param["default"] = p.defaultNumber;
            } else if (p.kind == sim::BtParamKind::Text) {
                param["default"] = p.defaultText;
            } else {
                param["default"] = Json::array({p.defaultVec.x, p.defaultVec.y, p.defaultVec.z});
            }
            params.push_back(param);
        }
        list.push_back(Json{{"type", spec.name},
                            {"category", categoryName(spec.category)},
                            {"minChildren", 0},
                            {"maxChildren", 0},
                            {"params", params},
                            {"description", spec.description},
                            {"script", true}});
    }
    return list;
}

}  // namespace crowdsim::assets
