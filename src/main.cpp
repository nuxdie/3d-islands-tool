#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kGridX = 88;
constexpr int kGridY = 60;
constexpr int kGridZ = 88;
constexpr Vector3 kVolumeMin{-27.0F, -14.0F, -27.0F};
constexpr Vector3 kVolumeMax{27.0F, 17.0F, 27.0F};
constexpr float kSidebarWidth = 400.0F;
constexpr float kWaterLevelScale = 0.65F;

struct Sample {
    Vector3 position;
    Vector3 normal;
    float density;
};

struct SurfaceVertex {
    Vector3 position;
    Vector3 normal;
};

struct Metaball {
    Vector3 center;
    float radius;
};

struct IslandShape {
    Vector3 center;
    Vector3 radius;
    std::vector<Metaball> lobes;
    float surfaceThreshold;
};

struct GenerationSettings {
    int islandCount = 6;
    float islandScale = 1.0F;
    float verticalScale = 1.0F;
    float roughness = 0.72F;
    float caveSize = 0.34F;
    float caveStrength = 1.36F;
};

struct Cloud {
    Vector3 position;
    float size;
    float phase;
    float rainAccumulator;
    std::uint32_t shapeSeed;
};

struct RainDrop {
    Vector3 position;
    float speed;
    float wind;
};

struct HydrologyMap {
    std::vector<float> height;
    std::vector<float> water;
    std::vector<float> wetness;
    std::vector<float> flow;
    std::vector<float> waterfall;
    std::vector<int> downstream;
    std::vector<int> outlet;
    std::vector<SurfaceVertex> surface;
    std::vector<std::vector<int>> triangles;
    std::unordered_map<int, std::vector<Vector3>> paths;
};

enum class SidebarAction {
    none,
    rebuild,
    newSeed
};

float smoothstep(float value) {
    return value * value * (3.0F - 2.0F * value);
}

float smoothstep(float low, float high, float value) {
    const float amount = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return smoothstep(amount);
}

float mix(float a, float b, float amount) {
    return a + (b - a) * amount;
}

Vector3 mix(const Vector3& a, const Vector3& b, float amount) {
    return Vector3{
        mix(a.x, b.x, amount),
        mix(a.y, b.y, amount),
        mix(a.z, b.z, amount)
    };
}

Vector3 normalize(const Vector3& vector) {
    const float length = std::sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
    if (length < 0.00001F) {
        return Vector3{0.0F, 1.0F, 0.0F};
    }
    return Vector3{vector.x / length, vector.y / length, vector.z / length};
}

float dot(const Vector3& a, const Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 subtract(const Vector3& a, const Vector3& b) {
    return Vector3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector3 cross(const Vector3& a, const Vector3& b) {
    return Vector3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

std::uint32_t hash3d(int x, int y, int z, std::uint32_t seed) {
    std::uint32_t value = static_cast<std::uint32_t>(x) * 0x8da6b343U;
    value ^= static_cast<std::uint32_t>(y) * 0xd8163841U;
    value ^= static_cast<std::uint32_t>(z) * 0xcb1ab31fU;
    value ^= seed * 0x165667b1U;
    value ^= value >> 15U;
    value *= 0x2c1b3c6dU;
    value ^= value >> 12U;
    return value;
}

float randomAt(int x, int y, int z, std::uint32_t seed) {
    return static_cast<float>(hash3d(x, y, z, seed) & 0x00ffffffU) /
           static_cast<float>(0x00ffffffU);
}

float valueNoise3d(float x, float y, float z, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const float tx = smoothstep(x - static_cast<float>(x0));
    const float ty = smoothstep(y - static_cast<float>(y0));
    const float tz = smoothstep(z - static_cast<float>(z0));

    const float x00 = mix(randomAt(x0, y0, z0, seed), randomAt(x0 + 1, y0, z0, seed), tx);
    const float x10 = mix(randomAt(x0, y0 + 1, z0, seed), randomAt(x0 + 1, y0 + 1, z0, seed), tx);
    const float x01 = mix(randomAt(x0, y0, z0 + 1, seed), randomAt(x0 + 1, y0, z0 + 1, seed), tx);
    const float x11 = mix(randomAt(x0, y0 + 1, z0 + 1, seed),
                          randomAt(x0 + 1, y0 + 1, z0 + 1, seed), tx);
    return mix(mix(x00, x10, ty), mix(x01, x11, ty), tz);
}

float fractalNoise3d(float x, float y, float z, std::uint32_t seed, int octaves) {
    float value = 0.0F;
    float amplitude = 0.5F;
    float frequency = 1.0F;
    float amplitudeSum = 0.0F;

    for (int octave = 0; octave < octaves; ++octave) {
        value += valueNoise3d(x * frequency, y * frequency, z * frequency,
                              seed + static_cast<std::uint32_t>(octave) * 1013U) * amplitude;
        amplitudeSum += amplitude;
        amplitude *= 0.5F;
        frequency *= 2.01F;
    }
    return value / amplitudeSum;
}

float cellularDistance3d(float x, float y, float z, std::uint32_t seed) {
    const int baseX = static_cast<int>(std::floor(x));
    const int baseY = static_cast<int>(std::floor(y));
    const int baseZ = static_cast<int>(std::floor(z));
    float nearestSquared = 4.0F;

    for (int offsetZ = -1; offsetZ <= 1; ++offsetZ) {
        for (int offsetY = -1; offsetY <= 1; ++offsetY) {
            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                const int cellX = baseX + offsetX;
                const int cellY = baseY + offsetY;
                const int cellZ = baseZ + offsetZ;
                const float featureX = static_cast<float>(cellX) + randomAt(cellX, cellY, cellZ, seed);
                const float featureY = static_cast<float>(cellY) + randomAt(cellX, cellY, cellZ, seed + 313U);
                const float featureZ = static_cast<float>(cellZ) + randomAt(cellX, cellY, cellZ, seed + 619U);
                const float dx = x - featureX;
                const float dy = y - featureY;
                const float dz = z - featureZ;
                nearestSquared = std::min(nearestSquared, dx * dx + dy * dy + dz * dz);
            }
        }
    }
    return std::sqrt(nearestSquared);
}

std::vector<IslandShape> createIslandConfiguration(std::uint32_t seed,
                                                   const GenerationSettings& settings) {
    std::mt19937 engine(seed);
    std::uniform_real_distribution<float> xzDistribution(-15.0F, 15.0F);
    std::uniform_real_distribution<float> yDistribution(-5.0F, 8.0F);
    std::uniform_real_distribution<float> horizontalRadius(3.8F, 8.5F);
    std::uniform_real_distribution<float> verticalRadius(2.7F, 6.2F);
    std::uniform_int_distribution<int> lobeCountDistribution(4, 9);
    std::uniform_real_distribution<float> offsetDistribution(-1.0F, 1.0F);
    std::uniform_real_distribution<float> lobeRadiusDistribution(0.2F, 0.46F);
    std::uniform_real_distribution<float> thresholdDistribution(0.9F, 1.18F);
    const int desiredCount = settings.islandCount;

    std::vector<IslandShape> islands;
    islands.reserve(static_cast<std::size_t>(desiredCount));
    for (int islandIndex = 0; islandIndex < desiredCount; ++islandIndex) {
        IslandShape candidate{};
        for (int attempt = 0; attempt < 80; ++attempt) {
            candidate = IslandShape{
                Vector3{xzDistribution(engine), yDistribution(engine), xzDistribution(engine)},
                Vector3{
                    horizontalRadius(engine) * settings.islandScale,
                    verticalRadius(engine) * settings.islandScale * settings.verticalScale,
                    horizontalRadius(engine) * settings.islandScale
                },
                {},
                thresholdDistribution(engine)
            };

            bool hasSpace = true;
            for (const IslandShape& existing : islands) {
                const float dx = (candidate.center.x - existing.center.x) /
                                 (candidate.radius.x + existing.radius.x);
                const float dy = (candidate.center.y - existing.center.y) /
                                 (candidate.radius.y + existing.radius.y);
                const float dz = (candidate.center.z - existing.center.z) /
                                 (candidate.radius.z + existing.radius.z);
                if (std::sqrt(dx * dx + dy * dy + dz * dz) < 0.68F) {
                    hasSpace = false;
                    break;
                }
            }

            if (hasSpace) {
                break;
            }
        }

        const float averageRadius = (candidate.radius.x + candidate.radius.z) * 0.5F;
        const int lobeCount = lobeCountDistribution(engine);
        candidate.lobes.reserve(static_cast<std::size_t>(lobeCount));
        candidate.lobes.push_back(Metaball{candidate.center, averageRadius * 0.43F});
        for (int lobe = 1; lobe < lobeCount; ++lobe) {
            candidate.lobes.push_back(Metaball{
                Vector3{
                    candidate.center.x + offsetDistribution(engine) * candidate.radius.x * 0.62F,
                    candidate.center.y + offsetDistribution(engine) * candidate.radius.y * 0.58F,
                    candidate.center.z + offsetDistribution(engine) * candidate.radius.z * 0.62F
                },
                averageRadius * lobeRadiusDistribution(engine)
            });
        }
        islands.push_back(candidate);
    }
    return islands;
}

std::vector<Cloud> createClouds(std::uint32_t seed, const GenerationSettings& settings) {
    const std::vector<IslandShape> islands = createIslandConfiguration(seed, settings);
    std::mt19937 engine(seed ^ 0xa341316cU);
    std::uniform_real_distribution<float> offset(-1.8F, 1.8F);
    std::uniform_real_distribution<float> sizeVariation(0.85F, 1.2F);
    std::uniform_real_distribution<float> phase(0.0F, 6.28318F);

    std::vector<Cloud> clouds;
    clouds.reserve(islands.size());
    for (std::size_t index = 0; index < islands.size(); ++index) {
        const IslandShape& island = islands[index];
        const float horizontalSize = (island.radius.x + island.radius.z) * 0.22F;
        clouds.push_back(Cloud{
            Vector3{
                island.center.x + offset(engine),
                std::clamp(island.center.y + island.radius.y + 12.0F, 20.0F, 30.0F),
                island.center.z + offset(engine)
            },
            std::clamp(horizontalSize * sizeVariation(engine), 2.2F, 4.8F),
            phase(engine),
            0.0F,
            seed + static_cast<std::uint32_t>(index) * 2654435761U
        });
    }
    return clouds;
}

float densityAt(const Vector3& point, std::uint32_t seed,
                const std::vector<IslandShape>& islands,
                const GenerationSettings& settings) {
    float island = -1000.0F;
    for (const IslandShape& shape : islands) {
        float field = 0.0F;
        for (const Metaball& lobe : shape.lobes) {
            const float dx = point.x - lobe.center.x;
            const float dy = point.y - lobe.center.y;
            const float dz = point.z - lobe.center.z;
            const float radiusSquared = lobe.radius * lobe.radius;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            field += radiusSquared / (distanceSquared + radiusSquared * 0.08F);
        }
        island = std::max(island, field - shape.surfaceThreshold);
    }

    const float rock = fractalNoise3d(point.x * 0.105F + 8.0F,
                                      point.y * 0.12F - 3.0F,
                                      point.z * 0.105F + 13.0F, seed, 4) - 0.5F;
    const float caveScale = 0.135F;
    const float caveDistance = cellularDistance3d(point.x * caveScale - 21.0F,
                                                   point.y * caveScale + 7.0F,
                                                   point.z * caveScale - 4.0F, seed + 7919U);
    const float caveRoughness = fractalNoise3d(point.x * 0.24F, point.y * 0.24F,
                                               point.z * 0.24F, seed + 104729U, 3) - 0.5F;
    const float caveRadius = settings.caveSize + caveRoughness * 0.16F;
    const float caveCut = (1.0F - smoothstep(caveRadius, caveRadius + 0.13F, caveDistance)) *
                          settings.caveStrength;
    // Fade to air before the sampling boundary so even large islands close.
    const float boundary = std::min({point.x - kVolumeMin.x, kVolumeMax.x - point.x,
                                     point.y - kVolumeMin.y, kVolumeMax.y - point.y,
                                     point.z - kVolumeMin.z, kVolumeMax.z - point.z});
    const float field = island + rock * settings.roughness - caveCut;
    const float limit = (boundary - 0.8F) * 0.7F;
    const float blend = std::max(0.6F - std::abs(field - limit), 0.0F) / 0.6F;
    return std::min(field, limit) - blend * blend * 0.15F;
}

Color surfaceColor(const SurfaceVertex& vertex) {
    const auto blend = [](Color a, Color b, float t) {
        return Color{static_cast<unsigned char>(mix(a.r, b.r, t)),
                     static_cast<unsigned char>(mix(a.g, b.g, t)),
                     static_cast<unsigned char>(mix(a.b, b.b, t)), 255};
    };
    const float upward = vertex.normal.y;
    Color base = blend(Color{76, 60, 55, 255}, Color{119, 101, 82, 255},
                       smoothstep(-0.65F, -0.25F, upward));
    const Color grass = blend(Color{63, 121, 73, 255}, Color{168, 178, 151, 255},
                              smoothstep(6.0F, 8.0F, vertex.position.y));
    base = blend(base, grass, smoothstep(0.35F, 0.7F, upward) * smoothstep(-2.0F, -1.0F, vertex.position.y));

    const Vector3 light = normalize(Vector3{-0.55F, 0.78F, -0.3F});
    const float diffuse = std::clamp(dot(vertex.normal, light), 0.0F, 1.0F);
    const float brightness = 0.45F + diffuse * 0.55F;
    return Color{
        static_cast<unsigned char>(static_cast<float>(base.r) * brightness),
        static_cast<unsigned char>(static_cast<float>(base.g) * brightness),
        static_cast<unsigned char>(static_cast<float>(base.b) * brightness),
        255
    };
}

SurfaceVertex interpolateSurface(const Sample& a, const Sample& b) {
    // Use the same arithmetic order on every shared edge.
    if (a.position.x > b.position.x ||
        (a.position.x == b.position.x && a.position.y > b.position.y) ||
        (a.position.x == b.position.x && a.position.y == b.position.y && a.position.z > b.position.z)) {
        return interpolateSurface(b, a);
    }
    const float amount = std::clamp(a.density / (a.density - b.density), 0.0F, 1.0F);
    return SurfaceVertex{
        mix(a.position, b.position, amount),
        normalize(mix(a.normal, b.normal, amount))
    };
}

void addTriangle(std::vector<SurfaceVertex>& vertices,
                 SurfaceVertex a, SurfaceVertex b, SurfaceVertex c, const Vector3& outward) {
    const Vector3 faceNormal = cross(subtract(b.position, a.position), subtract(c.position, a.position));
    // Topology, not smoothed shading normals, determines the visible side.
    if (dot(faceNormal, outward) < 0.0F) {
        std::swap(b, c);
    }
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
}

void polygonizeTetrahedron(const std::array<const Sample*, 4>& tetra,
                           std::vector<SurfaceVertex>& vertices) {
    std::array<int, 4> inside{};
    std::array<int, 4> outside{};
    int insideCount = 0;
    int outsideCount = 0;
    for (int index = 0; index < 4; ++index) {
        if (tetra[static_cast<std::size_t>(index)]->density >= 0.0F) {
            inside[static_cast<std::size_t>(insideCount++)] = index;
        } else {
            outside[static_cast<std::size_t>(outsideCount++)] = index;
        }
    }

    if (insideCount == 0 || insideCount == 4) {
        return;
    }
    const Vector3 outward = subtract(tetra[outside[0]]->position, tetra[inside[0]]->position);
    if (insideCount == 1 || insideCount == 3) {
        const bool singleInside = insideCount == 1;
        const int anchor = singleInside ? inside[0] : outside[0];
        const auto& others = singleInside ? outside : inside;
        addTriangle(vertices,
                    interpolateSurface(*tetra[static_cast<std::size_t>(anchor)], *tetra[static_cast<std::size_t>(others[0])]),
                    interpolateSurface(*tetra[static_cast<std::size_t>(anchor)], *tetra[static_cast<std::size_t>(others[1])]),
                    interpolateSurface(*tetra[static_cast<std::size_t>(anchor)], *tetra[static_cast<std::size_t>(others[2])]), outward);
        return;
    }

    const Sample& a = *tetra[static_cast<std::size_t>(inside[0])];
    const Sample& b = *tetra[static_cast<std::size_t>(inside[1])];
    const Sample& c = *tetra[static_cast<std::size_t>(outside[0])];
    const Sample& d = *tetra[static_cast<std::size_t>(outside[1])];
    const SurfaceVertex ac = interpolateSurface(a, c);
    const SurfaceVertex ad = interpolateSurface(a, d);
    const SurfaceVertex bc = interpolateSurface(b, c);
    const SurfaceVertex bd = interpolateSurface(b, d);
    addTriangle(vertices, ac, ad, bd, outward);
    addTriangle(vertices, ac, bd, bc, outward);
}

std::vector<SurfaceVertex> extractIslands(std::uint32_t seed, const GenerationSettings& settings,
                                         int& islandCount, HydrologyMap& hydrology) {
    const auto sampleIndex = [](int x, int y, int z) {
        return static_cast<std::size_t>((z * kGridY + y) * kGridX + x);
    };
    const Vector3 step{
        (kVolumeMax.x - kVolumeMin.x) / static_cast<float>(kGridX - 1),
        (kVolumeMax.y - kVolumeMin.y) / static_cast<float>(kGridY - 1),
        (kVolumeMax.z - kVolumeMin.z) / static_cast<float>(kGridZ - 1)
    };
    const std::vector<IslandShape> islands = createIslandConfiguration(seed, settings);
    islandCount = static_cast<int>(islands.size());

    std::vector<Sample> samples(static_cast<std::size_t>(kGridX * kGridY * kGridZ));
    for (int z = 0; z < kGridZ; ++z) {
        for (int y = 0; y < kGridY; ++y) {
            for (int x = 0; x < kGridX; ++x) {
                Sample& sample = samples[sampleIndex(x, y, z)];
                sample.position = Vector3{
                    kVolumeMin.x + static_cast<float>(x) * step.x,
                    kVolumeMin.y + static_cast<float>(y) * step.y,
                    kVolumeMin.z + static_cast<float>(z) * step.z
                };
                sample.density = densityAt(sample.position, seed, islands, settings);
            }
        }
    }

    const std::size_t surfaceCellCount = static_cast<std::size_t>(kGridX * kGridZ);
    hydrology.height.assign(surfaceCellCount, kVolumeMin.y - 1.0F);
    hydrology.water.assign(surfaceCellCount, 0.0F);
    hydrology.wetness.assign(surfaceCellCount, 0.0F);
    hydrology.flow.assign(surfaceCellCount, 0.0F);
    hydrology.waterfall.assign(surfaceCellCount, 0.0F);
    hydrology.downstream.assign(surfaceCellCount, -1);
    hydrology.outlet.assign(surfaceCellCount, -1);
    hydrology.paths.clear();
    for (int z = 0; z < kGridZ; ++z) {
        for (int x = 0; x < kGridX; ++x) {
            const std::size_t cell = static_cast<std::size_t>(z * kGridX + x);
            for (int y = kGridY - 2; y >= 0; --y) {
                const Sample& lower = samples[sampleIndex(x, y, z)];
                const Sample& upper = samples[sampleIndex(x, y + 1, z)];
                if (lower.density >= 0.0F && upper.density < 0.0F) {
                    const float amount = lower.density / (lower.density - upper.density);
                    hydrology.height[cell] = mix(lower.position.y, upper.position.y, amount);
                    break;
                }
            }
        }
    }

    for (int z = 0; z < kGridZ; ++z) {
        for (int y = 0; y < kGridY; ++y) {
            for (int x = 0; x < kGridX; ++x) {
                const int left = std::max(0, x - 1);
                const int right = std::min(kGridX - 1, x + 1);
                const int down = std::max(0, y - 1);
                const int up = std::min(kGridY - 1, y + 1);
                const int back = std::max(0, z - 1);
                const int front = std::min(kGridZ - 1, z + 1);
                const float dx = samples[sampleIndex(right, y, z)].density - samples[sampleIndex(left, y, z)].density;
                const float dy = samples[sampleIndex(x, up, z)].density - samples[sampleIndex(x, down, z)].density;
                const float dz = samples[sampleIndex(x, y, front)].density - samples[sampleIndex(x, y, back)].density;
                samples[sampleIndex(x, y, z)].normal = normalize(Vector3{
                    -dx / (static_cast<float>(right - left) * step.x),
                    -dy / (static_cast<float>(up - down) * step.y),
                    -dz / (static_cast<float>(front - back) * step.z)});
            }
        }
    }

    constexpr std::array<std::array<int, 3>, 8> kCorners{{
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}
    }};
    constexpr std::array<std::array<int, 4>, 6> kTetrahedra{{
        {0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6},
        {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}
    }};

    std::vector<SurfaceVertex> surface;
    surface.reserve(300000);
    for (int z = 0; z < kGridZ - 1; ++z) {
        for (int y = 0; y < kGridY - 1; ++y) {
            for (int x = 0; x < kGridX - 1; ++x) {
                std::array<const Sample*, 8> cube{};
                for (std::size_t corner = 0; corner < kCorners.size(); ++corner) {
                    cube[corner] = &samples[sampleIndex(
                        x + kCorners[corner][0], y + kCorners[corner][1], z + kCorners[corner][2])];
                }
                for (const auto& indices : kTetrahedra) {
                    polygonizeTetrahedron(
                        {cube[static_cast<std::size_t>(indices[0])], cube[static_cast<std::size_t>(indices[1])],
                         cube[static_cast<std::size_t>(indices[2])], cube[static_cast<std::size_t>(indices[3])]},
                        surface);
                }
            }
        }
    }

    hydrology.surface = surface;
    hydrology.triangles.assign(surfaceCellCount, {});
    for (std::size_t i = 0; i < surface.size(); i += 3) {
        const Vector3 a = surface[i].position;
        const Vector3 b = surface[i + 1].position;
        const Vector3 c = surface[i + 2].position;
        const int minX = std::clamp(static_cast<int>((std::min({a.x, b.x, c.x}) - kVolumeMin.x) / step.x), 0, kGridX - 2);
        const int maxX = std::clamp(static_cast<int>((std::max({a.x, b.x, c.x}) - kVolumeMin.x) / step.x), 0, kGridX - 2);
        const int minZ = std::clamp(static_cast<int>((std::min({a.z, b.z, c.z}) - kVolumeMin.z) / step.z), 0, kGridZ - 2);
        const int maxZ = std::clamp(static_cast<int>((std::max({a.z, b.z, c.z}) - kVolumeMin.z) / step.z), 0, kGridZ - 2);
        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                hydrology.triangles[z * kGridX + x].push_back(static_cast<int>(i));
            }
        }
    }
    return surface;
}

Model createIslands(std::uint32_t seed, const GenerationSettings& settings,
                    int& triangleCount, int& islandCount, HydrologyMap& hydrology,
                    std::vector<unsigned char>& baseColors) {
    const auto surface = extractIslands(seed, settings, islandCount, hydrology);
    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(surface.size());
    mesh.triangleCount = mesh.vertexCount / 3;
    mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned int>(mesh.vertexCount * 3 * sizeof(float))));
    mesh.normals = static_cast<float*>(MemAlloc(static_cast<unsigned int>(mesh.vertexCount * 3 * sizeof(float))));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(mesh.vertexCount * 4 * sizeof(unsigned char))));

    for (int index = 0; index < mesh.vertexCount; ++index) {
        const SurfaceVertex& vertex = surface[static_cast<std::size_t>(index)];
        const Color color = surfaceColor(vertex);
        mesh.vertices[index * 3] = vertex.position.x;
        mesh.vertices[index * 3 + 1] = vertex.position.y;
        mesh.vertices[index * 3 + 2] = vertex.position.z;
        mesh.normals[index * 3] = vertex.normal.x;
        mesh.normals[index * 3 + 1] = vertex.normal.y;
        mesh.normals[index * 3 + 2] = vertex.normal.z;
        mesh.colors[index * 4] = color.r;
        mesh.colors[index * 4 + 1] = color.g;
        mesh.colors[index * 4 + 2] = color.b;
        mesh.colors[index * 4 + 3] = color.a;
    }

    triangleCount = mesh.triangleCount;
    baseColors.assign(mesh.colors, mesh.colors + mesh.vertexCount * 4);
    UploadMesh(&mesh, false);
    return LoadModelFromMesh(mesh);
}

void drawInterface(std::uint32_t seed, int islandCount, int triangleCount) {
    DrawRectangleRounded(Rectangle{20.0F, 20.0F, 338.0F, 118.0F}, 0.12F, 8,
                         Fade(Color{8, 14, 24, 255}, 0.84F));
    DrawText("VOLUMETRIC NOISE ISLANDS", 36, 34, 20, Color{231, 222, 191, 255});
    DrawText(TextFormat("SEED  %u", seed), 36, 62, 16, Color{127, 195, 183, 255});
    DrawText(TextFormat("%d ISLANDS  |  %d TRIANGLES  |  3D CAVES", islandCount, triangleCount),
             36, 85, 12, Color{153, 170, 174, 255});
    DrawText("DRAG orbit   WHEEL zoom   SPACE new seed", 36, 108, 12, Color{190, 204, 201, 255});
}

bool drawSlider(const char* label, const char* description, float& value,
                float minimum, float maximum, float wheelStep,
                float x, float y, const char* valueFormat) {
    const float width = kSidebarWidth - 48.0F;
    const Rectangle hitArea{x, y + 32.0F, width, 24.0F};
    const Vector2 mouse = GetMousePosition();
    bool changed = false;
    if (CheckCollisionPointRec(mouse, hitArea) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const float nextValue = minimum +
            std::clamp((mouse.x - x) / width, 0.0F, 1.0F) * (maximum - minimum);
        changed = std::abs(nextValue - value) > 0.0001F;
        value = nextValue;
    }
    if (CheckCollisionPointRec(mouse, hitArea)) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0F) {
            const float nextValue = std::clamp(value + wheel * wheelStep, minimum, maximum);
            changed |= std::abs(nextValue - value) > 0.0001F;
            value = nextValue;
        }
    }

    DrawText(label, static_cast<int>(x), static_cast<int>(y), 13, Color{181, 198, 201, 255});
    const char* valueText = TextFormat(valueFormat, value);
    DrawText(valueText, static_cast<int>(x + width - static_cast<float>(MeasureText(valueText, 13))),
             static_cast<int>(y), 13, Color{116, 202, 183, 255});
    DrawText(description, static_cast<int>(x), static_cast<int>(y + 18.0F), 10,
             Color{105, 126, 133, 255});
    DrawRectangleRounded(Rectangle{x, y + 41.0F, width, 4.0F}, 1.0F, 4, Color{47, 68, 76, 255});
    const float amount = (value - minimum) / (maximum - minimum);
    DrawRectangleRounded(Rectangle{x, y + 41.0F, width * amount, 4.0F}, 1.0F, 4,
                         Color{72, 166, 149, 255});
    DrawCircleV(Vector2{x + width * amount, y + 43.0F}, 6.0F,
                CheckCollisionPointRec(mouse, hitArea) ? Color{229, 220, 184, 255}
                                                       : Color{154, 210, 194, 255});
    return changed;
}

bool drawButton(const Rectangle& bounds, const char* label, bool emphasized = false) {
    const bool hovered = CheckCollisionPointRec(GetMousePosition(), bounds);
    const Color base = emphasized ? Color{53, 135, 121, 255} : Color{31, 52, 62, 255};
    const Color hover = emphasized ? Color{69, 158, 140, 255} : Color{43, 68, 79, 255};
    DrawRectangleRounded(bounds, 0.16F, 6, hovered ? hover : base);
    DrawRectangleRoundedLinesEx(bounds, 0.16F, 6, 1.0F, Color{91, 130, 133, 255});
    DrawText(label,
             static_cast<int>(bounds.x + (bounds.width - static_cast<float>(MeasureText(label, 13))) * 0.5F),
             static_cast<int>(bounds.y + 11.0F), 13, Color{223, 226, 211, 255});
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

SidebarAction drawSidebar(GenerationSettings& settings, std::uint32_t seed,
                          bool wireframe, bool autoRotate) {
    const float panelX = static_cast<float>(GetScreenWidth()) - kSidebarWidth;
    const float controlX = panelX + 24.0F;
    DrawRectangle(static_cast<int>(panelX), 0, static_cast<int>(kSidebarWidth), GetScreenHeight(),
                  Color{9, 18, 27, 247});
    DrawRectangle(static_cast<int>(panelX), 0, 1, GetScreenHeight(), Color{65, 99, 106, 255});
    DrawText("GENERATOR", static_cast<int>(controlX), 22, 22, Color{231, 222, 191, 255});
    DrawText(TextFormat("SEED %u", seed), static_cast<int>(controlX), 49, 12,
             Color{102, 158, 155, 255});

    bool settingsChanged = false;
    float count = static_cast<float>(settings.islandCount);
    if (drawSlider("ISLAND COUNT", "Number of independent organic clusters", count,
                   1.0F, 10.0F, 1.0F, controlX, 76.0F, "%.0f")) {
        const int nextCount = static_cast<int>(std::round(count));
        settingsChanged = nextCount != settings.islandCount;
        settings.islandCount = nextCount;
    }
    settingsChanged |= drawSlider("ISLAND SCALE", "Overall width and depth of each island",
                                  settings.islandScale, 0.55F, 1.35F, 0.02F,
                                  controlX, 140.0F, "%.2f");
    settingsChanged |= drawSlider("VERTICAL SCALE", "Stretches or flattens island height",
                                  settings.verticalScale, 0.55F, 1.45F, 0.02F,
                                  controlX, 204.0F, "%.2f");
    settingsChanged |= drawSlider("SURFACE ROUGHNESS", "Strength of the 3D rock deformation",
                                  settings.roughness, 0.0F, 1.25F, 0.03F,
                                  controlX, 268.0F, "%.2f");
    settingsChanged |= drawSlider("CAVE SIZE", "Radius of the cellular cave chambers",
                                  settings.caveSize, 0.12F, 0.58F, 0.01F,
                                  controlX, 332.0F, "%.2f");
    settingsChanged |= drawSlider("CAVE STRENGTH", "How deeply caves cut through the rock",
                                  settings.caveStrength, 0.25F, 1.9F, 0.03F,
                                  controlX, 396.0F, "%.2f");

    DrawText("DRAG OR SCROLL - MESH UPDATES LIVE", static_cast<int>(controlX), 461, 11,
             Color{91, 151, 139, 255});

    SidebarAction action = settingsChanged ? SidebarAction::rebuild : SidebarAction::none;
    if (drawButton(Rectangle{controlX, 482.0F, kSidebarWidth - 48.0F, 36.0F},
                   "NEW RANDOM SEED", true)) {
        action = SidebarAction::newSeed;
    }

    const char* mode = wireframe ? "WIREFRAME" : "SOLID";
    const char* motion = autoRotate ? "AUTO" : "MANUAL";
    DrawText(TextFormat("W  %s", mode), static_cast<int>(controlX), 538, 12,
             Color{160, 179, 181, 255});
    DrawText(TextFormat("R  ROTATION %s", motion), static_cast<int>(controlX + 112.0F), 538, 12,
             Color{160, 179, 181, 255});
    return action;
}

void drawStars() {
    std::uint32_t state = 0x12345678U;
    for (int index = 0; index < 90; ++index) {
        state = state * 1664525U + 1013904223U;
        const int x = static_cast<int>(state % static_cast<std::uint32_t>(GetScreenWidth()));
        state = state * 1664525U + 1013904223U;
        const int y = static_cast<int>(state % static_cast<std::uint32_t>(GetScreenHeight()));
        const unsigned char alpha = static_cast<unsigned char>(70U + state % 130U);
        DrawCircle(x, y, index % 11 == 0 ? 1.5F : 0.8F, Color{178, 205, 217, alpha});
    }
}

float cloudHeight(const Cloud& cloud) {
    return cloud.position.y + std::sin(cloud.phase) * 0.22F;
}

int surfaceCellAt(const HydrologyMap& hydrology, float worldX, float worldZ) {
    const float normalizedX = (worldX - kVolumeMin.x) / (kVolumeMax.x - kVolumeMin.x);
    const float normalizedZ = (worldZ - kVolumeMin.z) / (kVolumeMax.z - kVolumeMin.z);
    if (normalizedX < 0.0F || normalizedX > 1.0F || normalizedZ < 0.0F || normalizedZ > 1.0F) {
        return -1;
    }
    const int x = std::clamp(static_cast<int>(std::round(normalizedX * static_cast<float>(kGridX - 1))),
                             0, kGridX - 1);
    const int z = std::clamp(static_cast<int>(std::round(normalizedZ * static_cast<float>(kGridZ - 1))),
                             0, kGridZ - 1);
    const int cell = z * kGridX + x;
    return hydrology.height[static_cast<std::size_t>(cell)] > kVolumeMin.y ? cell : -1;
}

Vector3 surfaceCellPosition(const HydrologyMap& hydrology, int cell) {
    const int x = cell % kGridX;
    const int z = cell / kGridX;
    return Vector3{
        mix(kVolumeMin.x, kVolumeMax.x, static_cast<float>(x) / static_cast<float>(kGridX - 1)),
        hydrology.height[static_cast<std::size_t>(cell)],
        mix(kVolumeMin.z, kVolumeMax.z, static_cast<float>(z) / static_cast<float>(kGridZ - 1))
    };
}

RayCollision terrainHit(const HydrologyMap& hydrology, const Vector3& start, const Vector3& end) {
    const Vector3 delta = subtract(end, start);
    const float length = std::sqrt(dot(delta, delta));
    RayCollision closest{};
    closest.distance = length + 0.0001F;
    if (length < 0.00001F) return closest;
    const float dx = (kVolumeMax.x - kVolumeMin.x) / static_cast<float>(kGridX - 1);
    const float dz = (kVolumeMax.z - kVolumeMin.z) / static_cast<float>(kGridZ - 1);
    if (std::max(start.x, end.x) < kVolumeMin.x || std::min(start.x, end.x) > kVolumeMax.x ||
        std::max(start.z, end.z) < kVolumeMin.z || std::min(start.z, end.z) > kVolumeMax.z) return closest;
    const int minX = std::clamp(static_cast<int>(std::floor((std::min(start.x, end.x) - kVolumeMin.x) / dx)), 0, kGridX - 2);
    const int maxX = std::clamp(static_cast<int>(std::floor((std::max(start.x, end.x) - kVolumeMin.x) / dx)), 0, kGridX - 2);
    const int minZ = std::clamp(static_cast<int>(std::floor((std::min(start.z, end.z) - kVolumeMin.z) / dz)), 0, kGridZ - 2);
    const int maxZ = std::clamp(static_cast<int>(std::floor((std::max(start.z, end.z) - kVolumeMin.z) / dz)), 0, kGridZ - 2);
    const Ray ray{start, normalize(delta)};
    for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
            for (int triangle : hydrology.triangles[z * kGridX + x]) {
                const RayCollision hit = GetRayCollisionTriangle(ray,
                    hydrology.surface[triangle].position,
                    hydrology.surface[triangle + 1].position,
                    hydrology.surface[triangle + 2].position);
                if (hit.hit && hit.distance < closest.distance) closest = hit;
            }
        }
    }
    return closest;
}

bool terrainHeight(const HydrologyMap& hydrology, float x, float z, float ceiling, float& height) {
    const RayCollision hit = terrainHit(hydrology, Vector3{x, ceiling, z}, Vector3{x, kVolumeMin.y - 1.0F, z});
    if (!hit.hit) return false;
    height = hit.point.y;
    return true;
}

// Cache finely sampled terrain paths, including ballistic motion after a lip.
const std::vector<Vector3>& waterPath(HydrologyMap& hydrology, int cell, int destination) {
    const int key = cell * (kGridX * kGridZ) + destination;
    auto [entry, inserted] = hydrology.paths.try_emplace(key);
    if (!inserted) return entry->second;
    auto& path = entry->second;
    Vector3 position = surfaceCellPosition(hydrology, cell);
    const Vector3 target = surfaceCellPosition(hydrology, destination);
    const Vector3 direction = normalize(Vector3{target.x - position.x, 0.0F, target.z - position.z});
    const bool edge = hydrology.height[destination] <= kVolumeMin.y;
    position.y += 0.045F;
    path.push_back(position);
    Vector3 velocity{direction.x * 1.8F, 0.0F, direction.z * 1.8F};
    bool airborne = false;
    constexpr float dt = 0.035F;
    for (int step = 0; step < 400; ++step) {
        Vector3 next{position.x + velocity.x * dt, position.y, position.z + velocity.z * dt};
        if (!airborne) {
            const float remaining = dot(subtract(target, position), direction);
            if (!edge && remaining <= 0.065F && std::abs(position.y - target.y) < 0.38F) {
                Vector3 end = target;
                end.y += 0.045F;
                path.push_back(end);
                break;
            }
            float height = 0.0F;
            if (terrainHeight(hydrology, next.x, next.z, position.y + 0.25F, height) &&
                position.y - height < 0.38F) {
                next.y = height + 0.045F;
            } else {
                airborne = true;
            }
        }
        if (airborne) {
            velocity.y -= 9.81F * dt;
            next.y = position.y + velocity.y * dt;
            const RayCollision hit = terrainHit(hydrology, position, next);
            if (hit.hit) {
                path.push_back(Vector3{hit.point.x, hit.point.y + 0.045F, hit.point.z});
                break;
            }
        }
        path.push_back(next);
        position = next;
        if (position.y < kVolumeMin.y) break;
    }
    return path;
}

void updateWeather(std::vector<Cloud>& clouds, std::vector<RainDrop>& drops,
                   std::mt19937& engine, HydrologyMap& hydrology, float deltaTime) {
    std::uniform_real_distribution<float> unit(-1.0F, 1.0F);
    std::uniform_real_distribution<float> fallSpeed(10.0F, 16.0F);
    std::uniform_real_distribution<float> wind(0.25F, 0.65F);

    for (Cloud& cloud : clouds) {
        cloud.phase += deltaTime * 0.55F;
        cloud.rainAccumulator += deltaTime * (18.0F + cloud.size * 4.0F);
        while (cloud.rainAccumulator >= 1.0F && drops.size() < 900U) {
            float x = 0.0F;
            float z = 0.0F;
            do {
                x = unit(engine);
                z = unit(engine);
            } while (x * x + z * z > 1.0F);

            drops.push_back(RainDrop{
                Vector3{
                    cloud.position.x + x * cloud.size * 1.45F,
                    cloudHeight(cloud) - cloud.size * 0.42F,
                    cloud.position.z + z * cloud.size
                },
                fallSpeed(engine),
                wind(engine)
            });
            cloud.rainAccumulator -= 1.0F;
        }
    }

    for (RainDrop& drop : drops) {
        const float previousY = drop.position.y;
        drop.position.x += drop.wind * deltaTime;
        drop.position.y -= drop.speed * deltaTime;
        const int cell = surfaceCellAt(hydrology, drop.position.x, drop.position.z);
        if (cell >= 0) {
            float surface = 0.0F;
            if (terrainHeight(hydrology, drop.position.x, drop.position.z, previousY + 0.12F, surface) &&
                previousY >= surface && drop.position.y <= surface + 0.12F) {
                hydrology.water[static_cast<std::size_t>(cell)] =
                    std::min(4.0F, hydrology.water[static_cast<std::size_t>(cell)] + 0.045F);
                hydrology.wetness[static_cast<std::size_t>(cell)] =
                    std::min(1.0F, hydrology.wetness[static_cast<std::size_t>(cell)] + 0.08F);
                drop.position.y = kVolumeMin.y - 2.0F;
            }
        }
    }
    std::erase_if(drops, [](const RainDrop& drop) {
        return drop.position.y < kVolumeMin.y;
    });
}

void updateHydrology(HydrologyMap& hydrology, float deltaTime) {
    std::vector<float> incoming(hydrology.water.size(), 0.0F);
    const float flowDecay = std::exp(-deltaTime * 2.2F);
    constexpr std::array<std::array<int, 2>, 8> kNeighbors{{
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
        {1, 0}, {-1, 1}, {0, 1}, {1, 1}
    }};

    for (int z = 0; z < kGridZ; ++z) {
        for (int x = 0; x < kGridX; ++x) {
            const int cell = z * kGridX + x;
            const std::size_t index = static_cast<std::size_t>(cell);
            if (hydrology.height[index] <= kVolumeMin.y) {
                continue;
            }

            hydrology.flow[index] *= flowDecay;
            hydrology.waterfall[index] *= flowDecay;
            hydrology.downstream[index] = -1;
            hydrology.wetness[index] = std::max(0.0F, hydrology.wetness[index] - deltaTime * 0.002F);
            hydrology.wetness[index] = std::min(
                1.0F, hydrology.wetness[index] + std::min(1.0F, hydrology.water[index] * 0.4F) * deltaTime);
            hydrology.water[index] *= std::exp(-deltaTime * 0.006F);

            int lowestNeighbor = -1;
            const float currentWaterLevel = hydrology.height[index] +
                                            hydrology.water[index] * kWaterLevelScale;
            float lowestWaterLevel = currentWaterLevel - 0.025F;
            float steepestSlope = 0.0F;
            int airNeighbor = -1;
            for (const auto& offset : kNeighbors) {
                const int neighborX = x + offset[0];
                const int neighborZ = z + offset[1];
                if (neighborX < 0 || neighborX >= kGridX || neighborZ < 0 || neighborZ >= kGridZ) {
                    continue;
                }
                const int neighbor = neighborZ * kGridX + neighborX;
                const std::size_t neighborIndex = static_cast<std::size_t>(neighbor);
                const float neighborHeight = hydrology.height[neighborIndex];
                if (neighborHeight <= kVolumeMin.y) {
                    if (airNeighbor < 0 || offset[0] == 0 || offset[1] == 0) airNeighbor = neighbor;
                } else {
                    const float neighborWaterLevel = neighborHeight +
                                                     hydrology.water[neighborIndex] * kWaterLevelScale;
                    const float distance = std::sqrt(static_cast<float>(offset[0] * offset[0] + offset[1] * offset[1]));
                    const float slope = (currentWaterLevel - neighborWaterLevel) / distance;
                    if (neighborWaterLevel < currentWaterLevel - 0.025F && slope > steepestSlope) {
                        lowestWaterLevel = neighborWaterLevel;
                        steepestSlope = slope;
                        lowestNeighbor = neighbor;
                    }
                }
            }

            const float available = lowestNeighbor >= 0
                ? std::min(hydrology.water[index], (currentWaterLevel - lowestWaterLevel) / (2.0F * kWaterLevelScale))
                : hydrology.water[index];
            const float transfer = available * std::min(0.72F, deltaTime * 3.5F);
            if (lowestNeighbor >= 0 && transfer > 0.00001F) {
                const auto& path = waterPath(hydrology, cell, lowestNeighbor);
                const Vector3 landing = path.back();
                const int receiver = surfaceCellAt(hydrology, landing.x, landing.z);
                // A cliff path may land farther away than the adjacent grid node.
                // Keep water at blocked outlets, and deliver falls where they hit.
                if (receiver == cell) continue;
                hydrology.water[index] -= transfer;
                if (receiver >= 0 && landing.y > kVolumeMin.y) {
                    incoming[static_cast<std::size_t>(receiver)] += transfer;
                }
                hydrology.downstream[index] = lowestNeighbor;
                hydrology.flow[index] = std::max(hydrology.flow[index], transfer / std::max(deltaTime, 0.001F));
            } else if (airNeighbor >= 0 && transfer > 0.00001F) {
                const auto& path = waterPath(hydrology, cell, airNeighbor);
                const Vector3 landing = path.back();
                const int receiver = surfaceCellAt(hydrology, landing.x, landing.z);
                if (receiver == cell) continue;
                hydrology.water[index] -= transfer;
                if (receiver >= 0 && landing.y > kVolumeMin.y) {
                    incoming[static_cast<std::size_t>(receiver)] += transfer;
                }
                hydrology.downstream[index] = -2;
                hydrology.outlet[index] = airNeighbor;
                hydrology.waterfall[index] = std::max(
                    hydrology.waterfall[index], transfer / std::max(deltaTime, 0.001F));
            }
        }
    }

    for (std::size_t index = 0; index < hydrology.water.size(); ++index) {
        hydrology.water[index] = std::min(4.0F, hydrology.water[index] + incoming[index]);
    }
}

void applyWetness(Model& model, const HydrologyMap& hydrology,
                  const std::vector<unsigned char>& baseColors) {
    Mesh& mesh = model.meshes[0];
    for (int vertex = 0; vertex < mesh.vertexCount; ++vertex) {
        const int cell = surfaceCellAt(hydrology, mesh.vertices[vertex * 3], mesh.vertices[vertex * 3 + 2]);
        float wet = 0.0F;
        if (cell >= 0) {
            const float verticalDistance = std::abs(
                hydrology.height[static_cast<std::size_t>(cell)] - mesh.vertices[vertex * 3 + 1]);
            const float exposure = std::clamp(mesh.normals[vertex * 3 + 1] * 1.6F, 0.0F, 1.0F) *
                                   std::clamp(1.0F - verticalDistance / 1.8F, 0.0F, 1.0F);
            wet = hydrology.wetness[static_cast<std::size_t>(cell)] * exposure;
        }

        const float darkness = 1.0F - wet * 0.42F;
        mesh.colors[vertex * 4] = static_cast<unsigned char>(
            static_cast<float>(baseColors[static_cast<std::size_t>(vertex * 4)]) * darkness);
        mesh.colors[vertex * 4 + 1] = static_cast<unsigned char>(
            static_cast<float>(baseColors[static_cast<std::size_t>(vertex * 4 + 1)]) * darkness);
        mesh.colors[vertex * 4 + 2] = static_cast<unsigned char>(std::min(
            255.0F, static_cast<float>(baseColors[static_cast<std::size_t>(vertex * 4 + 2)]) *
                          (1.0F - wet * 0.2F) + wet * 20.0F));
        mesh.colors[vertex * 4 + 3] = 255;
    }
    UpdateMeshBuffer(mesh, 3, mesh.colors, mesh.vertexCount * 4 * static_cast<int>(sizeof(unsigned char)), 0);
}

void drawWaterRibbon(const HydrologyMap& hydrology, const std::vector<Vector3>& path,
                     float width, Color color) {
    if (path.size() < 2) return;
    Vector3 previousLeft{};
    Vector3 previousRight{};
    Vector3 side{1.0F, 0.0F, 0.0F};
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Vector3 tangent = subtract(path[std::min(i + 1, path.size() - 1)], path[i == 0 ? 0 : i - 1]);
        if (tangent.x * tangent.x + tangent.z * tangent.z > 0.000001F) {
            side = normalize(Vector3{-tangent.z, 0.0F, tangent.x});
        }
        Vector3 left{path[i].x + side.x * width, path[i].y, path[i].z + side.z * width};
        Vector3 right{path[i].x - side.x * width, path[i].y, path[i].z - side.z * width};
        float centerHeight = 0.0F;
        const bool attached = terrainHeight(hydrology, path[i].x, path[i].z, path[i].y + 0.1F, centerHeight) &&
                              std::abs(path[i].y - centerHeight) < 0.15F;
        if (attached) {
            for (Vector3* bank : {&left, &right}) {
                float height = 0.0F;
                if (terrainHeight(hydrology, bank->x, bank->z, path[i].y + 0.4F, height) &&
                    std::abs(height - centerHeight) < 0.5F) {
                    bank->y = height + 0.045F;
                } else {
                    *bank = path[i];
                }
            }
        }
        if (i > 0) {
            DrawTriangle3D(previousLeft, left, right, color);
            DrawTriangle3D(previousLeft, right, previousRight, color);
            // A thin water sheet is visible from either side of an overhang.
            DrawTriangle3D(right, left, previousLeft, color);
            DrawTriangle3D(previousRight, right, previousLeft, color);
        }
        previousLeft = left;
        previousRight = right;
    }
}

void drawHydrology(HydrologyMap& hydrology) {
    const float cellWidth = (kVolumeMax.x - kVolumeMin.x) / static_cast<float>(kGridX - 1);
    const float cellDepth = (kVolumeMax.z - kVolumeMin.z) / static_cast<float>(kGridZ - 1);
    const float poolRadius = std::min(cellWidth, cellDepth) * 0.62F;
    for (std::size_t index = 0; index < hydrology.water.size(); ++index) {
        const float storedWater = hydrology.water[index];
        const bool standingWater = hydrology.downstream[index] == -1 && storedWater > 0.045F;
        const bool deepPool = storedWater > 0.14F;
        if (hydrology.height[index] > kVolumeMin.y && (standingWater || deepPool)) {
            Vector3 center = surfaceCellPosition(hydrology, static_cast<int>(index));
            center.y += std::min(1.4F, storedWater * kWaterLevelScale) + 0.025F;
            const unsigned char alpha = static_cast<unsigned char>(std::clamp(
                105.0F + storedWater * 120.0F, 105.0F, 205.0F));
            DrawCylinder(center, poolRadius, poolRadius, 0.05F, 10,
                         Color{42, 124, 166, alpha});
        }
    }

    for (std::size_t index = 0; index < hydrology.flow.size(); ++index) {
        if (hydrology.downstream[index] >= 0 && hydrology.flow[index] > 0.008F) {
            const float strength = std::clamp(std::sqrt(hydrology.flow[index]) * 0.12F, 0.025F, 0.2F);
            drawWaterRibbon(hydrology, waterPath(hydrology, static_cast<int>(index), hydrology.downstream[index]),
                            strength, Color{54, 139, 178, 220});
        } else if (hydrology.downstream[index] == -2 && hydrology.outlet[index] >= 0 &&
                   hydrology.waterfall[index] > 0.012F) {
            const float strength = std::clamp(std::sqrt(hydrology.waterfall[index]) * 0.14F,
                                              0.035F, 0.24F);
            drawWaterRibbon(hydrology, waterPath(hydrology, static_cast<int>(index), hydrology.outlet[index]),
                            strength, Color{104, 181, 211, 205});
        }
    }
}

void drawWeather(const std::vector<Cloud>& clouds, const std::vector<RainDrop>& drops) {
    for (const RainDrop& drop : drops) {
        DrawLine3D(drop.position,
                   Vector3{drop.position.x - drop.wind * 0.035F,
                           drop.position.y + 0.55F,
                           drop.position.z},
                   Color{151, 203, 222, 175});
    }

    constexpr std::array<Vector3, 8> kPuffOffsets{{
        {-0.95F, -0.08F, 0.0F}, {-0.45F, 0.22F, -0.12F}, {0.0F, 0.02F, 0.12F},
        {0.48F, 0.25F, -0.08F}, {0.98F, -0.05F, 0.08F}, {-0.35F, -0.2F, 0.24F},
        {0.35F, -0.18F, 0.3F}, {0.05F, 0.38F, -0.24F}
    }};
    for (const Cloud& cloud : clouds) {
        const float height = cloudHeight(cloud);
        for (std::size_t index = 0; index < kPuffOffsets.size(); ++index) {
            const Vector3& offset = kPuffOffsets[index];
            const float variation = 0.82F + randomAt(static_cast<int>(index), 0, 0, cloud.shapeSeed) * 0.32F;
            const float radius = cloud.size * 0.42F * variation;
            const Color color = index == 5 || index == 6
                ? Color{128, 145, 151, 245}
                : Color{190, 203, 201, 250};
            DrawSphereEx(Vector3{
                             cloud.position.x + offset.x * cloud.size,
                             height + offset.y * cloud.size,
                             cloud.position.z + offset.z * cloud.size
                         },
                         radius, 8, 12, color);
        }
    }
}

} // namespace

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1280, 760, "Volumetric Noise Islands");
    SetWindowMinSize(1000, 650);
    SetTargetFPS(60);

    std::mt19937 randomEngine(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> seedDistribution;
    std::uint32_t seed = seedDistribution(randomEngine);
    GenerationSettings settings;
    int triangleCount = 0;
    int islandCount = 0;
    HydrologyMap hydrology;
    std::vector<unsigned char> baseColors;
    Model islands = createIslands(seed, settings, triangleCount, islandCount,
                                  hydrology, baseColors);
    std::vector<Cloud> clouds = createClouds(seed, settings);
    std::vector<RainDrop> rainDrops;
    std::mt19937 weatherEngine(seed ^ 0x9e3779b9U);
    TraceLog(LOG_INFO, "VOLUME: Generated %d islands and %d triangles from seed %u",
             islandCount, triangleCount, seed);

    float yaw = 0.72F;
    float pitch = 0.32F;
    float distance = 52.0F;
    bool wireframe = false;
    bool autoRotate = true;
    bool rebuildRequested = false;

    Camera3D camera{};
    camera.target = Vector3{0.0F, 1.5F, 0.0F};
    camera.up = Vector3{0.0F, 1.0F, 0.0F};
    camera.fovy = 45.0F;
    camera.projection = CAMERA_PERSPECTIVE;

    while (!WindowShouldClose()) {
        const float deltaTime = GetFrameTime();
        if (IsKeyPressed(KEY_SPACE)) {
            seed = seedDistribution(randomEngine);
            rebuildRequested = true;
        }
        if (IsKeyPressed(KEY_W)) {
            wireframe = !wireframe;
        }
        if (IsKeyPressed(KEY_R)) {
            autoRotate = !autoRotate;
        }

        const bool mouseOverSidebar = GetMousePosition().x >=
                                      static_cast<float>(GetScreenWidth()) - kSidebarWidth;
        if (!mouseOverSidebar && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const Vector2 delta = GetMouseDelta();
            yaw -= delta.x * 0.006F;
            pitch = std::clamp(pitch + delta.y * 0.006F, -1.25F, 1.25F);
            autoRotate = false;
        } else if (autoRotate) {
            yaw += deltaTime * 0.08F;
        }
        if (!mouseOverSidebar) {
            distance = std::clamp(distance - GetMouseWheelMove() * 2.5F, 24.0F, 82.0F);
        }

        camera.position = Vector3{
            std::cos(yaw) * std::cos(pitch) * distance,
            std::sin(pitch) * distance + 1.5F,
            std::sin(yaw) * std::cos(pitch) * distance
        };
        updateWeather(clouds, rainDrops, weatherEngine, hydrology, deltaTime);
        updateHydrology(hydrology, deltaTime);
        applyWetness(islands, hydrology, baseColors);

        BeginDrawing();
        ClearBackground(Color{5, 10, 19, 255});
        DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                               Color{18, 33, 48, 255}, Color{3, 7, 15, 255});
        drawStars();

        BeginMode3D(camera);
        if (wireframe) {
            DrawModelWires(islands, Vector3{0.0F, 0.0F, 0.0F}, 1.0F, Color{174, 224, 206, 255});
        } else {
            DrawModel(islands, Vector3{0.0F, 0.0F, 0.0F}, 1.0F, WHITE);
        }
        drawHydrology(hydrology);
        drawWeather(clouds, rainDrops);
        EndMode3D();

        drawInterface(seed, islandCount, triangleCount);
        const SidebarAction sidebarAction = drawSidebar(settings, seed, wireframe, autoRotate);
        if (sidebarAction == SidebarAction::rebuild) {
            rebuildRequested = true;
        } else if (sidebarAction == SidebarAction::newSeed) {
            seed = seedDistribution(randomEngine);
            rebuildRequested = true;
        }
        DrawFPS(GetScreenWidth() - static_cast<int>(kSidebarWidth) - 92, GetScreenHeight() - 34);
        EndDrawing();

        if (rebuildRequested) {
            UnloadModel(islands);
            islands = createIslands(seed, settings, triangleCount, islandCount,
                                    hydrology, baseColors);
            clouds = createClouds(seed, settings);
            rainDrops.clear();
            weatherEngine.seed(seed ^ 0x9e3779b9U);
            TraceLog(LOG_INFO, "VOLUME: Generated %d islands and %d triangles from seed %u",
                     islandCount, triangleCount, seed);
            rebuildRequested = false;
        }
    }

    UnloadModel(islands);
    CloseWindow();
    return 0;
}
