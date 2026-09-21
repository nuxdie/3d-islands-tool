#define main islandsApplicationMain
#include "../src/main.cpp"
#undef main

#include <iostream>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Synthetic closed basin: two initial pools, separated by a submerged saddle.
// This tests the actual GPU passes (not a CPU reimplementation of the shaders).
void checkMerging(GpuWater& gpu) {
    const int count = kGridX * kGridZ;
    std::vector<Vector4> terrain(count, Vector4{10,0,0,0});
    std::vector<Vector4> routes(count, Vector4{-2,-2,-2,-2});
    std::vector<Vector4> edges;
    std::vector<Vector4> initial(count);
    const int cx = kGridX / 2;
    const int cz = kGridZ / 2;
    for (int z = cz - 1; z <= cz + 1; ++z) for (int x = cx - 2; x <= cx + 2; ++x) {
        terrain[z * kGridX + x].x = x == cx ? 0.2F : 0.0F;
    }
    initial[(cz - 1) * kGridX + cx - 2].x = 8.0F;
    initial[(cz + 1) * kGridX + cx + 2].x = 8.0F;
    for (int cell = 0; cell < count; ++cell) {
        terrain[cell].y = static_cast<float>(edges.size());
        std::array<float,4> to{-2,-2,-2,-2};
        for (int d = 0; d < 4; ++d) {
            int x = cell % kGridX + kFlowDirections[d][0];
            int z = cell / kGridX + kFlowDirections[d][1];
            if (x < 0 || x >= kGridX || z < 0 || z >= kGridZ) continue;
            const int neighbor = z * kGridX + x;
            to[d] = static_cast<float>(neighbor);
            edges.push_back(Vector4{static_cast<float>(neighbor), static_cast<float>(d ^ 1), 0, 0});
        }
        routes[cell] = Vector4{to[0],to[1],to[2],to[3]};
        terrain[cell].z = static_cast<float>(edges.size()) - terrain[cell].y;
    }
    edges.resize(((edges.size() + 1023) / 1024) * 1024);
    UnloadTexture(gpu.incoming);
    gpu.incoming = floatTexture(1024, static_cast<int>(edges.size() / 1024), edges.data());
    UpdateTexture(gpu.terrain, terrain.data());
    UpdateTexture(gpu.routes, routes.data());
    UpdateTexture(gpu.state[gpu.current].texture, initial.data());
    gpu.evaporation = 0;
    for (int i = 0; i < 1800; ++i) stepGpuWater(gpu, {});
    const Texture2D texture = gpu.state[gpu.current].texture;
    auto* pixels = static_cast<Vector4*>(rlReadTexturePixels(texture.id, texture.width, texture.height, texture.format));
    require(pixels != nullptr, "GPU readback failed");
    float total = 0;
    for (int i = 0; i < count; ++i) {
        require(std::isfinite(pixels[i].x) && pixels[i].x >= 0, "Invalid GPU water depth");
        total += pixels[i].x;
    }
    require(std::abs(total - 16.0F) < 0.003F, "GPU water did not conserve volume");
    const float expectedLevel = (16.0F + 3.0F * 0.2F) / 15.0F;
    for (int z = cz - 1; z <= cz + 1; ++z) for (int x = cx - 2; x <= cx + 2; ++x) {
        const int cell = z * kGridX + x;
        require(pixels[cell].x > 0.8F, "Pools failed to spread across saddle");
        require(std::abs(terrain[cell].x + pixels[cell].x - expectedLevel) < 0.004F,
                "Connected pools did not settle to a shared water level");
    }
    MemFree(pixels);
    // Evaluate the same surface function used by the lake vertex/fragment shaders.
    // The joined pools must render at one elevation despite different bed heights.
    Shader surface = waterShader(nullptr, R"GLSL(
        out vec4 finalColor;
        void main() {
            vec2 world = mix(volumeMin.xz, volumeMax.xz, (gl_FragCoord.xy - 0.5) / (gridSize - 1.0));
            finalColor = vec4(lakeAt(world), 0, 1);
        }
    )GLSL", waterShaders::lakeSampling);
    simulationPass(gpu.flux, surface, gpu.state[gpu.current].texture,
                   {{"terrain", gpu.terrain}, {"state", gpu.state[gpu.current].texture}});
    auto* levels = static_cast<Vector4*>(rlReadTexturePixels(gpu.flux.texture.id, kGridX, kGridZ, gpu.flux.texture.format));
    require(levels != nullptr, "Lake surface readback failed");
    for (int z = cz - 1; z <= cz + 1; ++z) for (int x = cx - 2; x <= cx + 2; ++x) {
        require(std::abs(levels[z * kGridX + x].x - expectedLevel) < 0.004F,
                "GPU lake surface has a discontinuity between pools");
    }
    MemFree(levels);
    UnloadShader(surface);
    std::cout << "GPU pools merge across a saddle, equalize their surface, and conserve 16 units of water\n";
}

void checkRain(GpuWater& gpu) {
    Shader motion = waterShader(nullptr, R"GLSL(
        out vec4 finalColor;
        void main() {
            float id = floor(gl_FragCoord.y);
            vec4 cloud = vec4(0, 20.0 + mod(id, 10.0), 0, 3.0);
            float age;
            finalColor = rainAt(cloud, id, time + floor(gl_FragCoord.x) / 60.0, age);
        }
    )GLSL", waterShaders::rainMotion);
    int respawns = 0;
    int changedOrigins = 0;
    for (float clock : {0.0F, 180.0F, 3600.0F, 21600.0F}) {
        SetShaderValue(motion, GetShaderLocation(motion, "time"), &clock, SHADER_UNIFORM_FLOAT);
        simulationPass(gpu.flux, motion, gpu.state[gpu.current].texture, {});
        auto* samples = static_cast<Vector4*>(rlReadTexturePixels(gpu.flux.texture.id, kGridX, kGridZ, gpu.flux.texture.format));
        require(samples != nullptr, "Rain motion readback failed");
        for (int z = 0; z < kGridZ; ++z) for (int x = 1; x < kGridX; ++x) {
            const Vector4 a = samples[z * kGridX + x - 1];
            const Vector4 b = samples[z * kGridX + x];
            require(std::isfinite(b.y), "Nonfinite rain position");
            if (a.w == b.w) {
                require(b.y < a.y, "An in-flight raindrop moved upward");
                require(a.x == b.x && a.z == b.z, "Rain origin moved during its lifetime");
            } else {
                ++respawns;
                changedOrigins += a.x != b.x || a.z != b.z;
            }
        }
        MemFree(samples);
    }
    std::cout << "Rain respawns: " << respawns << ", changed origins: " << changedOrigins << '\n';
    require(respawns > 0 && changedOrigins > respawns * 3 / 4, "Rain repeats the same visible columns on respawn");
    UnloadShader(motion);
    std::cout << "GPU rain falls monotonically within each lifetime, including after six hours\n";
}

int main() {
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) return 77;
#endif
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(128, 128, "GPU water tests");
    if (!IsWindowReady()) return 77;
    try {
        HydrologyMap map;
        int islands = 0;
        extractIslands(3786946813U, {}, islands, map);
        GpuWater gpu = createGpuWater(map, 6);
        checkMerging(gpu);
        checkRain(gpu);
        unloadGpuWater(gpu);
        CloseWindow();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        CloseWindow();
        return 1;
    }
}
