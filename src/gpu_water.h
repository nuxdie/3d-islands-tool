// Included in the application's private namespace after terrain helpers.
constexpr float kWaterStep = 1.0F / 60.0F;
constexpr float kParticleSpacing = 0.16F;

Texture2D floatTexture(int width, int height, const void* pixels) {
    Texture2D texture{rlLoadTexture(pixels,width,height,PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,1),
                      width,height,1,PIXELFORMAT_UNCOMPRESSED_R32G32B32A32};
    if (!texture.id) throw std::runtime_error("GPU water texture allocation failed");
    SetTextureFilter(texture,TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(texture,TEXTURE_WRAP_CLAMP);
    return texture;
}
RenderTexture2D floatTarget() {
    const std::vector<Vector4> zeros(kGridX*kGridZ);
    RenderTexture2D target{};
    target.texture = floatTexture(kGridX,kGridZ,zeros.data());
    target.id = rlLoadFramebuffer();
    rlFramebufferAttach(target.id,target.texture.id,RL_ATTACHMENT_COLOR_CHANNEL0,RL_ATTACHMENT_TEXTURE2D,0);
    if (!rlFramebufferComplete(target.id)) throw std::runtime_error("Wetness framebuffer incomplete");
    rlDisableFramebuffer();
    return target;
}
Shader waterShader(const char* vertex, const char* fragment, const char* extra = "") {
    const std::string vs = std::string(waterShaders::common)+extra+(vertex ? vertex : "");
    const std::string fs = std::string(waterShaders::common)+extra+fragment;
    Shader shader = LoadShaderFromMemory(vertex ? vs.c_str() : nullptr,fs.c_str());
    if (shader.id == rlGetShaderIdDefault()) throw std::runtime_error("Water shader compilation failed");
    const Vector2 size{static_cast<float>(kGridX),static_cast<float>(kGridZ)};
    SetShaderValue(shader,GetShaderLocation(shader,"gridSize"),&size,SHADER_UNIFORM_VEC2);
    SetShaderValue(shader,GetShaderLocation(shader,"volumeMin"),&kVolumeMin,SHADER_UNIFORM_VEC3);
    SetShaderValue(shader,GetShaderLocation(shader,"volumeMax"),&kVolumeMax,SHADER_UNIFORM_VEC3);
    SetShaderValue(shader,GetShaderLocation(shader,"dt"),&kWaterStep,SHADER_UNIFORM_FLOAT);
    return shader;
}
void simulationPass(RenderTexture2D target, Shader shader, Texture2D source,
                    std::initializer_list<std::pair<const char*,Texture2D>> textures) {
    BeginTextureMode(target);
    BeginShaderMode(shader);
    for (const auto& [name,texture] : textures) SetShaderValueTexture(shader,GetShaderLocation(shader,name),texture);
    rlDisableColorBlend();
    DrawTexture(source,0,0,WHITE);
    rlDrawRenderBatchActive();
    rlEnableColorBlend();
    EndShaderMode();
    EndTextureMode();
}

struct GpuWater {
    std::unique_ptr<ParticleFluid> fluid;
    Texture2D terrain{}, detailedHeight{};
    std::array<RenderTexture2D,2> state{}; // persistent visual wetness only
    Shader wetnessShader{}, terrainShader{}, rainShader{};
    Model rain{}, cloudSphere{};
    int current = 0;
    float accumulator = 0;
    float time = 0;
};

GpuWater createGpuWater(const HydrologyMap& map, const std::vector<Cloud>& clouds) {
    GpuWater gpu;
    std::vector<Vector4> terrain(kGridX*kGridZ);
    for (std::size_t i = 0; i < terrain.size(); ++i) terrain[i].x = map.height[i];
    gpu.terrain = floatTexture(kGridX,kGridZ,terrain.data());
    gpu.state = {floatTarget(),floatTarget()};
    gpu.wetnessShader = waterShader(nullptr,waterShaders::wetness);
    gpu.terrainShader = waterShader(waterShaders::terrainVertex,waterShaders::terrainFragment);
    gpu.rainShader = waterShader(waterShaders::rainVertex,waterShaders::rainFragment,waterShaders::rainMotion);

    const int width = (kGridX-1)*3+1, depth = (kGridZ-1)*3+1;
    std::vector<Vector4> detailed(width*depth);
    for (int z = 0; z < depth; ++z) for (int x = 0; x < width; ++x) {
        float height = kVolumeMin.y-1.0F;
        terrainHeight(map,mix(kVolumeMin.x,kVolumeMax.x,static_cast<float>(x)/(width-1)),
                       mix(kVolumeMin.z,kVolumeMax.z,static_cast<float>(z)/(depth-1)),kVolumeMax.y,height);
        detailed[z*width+x].x = height;
    }
    gpu.detailedHeight = floatTexture(width,depth,detailed.data());

    // Seed a thin rain-fed layer beneath each cloud, with separated particles.
    // These are initial/recycled emission positions, never river trajectories.
    std::vector<Vector3> emitters;
    for (const Cloud& cloud : clouds) {
        // Concentrate rain runoff into a source patch instead of placing a
        // uniform water sheet over the whole island. The solver finds its path.
        const float radius = std::clamp(cloud.size*0.28F,0.65F,1.0F);
        const int cells = static_cast<int>(radius/kParticleSpacing);
        for (int z = -cells; z <= cells; ++z) for (int x = -cells; x <= cells; ++x) {
            const float dx = x*kParticleSpacing, dz = z*kParticleSpacing;
            if (dx*dx+dz*dz > radius*radius) continue;
            const float wx = cloud.position.x+dx, wz = cloud.position.z+dz;
            float height = 0;
            if (terrainHeight(map,wx,wz,kVolumeMax.y,height) && height > kVolumeMin.y+1.0F) {
                emitters.push_back(Vector3{wx,height+0.19F,wz});
            }
        }
    }
    // Sparse/very small islands can miss a cloud footprint; keep a valid source.
    if (emitters.empty()) {
        for (int i = 0; i < kGridX*kGridZ; ++i) if (map.height[i] > kVolumeMin.y) {
            Vector3 p = surfaceCellPosition(map,i); p.y += 0.2F; emitters.push_back(p);
        }
    }
    std::vector<Vector4> particles;
    const std::size_t budget = 32768;
    constexpr int layers = 12;
    const std::size_t stride = std::max<std::size_t>(1,(emitters.size()*layers+budget-1)/budget);
    for (int layer = 0; layer < layers; ++layer) for (std::size_t i = 0; i < emitters.size(); i += stride) {
        const auto& p = emitters[i];
        particles.push_back(Vector4{p.x,p.y+layer*kParticleSpacing,p.z,1});
    }
    std::vector<Vector3> triangles;
    triangles.reserve(map.surface.size());
    for (const SurfaceVertex& v : map.surface) triangles.push_back(v.position);
    gpu.fluid = std::make_unique<ParticleFluid>(triangles,particles,kParticleSpacing);

    Mesh rain{};
    rain.vertexCount = static_cast<int>(clouds.size())*90*6;
    rain.triangleCount = rain.vertexCount/3;
    rain.vertices = static_cast<float*>(MemAlloc(rain.vertexCount*3*sizeof(float)));
    rain.texcoords = static_cast<float*>(MemAlloc(rain.vertexCount*2*sizeof(float)));
    int index = 0;
    for (int cloud = 0; cloud < static_cast<int>(clouds.size()); ++cloud) for (int drop = 0; drop < 90; ++drop) {
        for (const Vector2 corner : std::array<Vector2,6>{{{-1,0},{1,0},{1,1},{-1,0},{1,1},{-1,1}}}) {
            rain.vertices[index*3] = static_cast<float>(cloud);
            rain.vertices[index*3+1] = static_cast<float>(drop);
            rain.vertices[index*3+2] = corner.y;
            rain.texcoords[index*2] = corner.x;
            rain.texcoords[index*2+1] = 0;
            ++index;
        }
    }
    UploadMesh(&rain,false);
    gpu.rain = LoadModelFromMesh(rain);
    gpu.rain.materials[0].shader = gpu.rainShader;
    gpu.cloudSphere = LoadModelFromMesh(GenMeshSphere(1.0F,8,12));
    return gpu;
}

void stepGpuWater(GpuWater& gpu, const std::vector<Cloud>& clouds) {
    // Substeps bound contact motion at waterfalls without tying physics to FPS.
    gpu.fluid->step(kWaterStep*0.5F);
    gpu.fluid->step(kWaterStep*0.5F);
    std::array<Vector4,10> rain{};
    const int count = std::min(10,static_cast<int>(clouds.size()));
    for (int i = 0; i < count; ++i) rain[i] = Vector4{clouds[i].position.x,clouds[i].position.z,clouds[i].size,0.055F};
    SetShaderValueV(gpu.wetnessShader,GetShaderLocation(gpu.wetnessShader,"clouds"),rain.data(),SHADER_UNIFORM_VEC4,10);
    SetShaderValue(gpu.wetnessShader,GetShaderLocation(gpu.wetnessShader,"cloudCount"),&count,SHADER_UNIFORM_INT);
    simulationPass(gpu.state[1-gpu.current],gpu.wetnessShader,gpu.state[gpu.current].texture,
                   {{"terrain",gpu.terrain},{"state",gpu.state[gpu.current].texture}});
    gpu.current = 1-gpu.current;
}
void updateGpuWater(GpuWater& gpu, const std::vector<Cloud>& clouds, float dt) {
    gpu.time += std::min(dt,0.1F);
    gpu.accumulator += std::min(dt,0.1F);
    while (gpu.accumulator >= kWaterStep) { stepGpuWater(gpu,clouds); gpu.accumulator -= kWaterStep; }
}
void bindWaterTextures(Model& model, GpuWater& gpu) {
    Material& material = model.materials[0];
    const std::array<std::pair<const char*,Texture2D>,3> textures{{
        {"state",gpu.state[gpu.current].texture},{"terrain",gpu.terrain},{"detailedHeight",gpu.detailedHeight}}};
    for (int i = 0; i < 3; ++i) {
        material.maps[i].texture = textures[i].second;
        material.shader.locs[SHADER_LOC_MAP_DIFFUSE+i] = GetShaderLocation(material.shader,textures[i].first);
    }
    SetShaderValue(material.shader,GetShaderLocation(material.shader,"time"),&gpu.time,SHADER_UNIFORM_FLOAT);
}
void drawGpuWater(GpuWater& gpu, const std::vector<Cloud>& clouds, const Camera3D& camera) {
    gpu.fluid->draw(rlGetMatrixModelview(),rlGetMatrixProjection(),GetScreenWidth(),GetScreenHeight());
    std::array<Vector4,10> rain{};
    for (std::size_t i = 0; i < clouds.size() && i < rain.size(); ++i) {
        rain[i] = Vector4{clouds[i].position.x,clouds[i].position.y,clouds[i].position.z,clouds[i].size};
    }
    const Vector3 right = normalize(cross(subtract(camera.target,camera.position),camera.up));
    SetShaderValueV(gpu.rainShader,GetShaderLocation(gpu.rainShader,"rainClouds"),rain.data(),SHADER_UNIFORM_VEC4,10);
    SetShaderValue(gpu.rainShader,GetShaderLocation(gpu.rainShader,"cameraRight"),&right,SHADER_UNIFORM_VEC3);
    bindWaterTextures(gpu.rain,gpu);
    rlDisableBackfaceCulling(); rlDisableDepthMask();
    DrawModel(gpu.rain,Vector3{},1.0F,WHITE);
    rlEnableDepthMask(); rlEnableBackfaceCulling();
}
void unloadGpuWater(GpuWater& gpu) {
    gpu.fluid.reset();
    for (Model model : {gpu.rain,gpu.cloudSphere}) if (model.meshCount > 0) UnloadModel(model);
    for (Shader shader : {gpu.wetnessShader,gpu.terrainShader,gpu.rainShader}) if (shader.id) UnloadShader(shader);
    for (Texture2D texture : {gpu.terrain,gpu.detailedHeight}) if (texture.id) UnloadTexture(texture);
    for (RenderTexture2D target : gpu.state) if (target.id) UnloadRenderTexture(target);
    gpu = {};
}
