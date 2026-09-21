#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int kGridSize = 128;
constexpr float kTerrainSize = 42.0F;
constexpr float kWaterLevel = 0.0F;

float smoothstep(float value) {
    return value * value * (3.0F - 2.0F * value);
}

float mix(float a, float b, float amount) {
    return a + (b - a) * amount;
}

Vector3 normalize(const Vector3& vector) {
    const float length = std::sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
    return Vector3{vector.x / length, vector.y / length, vector.z / length};
}

float dot(const Vector3& a, const Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

std::uint32_t hash2d(int x, int y, std::uint32_t seed) {
    std::uint32_t value = static_cast<std::uint32_t>(x) * 0x8da6b343U;
    value ^= static_cast<std::uint32_t>(y) * 0xd8163841U;
    value ^= seed * 0xcb1ab31fU;
    value ^= value >> 13U;
    value *= 0x85ebca6bU;
    value ^= value >> 16U;
    return value;
}

float randomAt(int x, int y, std::uint32_t seed) {
    return static_cast<float>(hash2d(x, y, seed) & 0x00ffffffU) /
           static_cast<float>(0x00ffffffU);
}

float valueNoise(float x, float y, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float tx = smoothstep(x - static_cast<float>(x0));
    const float ty = smoothstep(y - static_cast<float>(y0));

    const float top = mix(randomAt(x0, y0, seed), randomAt(x0 + 1, y0, seed), tx);
    const float bottom = mix(randomAt(x0, y0 + 1, seed),
                             randomAt(x0 + 1, y0 + 1, seed), tx);
    return mix(top, bottom, ty);
}

float fractalNoise(float x, float y, std::uint32_t seed) {
    float value = 0.0F;
    float amplitude = 0.5F;
    float frequency = 1.0F;
    float amplitudeSum = 0.0F;

    for (int octave = 0; octave < 6; ++octave) {
        value += valueNoise(x * frequency, y * frequency,
                            seed + static_cast<std::uint32_t>(octave) * 1013U) * amplitude;
        amplitudeSum += amplitude;
        amplitude *= 0.5F;
        frequency *= 2.03F;
    }

    return value / amplitudeSum;
}

float terrainHeight(float normalizedX, float normalizedZ, std::uint32_t seed) {
    const float warpX = valueNoise(normalizedX * 2.0F + 17.0F,
                                   normalizedZ * 2.0F - 9.0F, seed + 41U) - 0.5F;
    const float warpZ = valueNoise(normalizedX * 2.0F - 23.0F,
                                   normalizedZ * 2.0F + 5.0F, seed + 79U) - 0.5F;
    const float noise = fractalNoise(normalizedX * 3.4F + warpX * 0.9F,
                                     normalizedZ * 3.4F + warpZ * 0.9F, seed);

    const float distance = std::sqrt(normalizedX * normalizedX + normalizedZ * normalizedZ);
    const float irregularity = (valueNoise(normalizedX * 1.5F + 31.0F,
                                            normalizedZ * 1.5F - 12.0F, seed + 131U) - 0.5F) * 0.25F;
    const float islandFalloff = std::pow(std::max(0.0F, distance + irregularity), 2.2F);
    return std::max(-3.2F, (noise - 0.38F) * 10.0F + 2.6F - islandFalloff * 9.0F);
}

Color terrainColor(float height, const Vector3& normal) {
    Color base{};
    if (height < kWaterLevel + 0.18F) {
        base = Color{210, 190, 132, 255};
    } else if (height < 2.3F) {
        base = Color{67, 122, 69, 255};
    } else if (height < 4.8F) {
        base = Color{91, 105, 72, 255};
    } else {
        base = Color{205, 211, 207, 255};
    }

    const Vector3 light = normalize(Vector3{-0.45F, 0.85F, -0.3F});
    const float diffuse = std::clamp(dot(normal, light), 0.0F, 1.0F);
    const float brightness = 0.58F + diffuse * 0.42F;
    return Color{
        static_cast<unsigned char>(static_cast<float>(base.r) * brightness),
        static_cast<unsigned char>(static_cast<float>(base.g) * brightness),
        static_cast<unsigned char>(static_cast<float>(base.b) * brightness),
        255
    };
}

Model createTerrain(std::uint32_t seed) {
    std::vector<float> heights(static_cast<std::size_t>(kGridSize * kGridSize));
    const float halfSize = kTerrainSize * 0.5F;
    const float spacing = kTerrainSize / static_cast<float>(kGridSize - 1);

    for (int z = 0; z < kGridSize; ++z) {
        for (int x = 0; x < kGridSize; ++x) {
            const float nx = (static_cast<float>(x) / static_cast<float>(kGridSize - 1)) * 2.0F - 1.0F;
            const float nz = (static_cast<float>(z) / static_cast<float>(kGridSize - 1)) * 2.0F - 1.0F;
            heights[static_cast<std::size_t>(z * kGridSize + x)] = terrainHeight(nx, nz, seed);
        }
    }

    std::vector<Vector3> normals(heights.size());
    for (int z = 0; z < kGridSize; ++z) {
        for (int x = 0; x < kGridSize; ++x) {
            const int left = std::max(0, x - 1);
            const int right = std::min(kGridSize - 1, x + 1);
            const int near = std::max(0, z - 1);
            const int far = std::min(kGridSize - 1, z + 1);
            const float dx = heights[static_cast<std::size_t>(z * kGridSize + right)] -
                             heights[static_cast<std::size_t>(z * kGridSize + left)];
            const float dz = heights[static_cast<std::size_t>(far * kGridSize + x)] -
                             heights[static_cast<std::size_t>(near * kGridSize + x)];
            normals[static_cast<std::size_t>(z * kGridSize + x)] =
                normalize(Vector3{-dx, spacing * 2.0F, -dz});
        }
    }

    constexpr std::array<int, 6> kTrianglePattern{0, 2, 1, 1, 2, 3};
    const int quadCount = (kGridSize - 1) * (kGridSize - 1);
    Mesh mesh{};
    mesh.triangleCount = quadCount * 2;
    mesh.vertexCount = mesh.triangleCount * 3;
    mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned int>(mesh.vertexCount * 3 * sizeof(float))));
    mesh.normals = static_cast<float*>(MemAlloc(static_cast<unsigned int>(mesh.vertexCount * 3 * sizeof(float))));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(mesh.vertexCount * 4 * sizeof(unsigned char))));

    int vertex = 0;
    for (int z = 0; z < kGridSize - 1; ++z) {
        for (int x = 0; x < kGridSize - 1; ++x) {
            const std::array<int, 4> pointIndices{
                z * kGridSize + x,
                z * kGridSize + x + 1,
                (z + 1) * kGridSize + x,
                (z + 1) * kGridSize + x + 1
            };

            for (const int corner : kTrianglePattern) {
                const int pointIndex = pointIndices[static_cast<std::size_t>(corner)];
                const int pointX = pointIndex % kGridSize;
                const int pointZ = pointIndex / kGridSize;
                const float height = heights[static_cast<std::size_t>(pointIndex)];
                const Vector3 normal = normals[static_cast<std::size_t>(pointIndex)];
                const Color color = terrainColor(height, normal);

                mesh.vertices[vertex * 3] = static_cast<float>(pointX) * spacing - halfSize;
                mesh.vertices[vertex * 3 + 1] = height;
                mesh.vertices[vertex * 3 + 2] = static_cast<float>(pointZ) * spacing - halfSize;
                mesh.normals[vertex * 3] = normal.x;
                mesh.normals[vertex * 3 + 1] = normal.y;
                mesh.normals[vertex * 3 + 2] = normal.z;
                mesh.colors[vertex * 4] = color.r;
                mesh.colors[vertex * 4 + 1] = color.g;
                mesh.colors[vertex * 4 + 2] = color.b;
                mesh.colors[vertex * 4 + 3] = color.a;
                ++vertex;
            }
        }
    }

    UploadMesh(&mesh, false);
    return LoadModelFromMesh(mesh);
}

void drawInterface(std::uint32_t seed, bool wireframe, bool autoRotate) {
    DrawRectangleRounded(Rectangle{20.0F, 20.0F, 305.0F, 102.0F}, 0.12F, 8,
                         Fade(Color{8, 18, 24, 255}, 0.82F));
    DrawText("FRACTAL NOISE ISLAND", 36, 34, 20, Color{231, 222, 191, 255});
    DrawText(TextFormat("SEED  %u", seed), 36, 61, 16, Color{127, 195, 183, 255});
    DrawText("DRAG orbit   W wheel zoom   SPACE new seed", 36, 88, 12, Color{190, 204, 201, 255});

    const char* mode = wireframe ? "WIREFRAME" : "SOLID";
    const char* motion = autoRotate ? "AUTO" : "MANUAL";
    const char* status = TextFormat("[%s]  %s  |  R auto-rotate", mode, motion);
    const int right = GetScreenWidth() - 20;
    DrawText(status, right - MeasureText(status, 14), 28, 14, Color{205, 218, 214, 255});
}

} // namespace

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1280, 760, "Fractal Noise Islands");
    SetWindowMinSize(800, 500);
    SetTargetFPS(60);

    std::mt19937 randomEngine(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> seedDistribution;
    std::uint32_t seed = seedDistribution(randomEngine);
    Model terrain = createTerrain(seed);

    float yaw = 0.72F;
    float pitch = 0.48F;
    float distance = 48.0F;
    bool wireframe = false;
    bool autoRotate = true;

    Camera3D camera{};
    camera.target = Vector3{0.0F, 1.7F, 0.0F};
    camera.up = Vector3{0.0F, 1.0F, 0.0F};
    camera.fovy = 45.0F;
    camera.projection = CAMERA_PERSPECTIVE;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_SPACE)) {
            seed = seedDistribution(randomEngine);
            UnloadModel(terrain);
            terrain = createTerrain(seed);
        }
        if (IsKeyPressed(KEY_W)) {
            wireframe = !wireframe;
        }
        if (IsKeyPressed(KEY_R)) {
            autoRotate = !autoRotate;
        }

        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const Vector2 delta = GetMouseDelta();
            yaw -= delta.x * 0.006F;
            pitch = std::clamp(pitch + delta.y * 0.006F, 0.12F, 1.35F);
            autoRotate = false;
        } else if (autoRotate) {
            yaw += GetFrameTime() * 0.08F;
        }
        distance = std::clamp(distance - GetMouseWheelMove() * 2.5F, 24.0F, 78.0F);

        camera.position = Vector3{
            std::cos(yaw) * std::cos(pitch) * distance,
            std::sin(pitch) * distance + 1.7F,
            std::sin(yaw) * std::cos(pitch) * distance
        };

        BeginDrawing();
        ClearBackground(Color{12, 29, 37, 255});
        DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                               Color{25, 53, 62, 255}, Color{5, 14, 22, 255});

        BeginMode3D(camera);
        DrawPlane(Vector3{0.0F, -3.25F, 0.0F}, Vector2{80.0F, 80.0F}, Color{9, 27, 34, 255});
        if (wireframe) {
            DrawModelWires(terrain, Vector3{0.0F, 0.0F, 0.0F}, 1.0F, Color{202, 230, 219, 255});
        } else {
            DrawModel(terrain, Vector3{0.0F, 0.0F, 0.0F}, 1.0F, WHITE);
        }
        DrawCube(Vector3{0.0F, kWaterLevel - 0.08F, 0.0F},
                 kTerrainSize + 8.0F, 0.12F, kTerrainSize + 8.0F,
                 Color{34, 116, 143, 185});
        DrawCubeWires(Vector3{0.0F, kWaterLevel - 0.08F, 0.0F},
                      kTerrainSize + 8.0F, 0.12F, kTerrainSize + 8.0F,
                      Color{89, 170, 183, 100});
        EndMode3D();

        drawInterface(seed, wireframe, autoRotate);
        DrawFPS(GetScreenWidth() - 92, GetScreenHeight() - 34);
        EndDrawing();
    }

    UnloadModel(terrain);
    CloseWindow();
    return 0;
}
