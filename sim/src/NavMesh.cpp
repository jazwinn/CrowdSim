#include "crowdsim/sim/NavMesh.hpp"

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <spdlog/logger.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <glm/common.hpp>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Profile.hpp"

namespace crowdsim::sim {

namespace {

constexpr unsigned short kPolyFlagWalk = 0x01;
constexpr unsigned char kPolyAreaGround = 0;
constexpr int kMaxQueryNodes = 2048;

struct HeightfieldDeleter {
    void operator()(rcHeightfield* p) const noexcept { rcFreeHeightField(p); }
};
struct CompactDeleter {
    void operator()(rcCompactHeightfield* p) const noexcept { rcFreeCompactHeightfield(p); }
};
struct ContourDeleter {
    void operator()(rcContourSet* p) const noexcept { rcFreeContourSet(p); }
};
struct PolyMeshDeleter {
    void operator()(rcPolyMesh* p) const noexcept { rcFreePolyMesh(p); }
};
struct DetailDeleter {
    void operator()(rcPolyMeshDetail* p) const noexcept { rcFreePolyMeshDetail(p); }
};

/// Input prepared once and shared read-only by all tile builds.
struct BuildInput {
    std::vector<float> verts;
    std::vector<int> tris;
    std::vector<unsigned char> areas;
    std::vector<float> triMinX, triMaxX, triMinZ, triMaxZ;
    float bmin[3];
    float bmax[3];
};

struct TileResult {
    unsigned char* data = nullptr;
    int size = 0;
};

TileResult buildTile(const BuildInput& in, const NavBuildSettings& s, std::span<const OffMeshLink> links, int tx,
                     int ty) {
    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.agentMaxSlopeDegrees;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.agentMaxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(s.edgeMaxLength / cfg.cs);
    cfg.maxSimplificationError = s.edgeMaxError;
    cfg.minRegionArea = static_cast<int>(s.regionMinSize * s.regionMinSize);
    cfg.mergeRegionArea = static_cast<int>(s.regionMergeSize * s.regionMergeSize);
    cfg.maxVertsPerPoly = s.vertsPerPoly;
    cfg.tileSize = s.tileSizeCells;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = s.detailSampleDistance < 0.9f ? 0.0f : cfg.cs * s.detailSampleDistance;
    cfg.detailSampleMaxError = cfg.ch * s.detailSampleMaxError;

    const float tileWorld = static_cast<float>(cfg.tileSize) * cfg.cs;
    cfg.bmin[0] = in.bmin[0] + static_cast<float>(tx) * tileWorld;
    cfg.bmin[1] = in.bmin[1];
    cfg.bmin[2] = in.bmin[2] + static_cast<float>(ty) * tileWorld;
    cfg.bmax[0] = cfg.bmin[0] + tileWorld;
    cfg.bmax[1] = in.bmax[1];
    cfg.bmax[2] = cfg.bmin[2] + tileWorld;
    const float tileMinX = cfg.bmin[0];
    const float tileMinZ = cfg.bmin[2];
    const float border = static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmin[0] -= border;
    cfg.bmin[2] -= border;
    cfg.bmax[0] += border;
    cfg.bmax[2] += border;

    // Triangles overlapping this tile (with border).
    std::vector<int> tris;
    std::vector<unsigned char> areas;
    const auto numTris = static_cast<int>(in.areas.size());
    for (int t = 0; t < numTris; ++t) {
        const auto ut = static_cast<std::size_t>(t);
        if (in.triMaxX[ut] < cfg.bmin[0] || in.triMinX[ut] > cfg.bmax[0] || in.triMaxZ[ut] < cfg.bmin[2] ||
            in.triMinZ[ut] > cfg.bmax[2]) {
            continue;
        }
        tris.insert(tris.end(), {in.tris[3 * ut], in.tris[3 * ut + 1], in.tris[3 * ut + 2]});
        areas.push_back(in.areas[ut]);
    }
    if (tris.empty()) {
        return {};
    }

    rcContext ctx(false);
    std::unique_ptr<rcHeightfield, HeightfieldDeleter> solid(rcAllocHeightfield());
    if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        return {};
    }
    if (!rcRasterizeTriangles(&ctx, in.verts.data(), static_cast<int>(in.verts.size() / 3), tris.data(), areas.data(),
                              static_cast<int>(areas.size()), *solid, cfg.walkableClimb)) {
        return {};
    }
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    std::unique_ptr<rcCompactHeightfield, CompactDeleter> chf(rcAllocCompactHeightfield());
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf)) {
        return {};
    }
    solid.reset();
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) || !rcBuildDistanceField(&ctx, *chf) ||
        !rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) {
        return {};
    }
    std::unique_ptr<rcContourSet, ContourDeleter> cset(rcAllocContourSet());
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset) || cset->nconts == 0) {
        return {};
    }
    std::unique_ptr<rcPolyMesh, PolyMeshDeleter> pmesh(rcAllocPolyMesh());
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) {
        return {};
    }
    std::unique_ptr<rcPolyMeshDetail, DetailDeleter> dmesh(rcAllocPolyMeshDetail());
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) {
        return {};
    }
    if (pmesh->nverts >= 0xffff || pmesh->npolys == 0) {
        return {};
    }
    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == RC_WALKABLE_AREA) {
            pmesh->areas[i] = kPolyAreaGround;
        }
        pmesh->flags[i] = kPolyFlagWalk;
    }

    // Off-mesh links are owned by the tile containing their start point.
    std::vector<float> conVerts;
    std::vector<float> conRad;
    std::vector<unsigned short> conFlags;
    std::vector<unsigned char> conAreas;
    std::vector<unsigned char> conDir;
    std::vector<unsigned int> conIds;
    for (std::size_t i = 0; i < links.size(); ++i) {
        const OffMeshLink& l = links[i];
        if (l.start.x < tileMinX || l.start.x >= tileMinX + tileWorld || l.start.z < tileMinZ ||
            l.start.z >= tileMinZ + tileWorld) {
            continue;
        }
        conVerts.insert(conVerts.end(), {l.start.x, l.start.y, l.start.z, l.end.x, l.end.y, l.end.z});
        conRad.push_back(l.radius);
        conFlags.push_back(kPolyFlagWalk);
        conAreas.push_back(kPolyAreaGround);
        conDir.push_back(l.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
        conIds.push_back(static_cast<unsigned int>(i));
    }

    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.offMeshConVerts = conVerts.empty() ? nullptr : conVerts.data();
    params.offMeshConRad = conRad.empty() ? nullptr : conRad.data();
    params.offMeshConFlags = conFlags.empty() ? nullptr : conFlags.data();
    params.offMeshConAreas = conAreas.empty() ? nullptr : conAreas.data();
    params.offMeshConDir = conDir.empty() ? nullptr : conDir.data();
    params.offMeshConUserID = conIds.empty() ? nullptr : conIds.data();
    params.offMeshConCount = static_cast<int>(conRad.size());
    params.walkableHeight = s.agentHeight;
    params.walkableRadius = s.agentRadius;
    params.walkableClimb = s.agentMaxClimb;
    params.tileX = tx;
    params.tileY = ty;
    params.tileLayer = 0;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    TileResult result;
    if (!dtCreateNavMeshData(&params, &result.data, &result.size)) {
        return {};
    }
    return result;
}

}  // namespace

void NavMesh::NavMeshDeleter::operator()(dtNavMesh* p) const noexcept {
    dtFreeNavMesh(p);
}
void NavMesh::QueryDeleter::operator()(dtNavMeshQuery* p) const noexcept {
    dtFreeNavMeshQuery(p);
}

NavMesh::NavMesh() : filter_(std::make_unique<dtQueryFilter>()) {
    filter_->setIncludeFlags(kPolyFlagWalk);
    filter_->setExcludeFlags(0);
}

NavMesh::~NavMesh() = default;

void NavMesh::clear() {
    queries_.clear();
    navMesh_.reset();
    links_.clear();
    polyCount_ = 0;
    tileCount_ = 0;
    tileOffset_.clear();
    denseToRef_.clear();
    polyCenter_.clear();
    offMesh_.clear();
    extraCost_.clear();
    predStart_.clear();
    predList_.clear();
    component_.clear();
    walls_.clear();
    wallGridW_ = 0;
    wallGridH_ = 0;
    wallCellStart_.clear();
    wallEntries_.clear();
    debugTriangles_.clear();
}

bool NavMesh::build(const geometry::TriangleMesh& mesh, std::span<const OffMeshLink> links,
                    const NavBuildSettings& settings, core::JobSystem& jobs, spdlog::logger& log) {
    CS_PROFILE_ZONE();
    clear();
    links_.assign(links.begin(), links.end());
    if (mesh.empty()) {
        return false;
    }

    // Shared, read-only build input.
    BuildInput in;
    in.verts.reserve(mesh.positions.size() * 3);
    for (const glm::vec3& p : mesh.positions) {
        in.verts.insert(in.verts.end(), {p.x, p.y, p.z});
    }
    in.tris.assign(mesh.indices.begin(), mesh.indices.end());
    const std::uint32_t nt = mesh.triangleCount();
    const float walkableCos = std::cos(settings.agentMaxSlopeDegrees * 3.14159265f / 180.0f);
    in.areas.resize(nt);
    in.triMinX.resize(nt);
    in.triMaxX.resize(nt);
    in.triMinZ.resize(nt);
    in.triMaxZ.resize(nt);
    for (std::uint32_t t = 0; t < nt; ++t) {
        const glm::vec3 a = mesh.positions[mesh.indices[3 * t]];
        const glm::vec3 b = mesh.positions[mesh.indices[3 * t + 1]];
        const glm::vec3 c = mesh.positions[mesh.indices[3 * t + 2]];
        const glm::vec3 e0 = b - a;
        const glm::vec3 e1 = c - a;
        glm::vec3 n(e0.y * e1.z - e0.z * e1.y, e0.z * e1.x - e0.x * e1.z, e0.x * e1.y - e0.y * e1.x);
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        const float ny = len > 0.0f ? n.y / len : 0.0f;
        const std::uint8_t flags = t < mesh.triangleFlags.size() ? mesh.triangleFlags[t] : geometry::kSurfaceWalkable;
        const bool walkable = (flags & geometry::kSurfaceWalkable) != 0 && (flags & geometry::kSurfaceObstacle) == 0;
        in.areas[t] = walkable && ny > walkableCos ? RC_WALKABLE_AREA : RC_NULL_AREA;
        in.triMinX[t] = std::min({a.x, b.x, c.x});
        in.triMaxX[t] = std::max({a.x, b.x, c.x});
        in.triMinZ[t] = std::min({a.z, b.z, c.z});
        in.triMaxZ[t] = std::max({a.z, b.z, c.z});
    }
    const geometry::Bounds bounds = mesh.bounds();
    in.bmin[0] = bounds.min.x;
    in.bmin[1] = bounds.min.y - 1.0f;
    in.bmin[2] = bounds.min.z;
    in.bmax[0] = bounds.max.x;
    in.bmax[1] = bounds.max.y + 1.0f;
    in.bmax[2] = bounds.max.z;

    int gw = 0;
    int gh = 0;
    rcCalcGridSize(in.bmin, in.bmax, settings.cellSize, &gw, &gh);
    const int ts = settings.tileSizeCells;
    const int tw = (gw + ts - 1) / ts;
    const int th = (gh + ts - 1) / ts;
    const auto numTiles = static_cast<std::uint32_t>(tw * th);

    // Detour packs tile + poly ids into 22 bits of a 32-bit ref (10 bits salt).
    const auto tileBits =
        std::min<int>(static_cast<int>(std::bit_width(std::bit_ceil(std::max<std::uint32_t>(1, numTiles))) - 1), 14);
    const int polyBits = 22 - tileBits;

    dtNavMeshParams params{};
    rcVcopy(params.orig, in.bmin);
    params.tileWidth = static_cast<float>(ts) * settings.cellSize;
    params.tileHeight = static_cast<float>(ts) * settings.cellSize;
    params.maxTiles = 1 << tileBits;
    params.maxPolys = 1 << polyBits;
    navMesh_.reset(dtAllocNavMesh());
    if (!navMesh_ || dtStatusFailed(navMesh_->init(&params))) {
        log.error("NavMesh: dtNavMesh::init failed ({} tiles)", numTiles);
        clear();
        return false;
    }

    // Rasterise tiles in parallel; add them in index order for determinism.
    std::vector<TileResult> results(numTiles);
    jobs.parallelFor(numTiles, 1, [&](core::IndexRange r, std::uint32_t) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            results[i] = buildTile(in, settings, links_, static_cast<int>(i) % tw, static_cast<int>(i) / tw);
        }
    });
    for (TileResult& t : results) {
        if (t.data == nullptr) {
            continue;
        }
        if (dtStatusFailed(navMesh_->addTile(t.data, t.size, DT_TILE_FREE_DATA, 0, nullptr))) {
            dtFree(t.data);
            continue;
        }
        ++tileCount_;
    }

    queries_.clear();
    for (std::uint32_t i = 0; i < jobs.threadCount(); ++i) {
        std::unique_ptr<dtNavMeshQuery, QueryDeleter> q(dtAllocNavMeshQuery());
        q->init(navMesh_.get(), kMaxQueryNodes);
        queries_.push_back(std::move(q));
    }

    buildIndex();
    buildWalls();
    log.info("NavMesh: {}x{} tiles ({} non-empty), {} polygons, {} wall segments, {} off-mesh links", tw, th,
             tileCount_, polyCount_, walls_.size(), links_.size());
    return polyCount_ > 0;
}

void NavMesh::buildIndex() {
    const dtNavMesh& nav = *navMesh_;
    const int maxTiles = nav.getMaxTiles();
    tileOffset_.assign(static_cast<std::size_t>(maxTiles), 0);
    polyCount_ = 0;
    for (int i = 0; i < maxTiles; ++i) {
        const dtMeshTile* tile = nav.getTile(i);
        tileOffset_[static_cast<std::size_t>(i)] = polyCount_;
        if (tile != nullptr && tile->header != nullptr) {
            polyCount_ += static_cast<std::uint32_t>(tile->header->polyCount);
        }
    }

    denseToRef_.resize(polyCount_);
    polyCenter_.resize(polyCount_);
    offMesh_.assign(polyCount_, 0);
    extraCost_.assign(polyCount_, 0.0f);
    std::vector<std::vector<std::uint32_t>> preds(polyCount_);
    debugTriangles_.clear();

    for (int i = 0; i < maxTiles; ++i) {
        const dtMeshTile* tile = nav.getTile(i);
        if (tile == nullptr || tile->header == nullptr) {
            continue;
        }
        const dtPolyRef base = nav.getPolyRefBase(tile);
        for (int ip = 0; ip < tile->header->polyCount; ++ip) {
            const dtPoly& poly = tile->polys[ip];
            const std::uint32_t dense = tileOffset_[static_cast<std::size_t>(i)] + static_cast<std::uint32_t>(ip);
            denseToRef_[dense] = base | static_cast<dtPolyRef>(ip);

            glm::vec3 c(0.0f);
            for (int v = 0; v < poly.vertCount; ++v) {
                const float* p = &tile->verts[poly.verts[v] * 3];
                c += glm::vec3(p[0], p[1], p[2]);
            }
            polyCenter_[dense] = c / static_cast<float>(std::max<int>(1, poly.vertCount));
            offMesh_[dense] = poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION ? 1 : 0;
            if (offMesh_[dense] != 0) {
                const int conIndex = ip - tile->header->offMeshBase;
                const unsigned int userId = tile->offMeshCons[conIndex].userId;
                extraCost_[dense] = userId < links_.size() ? links_[userId].extraCost : 0.0f;
            }

            for (unsigned int k = poly.firstLink; k != DT_NULL_LINK; k = tile->links[k].next) {
                const dtPolyRef to = tile->links[k].ref;
                if (to != 0) {
                    preds[denseIndex(to)].push_back(dense);
                }
            }

            if (offMesh_[dense] != 0) {
                continue;
            }
            const dtPolyDetail& pd = tile->detailMeshes[ip];
            for (int j = 0; j < pd.triCount; ++j) {
                const unsigned char* t = &tile->detailTris[(pd.triBase + static_cast<unsigned int>(j)) * 4];
                for (int k = 0; k < 3; ++k) {
                    const float* v = t[k] < poly.vertCount
                                         ? &tile->verts[poly.verts[t[k]] * 3]
                                         : &tile->detailVerts[(pd.vertBase + t[k] - poly.vertCount) * 3];
                    debugTriangles_.emplace_back(v[0], v[1] + 0.03f, v[2]);
                }
            }
        }
    }

    predStart_.assign(polyCount_ + 1, 0);
    predList_.clear();
    for (std::uint32_t d = 0; d < polyCount_; ++d) {
        std::sort(preds[d].begin(), preds[d].end());
        preds[d].erase(std::unique(preds[d].begin(), preds[d].end()), preds[d].end());
        predStart_[d + 1] = predStart_[d] + static_cast<std::uint32_t>(preds[d].size());
        predList_.insert(predList_.end(), preds[d].begin(), preds[d].end());
    }

    // Connected components (links treated as undirected). Hollow closed obstacles
    // leave walkable islands inside them; targets there must be rejected.
    std::vector<std::uint32_t> parent(polyCount_);
    for (std::uint32_t d = 0; d < polyCount_; ++d) {
        parent[d] = d;
    }
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (std::uint32_t d = 0; d < polyCount_; ++d) {
        for (std::uint32_t k = predStart_[d]; k < predStart_[d + 1]; ++k) {
            const std::uint32_t a = find(d);
            const std::uint32_t b = find(predList_[k]);
            if (a != b) {
                parent[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    component_.resize(polyCount_);
    for (std::uint32_t d = 0; d < polyCount_; ++d) {
        component_[d] = find(d);
    }
}

void NavMesh::buildWalls() {
    const dtNavMesh& nav = *navMesh_;
    walls_.clear();
    for (int i = 0; i < nav.getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav.getTile(i);
        if (tile == nullptr || tile->header == nullptr) {
            continue;
        }
        for (int ip = 0; ip < tile->header->polyCount; ++ip) {
            const dtPoly& poly = tile->polys[ip];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
                continue;
            }
            for (int j = 0; j < poly.vertCount; ++j) {
                bool connected = false;
                for (unsigned int k = poly.firstLink; k != DT_NULL_LINK && !connected; k = tile->links[k].next) {
                    connected = tile->links[k].edge == j;
                }
                if (connected) {
                    continue;
                }
                const float* a = &tile->verts[poly.verts[j] * 3];
                const float* b = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
                walls_.push_back({{a[0], a[1], a[2]}, {b[0], b[1], b[2]}});
            }
        }
    }

    // Uniform grid over the wall bounds; each wall is listed in every cell its AABB touches.
    wallGridW_ = 0;
    wallGridH_ = 0;
    wallCellStart_.clear();
    wallEntries_.clear();
    if (walls_.empty()) {
        return;
    }
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (const WallSegment& w : walls_) {
        mn = glm::min(mn, glm::min(w.a, w.b));
        mx = glm::max(mx, glm::max(w.a, w.b));
    }
    wallOrigin_ = mn;
    wallGridW_ = static_cast<int>((mx.x - mn.x) / kWallCell) + 1;
    wallGridH_ = static_cast<int>((mx.z - mn.z) / kWallCell) + 1;
    const auto cells = static_cast<std::uint32_t>(wallGridW_ * wallGridH_);
    wallCellStart_.assign(cells + 1, 0);
    auto forCells = [&](const WallSegment& w, auto&& fn) {
        const int x0 = cellX(std::min(w.a.x, w.b.x));
        const int x1 = cellX(std::max(w.a.x, w.b.x));
        const int z0 = cellZ(std::min(w.a.z, w.b.z));
        const int z1 = cellZ(std::max(w.a.z, w.b.z));
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                fn(static_cast<std::uint32_t>(z * wallGridW_ + x));
            }
        }
    };
    for (const WallSegment& w : walls_) {
        forCells(w, [&](std::uint32_t c) { ++wallCellStart_[c + 1]; });
    }
    for (std::uint32_t c = 0; c < cells; ++c) {
        wallCellStart_[c + 1] += wallCellStart_[c];
    }
    wallEntries_.resize(wallCellStart_[cells]);
    std::vector<std::uint32_t> cursor(wallCellStart_.begin(), wallCellStart_.end() - 1);
    for (std::uint32_t i = 0; i < walls_.size(); ++i) {
        forCells(walls_[i], [&](std::uint32_t c) { wallEntries_[cursor[c]++] = i; });
    }
}

std::uint32_t NavMesh::denseIndex(PolyRef ref) const noexcept {
    if (ref == 0 || !navMesh_) {
        return kNoPoly;
    }
    unsigned int salt = 0;
    unsigned int it = 0;
    unsigned int ip = 0;
    navMesh_->decodePolyId(ref, salt, it, ip);
    if (it >= tileOffset_.size()) {
        return kNoPoly;
    }
    return tileOffset_[it] + ip;
}

bool NavMesh::connected(PolyRef a, PolyRef b) const noexcept {
    const std::uint32_t da = denseIndex(a);
    const std::uint32_t db = denseIndex(b);
    return da < polyCount_ && db < polyCount_ && component_[da] == component_[db];
}

std::span<const std::uint32_t> NavMesh::predecessors(std::uint32_t dense) const noexcept {
    return std::span(predList_.data() + predStart_[dense], predStart_[dense + 1] - predStart_[dense]);
}

std::optional<NearestPoly> NavMesh::findNearest(glm::vec3 p, glm::vec3 halfExtents, std::uint32_t threadIndex) const {
    if (!valid()) {
        return std::nullopt;
    }
    const float center[3] = {p.x, p.y, p.z};
    const float ext[3] = {halfExtents.x, halfExtents.y, halfExtents.z};
    dtPolyRef ref = 0;
    float nearest[3];
    const dtStatus st = queries_[threadIndex]->findNearestPoly(center, ext, filter_.get(), &ref, nearest);
    if (dtStatusFailed(st) || ref == 0) {
        return std::nullopt;
    }
    return NearestPoly{ref, {nearest[0], nearest[1], nearest[2]}};
}

std::optional<glm::vec3> NavMesh::closestPointOnPoly(PolyRef ref, glm::vec3 p, std::uint32_t threadIndex) const {
    if (!valid() || ref == 0) {
        return std::nullopt;
    }
    const float pos[3] = {p.x, p.y, p.z};
    float closest[3];
    if (dtStatusFailed(queries_[threadIndex]->closestPointOnPoly(ref, pos, closest, nullptr))) {
        return std::nullopt;
    }
    return glm::vec3(closest[0], closest[1], closest[2]);
}

bool NavMesh::portal(PolyRef from, PolyRef to, glm::vec3& left, glm::vec3& right, std::uint32_t /*threadIndex*/) const {
    // Same logic as dtNavMeshQuery::getPortalPoints (private in Recast 1.6).
    if (!navMesh_) {
        return false;
    }
    const dtMeshTile* fromTile = nullptr;
    const dtPoly* fromPoly = nullptr;
    const dtMeshTile* toTile = nullptr;
    const dtPoly* toPoly = nullptr;
    if (dtStatusFailed(navMesh_->getTileAndPolyByRef(from, &fromTile, &fromPoly)) ||
        dtStatusFailed(navMesh_->getTileAndPolyByRef(to, &toTile, &toPoly))) {
        return false;
    }
    const dtLink* link = nullptr;
    for (unsigned int k = fromPoly->firstLink; k != DT_NULL_LINK; k = fromTile->links[k].next) {
        if (fromTile->links[k].ref == to) {
            link = &fromTile->links[k];
            break;
        }
    }
    if (link == nullptr) {
        return false;
    }
    auto vert = [](const dtMeshTile* tile, unsigned short index) {
        const float* v = &tile->verts[index * 3];
        return glm::vec3(v[0], v[1], v[2]);
    };

    if (fromPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
        left = right = vert(fromTile, fromPoly->verts[link->edge]);
        return true;
    }
    if (toPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
        for (unsigned int k = toPoly->firstLink; k != DT_NULL_LINK; k = toTile->links[k].next) {
            if (toTile->links[k].ref == from) {
                left = right = vert(toTile, toPoly->verts[toTile->links[k].edge]);
                return true;
            }
        }
        return false;
    }

    const glm::vec3 v0 = vert(fromTile, fromPoly->verts[link->edge]);
    const glm::vec3 v1 = vert(fromTile, fromPoly->verts[(link->edge + 1) % fromPoly->vertCount]);
    left = v0;
    right = v1;
    // Tile-border links may cover only part of the edge.
    if (link->side != 0xff && (link->bmin != 0 || link->bmax != 255)) {
        constexpr float kScale = 1.0f / 255.0f;
        left = v0 + (v1 - v0) * (static_cast<float>(link->bmin) * kScale);
        right = v0 + (v1 - v0) * (static_cast<float>(link->bmax) * kScale);
    }
    return true;
}

}  // namespace crowdsim::sim
