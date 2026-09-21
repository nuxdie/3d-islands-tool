#define main islandsApplicationMain
#include "../src/main.cpp"
#undef main

#include <bit>
#include <iostream>
#include <map>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using PointKey = std::array<std::uint32_t, 3>;
PointKey pointKey(Vector3 p) {
    return {std::bit_cast<std::uint32_t>(p.x), std::bit_cast<std::uint32_t>(p.y),
            std::bit_cast<std::uint32_t>(p.z)};
}

void checkClosed(std::uint32_t seed, GenerationSettings settings) {
    HydrologyMap hydrology;
    int islands = 0;
    const auto surface = extractIslands(seed, settings, islands, hydrology);
    require(!surface.empty(), "Empty terrain");
    std::map<std::array<PointKey, 2>, std::pair<int, int>> edges;
    for (std::size_t i = 0; i < surface.size(); i += 3) {
        for (int j = 0; j < 3; ++j) {
            const Vector3 p = surface[i + j].position;
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Nonfinite vertex");
            require(p.x > kVolumeMin.x && p.x < kVolumeMax.x &&
                    p.y > kVolumeMin.y && p.y < kVolumeMax.y &&
                    p.z > kVolumeMin.z && p.z < kVolumeMax.z, "Surface touches volume boundary");
            PointKey a = pointKey(p);
            PointKey b = pointKey(surface[i + (j + 1) % 3].position);
            require(a != b, "Collapsed triangle edge");
            const int direction = a < b ? 1 : -1;
            if (b < a) std::swap(a, b);
            auto& edge = edges[{a, b}];
            ++edge.first;
            edge.second += direction;
        }
    }
    for (const auto& [key, edge] : edges) {
        (void)key;
        require(edge.first == 2, "Open or nonmanifold edge in island mesh");
        require(edge.second == 0, "Inconsistent triangle winding");
    }
    std::cout << "Closed, consistently wound mesh: seed " << seed << ", " << surface.size() / 3 << " triangles\n";
}

void addPlane(HydrologyMap& map, float x0, float x1, float y0, float y1) {
    const Vector3 normal = normalize(Vector3{y0 - y1, x1 - x0, 0.0F});
    const SurfaceVertex a{{x0, y0, -4.0F}, normal};
    const SurfaceVertex b{{x1, y1, -4.0F}, normal};
    const SurfaceVertex c{{x1, y1, 4.0F}, normal};
    const SurfaceVertex d{{x0, y0, 4.0F}, normal};
    addTriangle(map.surface, a, b, c, normal);
    addTriangle(map.surface, a, c, d, normal);
}

void indexFixture(HydrologyMap& map) {
    map.height.assign(kGridX * kGridZ, kVolumeMin.y - 1.0F);
    map.triangles.assign(kGridX * kGridZ, {});
    for (auto& bin : map.triangles) {
        for (std::size_t i = 0; i < map.surface.size(); i += 3) bin.push_back(static_cast<int>(i));
    }
    for (int i = 0; i < kGridX * kGridZ; ++i) {
        const Vector3 p = surfaceCellPosition(map, i);
        terrainHeight(map, p.x, p.z, kVolumeMax.y, map.height[i]);
    }
}

void checkWater() {
    const int cell = (kGridZ / 2) * kGridX + kGridX / 2 - 1;
    HydrologyMap hillside;
    // A bend in the slope between adjacent simulation nodes must be sampled.
    addPlane(hillside, -4.0F, 0.0F, 4.0F, 2.0F);
    addPlane(hillside, 0.0F, 4.0F, 2.0F, -3.0F);
    indexFixture(hillside);
    const auto& river = waterPath(hillside, cell, cell + 1);
    require(river.size() > 6, "River is still a single segment");
    bool bent = false;
    for (const Vector3 p : river) {
        float height = 0.0F;
        require(terrainHeight(hillside, p.x, p.z, kVolumeMax.y, height), "River left hillside");
        require(std::abs(p.y - height - 0.045F) < 0.001F, "River does not conform to triangle surface");
        const float t = (p.x - river.front().x) / (river.back().x - river.front().x);
        bent |= std::abs(p.y - mix(river.front().y, river.back().y, t)) > 0.04F;
    }
    require(bent, "River cuts straight through terrain bend");

    HydrologyMap cliff;
    addPlane(cliff, -4.0F, 0.0F, 4.0F, 4.0F);
    addPlane(cliff, -4.0F, 4.0F, 0.0F, 0.0F);
    indexFixture(cliff);
    // An outlet can fall onto terrain below rather than reaching the void.
    const auto& fall = waterPath(cliff, cell, cell + 1);
    require(fall.size() > 10, "Waterfall has no sampled trajectory");
    require(fall.back().x > 0.5F, "Waterfall is still a vertical cylinder");
    require(std::abs(fall.back().y - 0.045F) < 0.001F, "Waterfall did not stop on lower terrain");
    for (const Vector3 p : fall) require(p.y >= 0.0F, "Waterfall penetrated receiving island");
    const int receiver = surfaceCellAt(cliff, fall.back().x, fall.back().z);
    require(receiver >= 0 && receiver != cell + 1, "Fixture must land beyond adjacent cell");
    cliff.water.assign(kGridX * kGridZ, 0.0F);
    cliff.wetness.assign(kGridX * kGridZ, 0.0F);
    cliff.flow.assign(kGridX * kGridZ, 0.0F);
    cliff.waterfall.assign(kGridX * kGridZ, 0.0F);
    cliff.downstream.assign(kGridX * kGridZ, -1);
    cliff.outlet.assign(kGridX * kGridZ, -1);
    cliff.water[cell] = 0.2F;
    updateHydrology(cliff, 0.016F);
    require(cliff.water[receiver] > 0.0F, "Water was not delivered to waterfall landing");
    float total = 0.0F;
    for (float water : cliff.water) {
        require(water >= 0.0F && std::isfinite(water), "Invalid water quantity");
        total += water;
    }
    require(std::abs(total - 0.2F * std::exp(-0.016F * 0.006F)) < 0.00001F,
            "Water transfer did not conserve volume after evaporation");

    HydrologyMap edge;
    addPlane(edge, -4.0F, 0.0F, 4.0F, 4.0F);
    indexFixture(edge);
    const auto& spill = waterPath(edge, cell, cell + 1);
    require(spill.back().y < kVolumeMin.y, "Unobstructed waterfall did not exit volume");
    require(spill[1].y == spill[0].y, "Waterfall skipped its terrain lip");
    std::cout << "Rivers follow terrain bends; waterfalls follow lips and collide with lower islands\n";
}

int main() {
    try {
        checkClosed(42, {});
        checkClosed(7919, {});
        GenerationSettings extreme;
        extreme.islandCount = 10;
        extreme.islandScale = 1.35F;
        extreme.verticalScale = 1.45F;
        extreme.roughness = 1.25F;
        extreme.caveSize = 0.58F;
        extreme.caveStrength = 1.9F;
        checkClosed(123456, extreme);
        checkWater();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
