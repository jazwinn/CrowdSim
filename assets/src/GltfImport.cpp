// cgltf implementation lives in exactly one translation unit.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>

#include "crowdsim/assets/AssetLibrary.hpp"

namespace crowdsim::assets {

namespace {

struct DataDeleter {
    void operator()(cgltf_data* d) const noexcept { cgltf_free(d); }
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Flags implied by a node (name conventions, then explicit extras). Returns 0xff for "inherit".
std::uint8_t nodeFlags(const cgltf_node& node, std::vector<std::string>& tagsOut) {
    std::uint8_t flags = 0xff;
    const std::string name = node.name != nullptr ? lower(node.name) : std::string();
    auto has = [&](const char* word) { return name.find(word) != std::string::npos; };
    if (has("obstacle") || has("wall") || has("pillar") || has("column") || has("blocker")) {
        flags = geometry::kSurfaceObstacle;
        tagsOut.emplace_back("obstacle");
    } else if (has("nowalk") || has("ceiling") || has("roof")) {
        flags = geometry::kSurfaceNone;
        tagsOut.emplace_back("nowalk");
    } else if (has("floor") || has("stairs") || has("ramp") || has("walkable")) {
        flags = geometry::kSurfaceWalkable;
        tagsOut.emplace_back("walkable");
    }
    if (node.extras.data != nullptr) {
        const nlohmann::json extras = nlohmann::json::parse(node.extras.data, nullptr, false);
        const nlohmann::json* tags = nullptr;
        if (extras.is_object() && extras.contains("crowdsim") && extras["crowdsim"].contains("tags")) {
            tags = &extras["crowdsim"]["tags"];
        } else if (extras.is_object() && extras.contains("tags")) {
            tags = &extras["tags"];
        }
        if (tags != nullptr && tags->is_array()) {
            for (const auto& t : *tags) {
                if (!t.is_string()) {
                    continue;
                }
                const std::string tag = t.get<std::string>();
                tagsOut.push_back(tag);
                if (tag == "obstacle") {
                    flags = geometry::kSurfaceObstacle;
                } else if (tag == "walkable") {
                    flags = geometry::kSurfaceWalkable;
                } else if (tag == "nowalk") {
                    flags = geometry::kSurfaceNone;
                }
            }
        }
    }
    return flags;
}

std::uint32_t materialColor(const cgltf_primitive& prim) {
    float rgba[4] = {0.72f, 0.72f, 0.72f, 1.0f};
    if (prim.material != nullptr && prim.material->has_pbr_metallic_roughness) {
        std::copy_n(prim.material->pbr_metallic_roughness.base_color_factor, 4, rgba);
    }
    auto ch = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (ch(rgba[3]) << 24) | (ch(rgba[2]) << 16) | (ch(rgba[1]) << 8) | ch(rgba[0]);
}

void importNode(const cgltf_node& node, std::uint8_t inheritedFlags, ImportedMesh& out, geometry::TriangleMesh& mesh) {
    std::vector<std::string> tags;
    const std::uint8_t own = nodeFlags(node, tags);
    const std::uint8_t flags = own == 0xff ? inheritedFlags : own;
    if (!tags.empty() && node.name != nullptr) {
        std::string line = std::string(node.name) + ":";
        for (const std::string& t : tags) {
            line += " " + t;
        }
        out.nodeTags.push_back(line);
    }

    if (node.mesh != nullptr) {
        float m[16];
        cgltf_node_transform_world(&node, m);
        const glm::mat4 world(glm::vec4(m[0], m[1], m[2], m[3]), glm::vec4(m[4], m[5], m[6], m[7]),
                              glm::vec4(m[8], m[9], m[10], m[11]), glm::vec4(m[12], m[13], m[14], m[15]));
        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) {
                continue;
            }
            const cgltf_accessor* positions = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position) {
                    positions = prim.attributes[a].data;
                }
            }
            if (positions == nullptr) {
                continue;
            }
            const auto base = static_cast<std::uint32_t>(mesh.positions.size());
            for (cgltf_size v = 0; v < positions->count; ++v) {
                float p[3] = {0.0f, 0.0f, 0.0f};
                cgltf_accessor_read_float(positions, v, p, 3);
                const glm::vec4 w = world * glm::vec4(p[0], p[1], p[2], 1.0f);
                // glTF is right-handed (+Z towards the viewer); CrowdSim is left-handed: flip Z.
                mesh.positions.emplace_back(w.x, w.y, -w.z);
            }
            const std::uint32_t color = materialColor(prim);
            const cgltf_size indexCount = prim.indices != nullptr ? prim.indices->count : positions->count;
            for (cgltf_size i = 0; i + 2 < indexCount; i += 3) {
                auto idx = [&](cgltf_size k) {
                    return base + static_cast<std::uint32_t>(
                                      prim.indices != nullptr ? cgltf_accessor_read_index(prim.indices, k) : k);
                };
                // The Z flip mirrors the mesh, so swap winding to keep faces outward.
                mesh.indices.insert(mesh.indices.end(), {idx(i), idx(i + 2), idx(i + 1)});
                mesh.triangleFlags.push_back(flags);
                mesh.triangleColors.push_back(color);
                if ((flags & geometry::kSurfaceObstacle) != 0) {
                    ++out.obstacleTriangles;
                } else if ((flags & geometry::kSurfaceWalkable) != 0) {
                    ++out.walkableTriangles;
                }
            }
        }
    }
    for (cgltf_size c = 0; c < node.children_count; ++c) {
        importNode(*node.children[c], flags, out, mesh);
    }
}

}  // namespace

ImportedMesh importGltf(const std::filesystem::path& file) {
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    const std::string path = file.string();
    if (cgltf_parse_file(&options, path.c_str(), &raw) != cgltf_result_success) {
        throw std::runtime_error("glTF: cannot parse '" + path + "'");
    }
    const std::unique_ptr<cgltf_data, DataDeleter> data(raw);
    if (cgltf_load_buffers(&options, data.get(), path.c_str()) != cgltf_result_success) {
        throw std::runtime_error("glTF: cannot load buffers for '" + path + "'");
    }
    if (cgltf_validate(data.get()) != cgltf_result_success) {
        throw std::runtime_error("glTF: validation failed for '" + path + "'");
    }

    ImportedMesh out;
    auto mesh = std::make_shared<geometry::TriangleMesh>();
    const cgltf_scene* scene =
        data->scene != nullptr ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene != nullptr) {
        for (cgltf_size n = 0; n < scene->nodes_count; ++n) {
            importNode(*scene->nodes[n], geometry::kSurfaceWalkable, out, *mesh);
        }
    } else {
        for (cgltf_size n = 0; n < data->nodes_count; ++n) {
            if (data->nodes[n].parent == nullptr) {
                importNode(data->nodes[n], geometry::kSurfaceWalkable, out, *mesh);
            }
        }
    }
    if (mesh->empty()) {
        throw std::runtime_error("glTF: no triangle geometry in '" + path + "'");
    }
    out.mesh = std::move(mesh);
    return out;
}

}  // namespace crowdsim::assets
