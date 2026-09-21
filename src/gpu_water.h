// Included inside the application namespace after the terrain sampling helpers.
// Geometry and routing are baked at generation time. No simulation readbacks or
// per-frame mesh uploads are used by the renderer.
constexpr std::array<std::array<int, 2>, 4> kFlowDirections{{{-1,0}, {1,0}, {0,-1}, {0,1}}};
constexpr float kWaterStep = 1.0F / 60.0F;
constexpr int kPoolSubdivisions = 3;

struct WaterVertex {
    Vector3 position;
    Vector3 offset;
    Vector2 source;
    Vector2 path;
    Color color = WHITE;
};

struct WaterBake {
    std::vector<Vector4> terrain;
    std::vector<Vector4> routes;
    std::vector<Vector4> incoming;
    std::vector<WaterVertex> rivers;
};

void appendRiver(std::vector<WaterVertex>& mesh, const HydrologyMap& map,
                 const std::vector<Vector3>& path, int cell, int direction) {
    if (path.size() < 2) return;
    const Vector2 source{(static_cast<float>(cell % kGridX) + 0.5F) / kGridX,
                         (static_cast<float>(cell / kGridX) + 0.5F) / kGridZ};
    std::array<WaterVertex, 2> previous{};
    Vector3 side{1,0,0};
    float distance = 0.0F;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Vector3 tangent = subtract(path[std::min(i + 1, path.size() - 1)], path[i == 0 ? 0 : i - 1]);
        if (tangent.x * tangent.x + tangent.z * tangent.z > 0.000001F) side = normalize(Vector3{-tangent.z, 0, tangent.x});
        if (i > 0) {
            const Vector3 delta = subtract(path[i], path[i - 1]);
            distance += std::sqrt(dot(delta, delta));
        }
        float bed = 0.0F;
        const bool attached = terrainHeight(map, path[i].x, path[i].z, path[i].y + 0.1F, bed) &&
                              std::abs(path[i].y - bed) < 0.15F;
        std::array<WaterVertex, 2> banks{};
        for (int bank = 0; bank < 2; ++bank) {
            const float width = bank == 0 ? -0.22F : 0.22F;
            Vector3 p{path[i].x + side.x * width, path[i].y, path[i].z + side.z * width};
            if (attached) {
                float height = 0.0F;
                if (terrainHeight(map, p.x, p.z, path[i].y + 0.4F, height) && std::abs(height - bed) < 0.5F) {
                    p.y = height + 0.045F;
                } else p = path[i];
            }
            banks[bank] = WaterVertex{path[i], subtract(p, path[i]), source,
                                      Vector2{static_cast<float>(direction), distance},
                                      Color{static_cast<unsigned char>(bank * 255), 255, 255, 255}};
        }
        if (i > 0) {
            for (const WaterVertex& v : {previous[0], banks[0], banks[1], previous[0], banks[1], previous[1]}) mesh.push_back(v);
        }
        previous = banks;
    }
}

WaterBake bakeWater(HydrologyMap& map, bool geometry = true) {
    WaterBake bake;
    const int count = kGridX * kGridZ;
    bake.terrain.resize(count);
    bake.routes.assign(count, Vector4{-2,-2,-2,-2});
    std::vector<std::vector<Vector4>> incoming(count);
    for (int cell = 0; cell < count; ++cell) {
        bake.terrain[cell].x = map.height[cell];
        if (map.height[cell] <= kVolumeMin.y) continue;
        std::array<float, 4> destinations{-2,-2,-2,-2};
        for (int d = 0; d < 4; ++d) {
            const int x = cell % kGridX + kFlowDirections[d][0];
            const int z = cell / kGridX + kFlowDirections[d][1];
            if (x < 0 || x >= kGridX || z < 0 || z >= kGridZ) continue;
            const int neighbor = z * kGridX + x;
            int receiver = neighbor;
            // Rising water can cross a bank even when a surface stream cannot.
            // Only downhill links need a terrain/ballistic path.
            if (map.height[neighbor] < map.height[cell] - 0.001F) {
                const auto& path = waterPath(map, cell, neighbor);
                const Vector3 end = path.back();
                receiver = end.y > kVolumeMin.y ? surfaceCellAt(map, end.x, end.z) : -1;
                if (receiver == cell) receiver = -2;
                if (geometry && receiver != -2) appendRiver(bake.rivers, map, path, cell, d);
            }
            destinations[d] = static_cast<float>(receiver);
            if (receiver >= 0) incoming[receiver].push_back(Vector4{static_cast<float>(cell), static_cast<float>(d), 0, 0});
        }
        bake.routes[cell] = Vector4{destinations[0], destinations[1], destinations[2], destinations[3]};
    }
    for (int cell = 0; cell < count; ++cell) {
        bake.terrain[cell].y = static_cast<float>(bake.incoming.size());
        bake.terrain[cell].z = static_cast<float>(incoming[cell].size());
        bake.incoming.insert(bake.incoming.end(), incoming[cell].begin(), incoming[cell].end());
    }
    // Texture rows are padded; offsets/counts still refer to the compact list.
    bake.incoming.resize(std::max<std::size_t>(1, (bake.incoming.size() + 1023) / 1024) * 1024);
    return bake;
}

Texture2D floatTexture(int width, int height, const void* pixels) {
    Texture2D texture{rlLoadTexture(pixels, width, height, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, 1),
                      width, height, 1, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32};
    if (texture.id == 0) throw std::runtime_error("Could not allocate GPU water texture");
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
    return texture;
}

RenderTexture2D floatTarget() {
    const std::vector<Vector4> zeros(kGridX * kGridZ);
    RenderTexture2D target{};
    target.texture = floatTexture(kGridX, kGridZ, zeros.data());
    target.id = rlLoadFramebuffer();
    rlFramebufferAttach(target.id, target.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(target.id)) throw std::runtime_error("Floating-point water framebuffer is incomplete");
    rlDisableFramebuffer();
    return target;
}

Shader waterShader(const char* vertex, const char* fragment, const char* extra = "") {
    const std::string vs = std::string(waterShaders::common) + extra + (vertex ? vertex : "");
    const std::string fs = std::string(waterShaders::common) + extra + fragment;
    Shader shader = LoadShaderFromMemory(vertex ? vs.c_str() : nullptr, fs.c_str());
    if (shader.id == rlGetShaderIdDefault()) throw std::runtime_error("GPU water shader compilation failed (see shader log)");
    const Vector2 size{static_cast<float>(kGridX), static_cast<float>(kGridZ)};
    SetShaderValue(shader, GetShaderLocation(shader, "gridSize"), &size, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader, GetShaderLocation(shader, "volumeMin"), &kVolumeMin, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, GetShaderLocation(shader, "volumeMax"), &kVolumeMax, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, GetShaderLocation(shader, "dt"), &kWaterStep, SHADER_UNIFORM_FLOAT);
    return shader;
}

Model waterModel(const std::vector<WaterVertex>& vertices, Shader shader) {
    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(vertices.size());
    mesh.triangleCount = mesh.vertexCount / 3;
    if (vertices.empty()) return Model{};
    mesh.vertices = static_cast<float*>(MemAlloc(mesh.vertexCount * 3 * sizeof(float)));
    mesh.normals = static_cast<float*>(MemAlloc(mesh.vertexCount * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(MemAlloc(mesh.vertexCount * 2 * sizeof(float)));
    mesh.texcoords2 = static_cast<float*>(MemAlloc(mesh.vertexCount * 2 * sizeof(float)));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(mesh.vertexCount * 4));
    for (int i = 0; i < mesh.vertexCount; ++i) {
        const auto& v = vertices[i];
        std::memcpy(mesh.vertices + i * 3, &v.position, sizeof(Vector3));
        std::memcpy(mesh.normals + i * 3, &v.offset, sizeof(Vector3));
        std::memcpy(mesh.texcoords + i * 2, &v.source, sizeof(Vector2));
        std::memcpy(mesh.texcoords2 + i * 2, &v.path, sizeof(Vector2));
        std::memcpy(mesh.colors + i * 4, &v.color, sizeof(Color));
    }
    UploadMesh(&mesh, false);
    Model model = LoadModelFromMesh(mesh);
    model.materials[0].shader = shader;
    return model;
}

struct GpuWater {
    Texture2D terrain{}, routes{}, incoming{}, detailedHeight{};
    std::array<RenderTexture2D, 2> state{};
    RenderTexture2D flux{};
    Shader flowShader{}, integrateShader{}, terrainShader{}, lakeShader{}, riverShader{}, rainShader{};
    Model lakes{}, rivers{}, rain{}, cloudSphere{};
    int current = 0;
    float accumulator = 0;
    float time = 0;
    float evaporation = 0.0015F;
};

GpuWater createGpuWater(HydrologyMap& map, int cloudCount) {
    GpuWater gpu;
    WaterBake bake = bakeWater(map);
    gpu.terrain = floatTexture(kGridX, kGridZ, bake.terrain.data());
    gpu.routes = floatTexture(kGridX, kGridZ, bake.routes.data());
    gpu.incoming = floatTexture(1024, static_cast<int>(bake.incoming.size() / 1024), bake.incoming.data());
    gpu.state = {floatTarget(), floatTarget()};
    SetTextureFilter(gpu.terrain, TEXTURE_FILTER_BILINEAR);
    for (const auto& target : gpu.state) SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);
    gpu.flux = floatTarget();
    gpu.flowShader = waterShader(nullptr, waterShaders::flux);
    gpu.integrateShader = waterShader(nullptr, waterShaders::integrate);
    gpu.terrainShader = waterShader(waterShaders::terrainVertex, waterShaders::terrainFragment);
    gpu.lakeShader = waterShader(waterShaders::lakeVertex, waterShaders::lakeFragment, waterShaders::lakeSampling);
    gpu.riverShader = waterShader(waterShaders::riverVertex, waterShaders::riverFragment);
    gpu.rainShader = waterShader(waterShaders::rainVertex, waterShaders::rainFragment);
    gpu.rivers = waterModel(bake.rivers, gpu.riverShader);

    const int width = (kGridX - 1) * kPoolSubdivisions + 1;
    const int depth = (kGridZ - 1) * kPoolSubdivisions + 1;
    std::vector<Vector4> detailed(width * depth);
    std::vector<WaterVertex> pools;
    pools.reserve((width - 1) * (depth - 1) * 6);
    for (int z = 0; z < depth; ++z) for (int x = 0; x < width; ++x) {
        const float wx = mix(kVolumeMin.x, kVolumeMax.x, static_cast<float>(x) / (width - 1));
        const float wz = mix(kVolumeMin.z, kVolumeMax.z, static_cast<float>(z) / (depth - 1));
        float height = kVolumeMin.y - 1.0F;
        terrainHeight(map, wx, wz, kVolumeMax.y, height);
        detailed[z * width + x].x = height;
    }
    for (int z = 0; z < depth - 1; ++z) for (int x = 0; x < width - 1; ++x) {
        if (detailed[z * width + x].x <= kVolumeMin.y && detailed[z * width + x + 1].x <= kVolumeMin.y &&
            detailed[(z + 1) * width + x].x <= kVolumeMin.y && detailed[(z + 1) * width + x + 1].x <= kVolumeMin.y) continue;
        for (const auto& offset : std::array<std::array<int,2>,6>{{{0,0},{0,1},{1,1},{0,0},{1,1},{1,0}}}) {
            WaterVertex v{};
            v.position = Vector3{mix(kVolumeMin.x, kVolumeMax.x, static_cast<float>(x + offset[0]) / (width - 1)), 0,
                                 mix(kVolumeMin.z, kVolumeMax.z, static_cast<float>(z + offset[1]) / (depth - 1))};
            pools.push_back(v);
        }
    }
    gpu.detailedHeight = floatTexture(width, depth, detailed.data());
    SetTextureFilter(gpu.detailedHeight, TEXTURE_FILTER_BILINEAR);
    gpu.lakes = waterModel(pools, gpu.lakeShader);
    std::vector<WaterVertex> drops;
    for (int c = 0; c < cloudCount; ++c) for (int i = 0; i < 90; ++i) {
        for (const auto& corner : std::array<Vector2,6>{{{-1,0},{1,0},{1,1},{-1,0},{1,1},{-1,1}}}) {
            WaterVertex v{};
            v.position = Vector3{static_cast<float>(c), static_cast<float>(i), corner.y};
            v.source.x = corner.x;
            drops.push_back(v);
        }
    }
    gpu.rain = waterModel(drops, gpu.rainShader);
    gpu.cloudSphere = LoadModelFromMesh(GenMeshSphere(1.0F, 8, 12));
    TraceLog(LOG_INFO, "GPU WATER: %d river triangles, %d lake triangles; simulation %dx%d at 60 Hz",
             static_cast<int>(bake.rivers.size() / 3), static_cast<int>(pools.size() / 3), kGridX, kGridZ);
    return gpu;
}

void simulationPass(RenderTexture2D target, Shader shader, Texture2D source,
                    std::initializer_list<std::pair<const char*, Texture2D>> textures) {
    BeginTextureMode(target);
    BeginShaderMode(shader);
    for (const auto& [name, texture] : textures) SetShaderValueTexture(shader, GetShaderLocation(shader, name), texture);
    rlDisableColorBlend(); // simulation writes data, including zero alpha
    DrawTexture(source, 0, 0, WHITE); // gl_FragCoord addresses texels; no UV flip
    rlDrawRenderBatchActive();
    rlEnableColorBlend();
    EndShaderMode();
    EndTextureMode();
}

void stepGpuWater(GpuWater& gpu, const std::vector<Cloud>& clouds) {
    simulationPass(gpu.flux, gpu.flowShader, gpu.state[gpu.current].texture,
                   {{"terrain", gpu.terrain}, {"state", gpu.state[gpu.current].texture}, {"routes", gpu.routes}});
    std::array<Vector4, 10> rain{};
    const int count = std::min(10, static_cast<int>(clouds.size()));
    for (int i = 0; i < count; ++i) rain[i] = Vector4{clouds[i].position.x, clouds[i].position.z, clouds[i].size, 0.055F};
    SetShaderValueV(gpu.integrateShader, GetShaderLocation(gpu.integrateShader, "clouds"), rain.data(), SHADER_UNIFORM_VEC4, 10);
    SetShaderValue(gpu.integrateShader, GetShaderLocation(gpu.integrateShader, "cloudCount"), &count, SHADER_UNIFORM_INT);
    SetShaderValue(gpu.integrateShader, GetShaderLocation(gpu.integrateShader, "evaporation"), &gpu.evaporation, SHADER_UNIFORM_FLOAT);
    simulationPass(gpu.state[1 - gpu.current], gpu.integrateShader, gpu.state[gpu.current].texture,
                   {{"terrain", gpu.terrain}, {"state", gpu.state[gpu.current].texture},
                    {"flux", gpu.flux.texture}, {"incomingEdges", gpu.incoming}});
    gpu.current = 1 - gpu.current;
}

void updateGpuWater(GpuWater& gpu, const std::vector<Cloud>& clouds, float deltaTime) {
    gpu.time += std::min(deltaTime, 0.1F);
    gpu.accumulator += std::min(deltaTime, 0.1F);
    while (gpu.accumulator >= kWaterStep) {
        stepGpuWater(gpu, clouds);
        gpu.accumulator -= kWaterStep;
    }
}

void bindWaterTextures(Model& model, GpuWater& gpu) {
    if (model.meshCount == 0) return;
    Material& material = model.materials[0];
    const std::array<std::pair<const char*, Texture2D>, 4> textures{{
        {"state", gpu.state[gpu.current].texture}, {"terrain", gpu.terrain},
        {"flux", gpu.flux.texture}, {"detailedHeight", gpu.detailedHeight}}};
    for (int i = 0; i < 4; ++i) {
        material.maps[i].texture = textures[i].second;
        material.shader.locs[SHADER_LOC_MAP_DIFFUSE + i] = GetShaderLocation(material.shader, textures[i].first);
    }
    SetShaderValue(material.shader, GetShaderLocation(material.shader, "time"), &gpu.time, SHADER_UNIFORM_FLOAT);
}

void drawGpuWater(GpuWater& gpu, const std::vector<Cloud>& clouds, const Camera3D& camera) {
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    for (Model* model : {&gpu.rivers, &gpu.lakes}) {
        bindWaterTextures(*model, gpu);
        if (model->meshCount > 0) DrawModel(*model, Vector3{}, 1.0F, WHITE);
    }
    std::array<Vector4,10> rain{};
    for (std::size_t i = 0; i < clouds.size() && i < rain.size(); ++i) {
        rain[i] = Vector4{clouds[i].position.x, cloudHeight(clouds[i]), clouds[i].position.z, clouds[i].size};
    }
    const Vector3 right = normalize(cross(subtract(camera.target, camera.position), camera.up));
    SetShaderValueV(gpu.rainShader, GetShaderLocation(gpu.rainShader, "rainClouds"), rain.data(), SHADER_UNIFORM_VEC4, 10);
    SetShaderValue(gpu.rainShader, GetShaderLocation(gpu.rainShader, "cameraRight"), &right, SHADER_UNIFORM_VEC3);
    bindWaterTextures(gpu.rain, gpu);
    rlDisableDepthMask();
    if (gpu.rain.meshCount > 0) DrawModel(gpu.rain, Vector3{}, 1.0F, WHITE);
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

void unloadGpuWater(GpuWater& gpu) {
    // UnloadModel frees mesh/material arrays but not their shared textures/shaders.
    for (Model* model : {&gpu.lakes, &gpu.rivers, &gpu.rain, &gpu.cloudSphere}) if (model->meshCount > 0) UnloadModel(*model);
    for (Shader shader : {gpu.flowShader, gpu.integrateShader, gpu.terrainShader, gpu.lakeShader, gpu.riverShader, gpu.rainShader}) UnloadShader(shader);
    for (Texture2D texture : {gpu.terrain, gpu.routes, gpu.incoming, gpu.detailedHeight}) UnloadTexture(texture);
    for (RenderTexture2D target : {gpu.state[0], gpu.state[1], gpu.flux}) UnloadRenderTexture(target);
    gpu = {};
}
