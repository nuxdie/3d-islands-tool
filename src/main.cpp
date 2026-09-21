#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace {

constexpr int kGridX = 56;
constexpr int kGridY = 40;
constexpr int kGridZ = 56;
constexpr Vector3 kVolumeMin{-27.0F, -14.0F, -27.0F};
constexpr Vector3 kVolumeMax{27.0F, 17.0F, 27.0F};
constexpr float kSidebarWidth = 400.0F;

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
                                      point.z * 0.105F + 13.0F, seed, 5) - 0.5F;
    const float caveScale = 0.135F;
    const float caveDistance = cellularDistance3d(point.x * caveScale - 21.0F,
                                                   point.y * caveScale + 7.0F,
                                                   point.z * caveScale - 4.0F, seed + 7919U);
    const float caveRoughness = fractalNoise3d(point.x * 0.24F, point.y * 0.24F,
                                               point.z * 0.24F, seed + 104729U, 3) - 0.5F;
    const float caveRadius = settings.caveSize + caveRoughness * 0.16F;
    const float caveCut = (1.0F - smoothstep(caveRadius, caveRadius + 0.13F, caveDistance)) *
                          settings.caveStrength;
    return island + rock * settings.roughness - caveCut;
}

Color surfaceColor(const SurfaceVertex& vertex) {
    Color base{};
    const float upward = vertex.normal.y;
    if (upward > 0.52F && vertex.position.y > -1.5F) {
        base = vertex.position.y > 7.0F ? Color{168, 178, 151, 255} : Color{63, 121, 73, 255};
    } else if (upward < -0.45F) {
        base = Color{76, 60, 55, 255};
    } else {
        base = Color{119, 101, 82, 255};
    }

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
    const float amount = std::clamp(a.density / (a.density - b.density), 0.0F, 1.0F);
    return SurfaceVertex{
        mix(a.position, b.position, amount),
        normalize(mix(a.normal, b.normal, amount))
    };
}

void addTriangle(std::vector<SurfaceVertex>& vertices,
                 SurfaceVertex a, SurfaceVertex b, SurfaceVertex c) {
    const Vector3 faceNormal = cross(subtract(b.position, a.position), subtract(c.position, a.position));
    const Vector3 averageNormal = Vector3{
        a.normal.x + b.normal.x + c.normal.x,
        a.normal.y + b.normal.y + c.normal.y,
        a.normal.z + b.normal.z + c.normal.z
    };
    if (dot(faceNormal, averageNormal) < 0.0F) {
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
    if (insideCount == 1 || insideCount == 3) {
        const bool singleInside = insideCount == 1;
        const int anchor = singleInside ? inside[0] : outside[0];
        const auto& others = singleInside ? outside : inside;
        addTriangle(vertices,
                    interpolateSurface(*tetra[static_cast<std::size_t>(anchor)], *tetra[static_cast<std::size_t>(others[0])]),
                    interpolateSurface(*tetra[static_cast<std::size_t>(anchor)], *tetra[static_cast<std::size_t>(others[1])]),
                    interpolateSurface(*tetra[static_cast<std::size_t>(anchor)], *tetra[static_cast<std::size_t>(others[2])]));
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
    addTriangle(vertices, ac, ad, bd);
    addTriangle(vertices, ac, bd, bc);
}

Model createIslands(std::uint32_t seed, const GenerationSettings& settings,
                    int& triangleCount, int& islandCount) {
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
                samples[sampleIndex(x, y, z)].normal = normalize(Vector3{-dx, -dy, -dz});
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
    Model islands = createIslands(seed, settings, triangleCount, islandCount);
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
            yaw += GetFrameTime() * 0.08F;
        }
        if (!mouseOverSidebar) {
            distance = std::clamp(distance - GetMouseWheelMove() * 2.5F, 24.0F, 82.0F);
        }

        camera.position = Vector3{
            std::cos(yaw) * std::cos(pitch) * distance,
            std::sin(pitch) * distance + 1.5F,
            std::sin(yaw) * std::cos(pitch) * distance
        };

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
            islands = createIslands(seed, settings, triangleCount, islandCount);
            TraceLog(LOG_INFO, "VOLUME: Generated %d islands and %d triangles from seed %u",
                     islandCount, triangleCount, seed);
            rebuildRequested = false;
        }
    }

    UnloadModel(islands);
    CloseWindow();
    return 0;
}
