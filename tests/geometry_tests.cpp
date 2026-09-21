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

void checkTerrainQueries() {
    HydrologyMap hillside;
    // Runoff sources and the rain collision texture use the actual triangles.
    addPlane(hillside, -4.0F, 0.0F, 4.0F, 2.0F);
    addPlane(hillside, 0.0F, 4.0F, 2.0F, -3.0F);
    indexFixture(hillside);
    for (float x : {-2.0F,-0.2F,0.2F,2.0F}) {
        float height = 0.0F;
        require(terrainHeight(hillside,x,0,kVolumeMax.y,height),"Height query missed hillside");
        const float expected = x < 0 ? 2.0F-0.5F*x : 2.0F-1.25F*x;
        require(std::abs(height-expected) < 0.001F,"Height query missed terrain bend");
    }

    HydrologyMap cliff;
    addPlane(cliff, -4.0F, 0.0F, 4.0F, 4.0F);
    addPlane(cliff, -4.0F, 4.0F, 0.0F, 0.0F);
    indexFixture(cliff);
    float height = 0;
    require(terrainHeight(cliff,-1,0,8,height) && std::abs(height-4) < 0.001F,"Missed upper island");
    require(terrainHeight(cliff,-1,0,3,height) && std::abs(height) < 0.001F,"Missed lower island");
    require(!terrainHit(cliff,Vector3{1,3,0},Vector3{1,2,0}).hit,"Ray hit beyond its segment");
    std::cout << "Terrain queries follow triangle slopes and distinguish stacked surfaces\n";
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
        checkTerrainQueries();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
