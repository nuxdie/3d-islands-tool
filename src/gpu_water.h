constexpr float kWaterStep = 1.0F/60.0F;
constexpr std::array<std::array<int,2>,4> kDirections{{{-1,0},{1,0},{0,-1},{0,1}}};

Texture2D atlasTexture(int width,int height,const void* pixels) {
    Texture2D t{rlLoadTexture(pixels,width,height,PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,1),width,height,1,PIXELFORMAT_UNCOMPRESSED_R32G32B32A32};
    if (!t.id) throw std::runtime_error("Shallow-water texture allocation failed");
    SetTextureFilter(t,TEXTURE_FILTER_BILINEAR); SetTextureWrap(t,TEXTURE_WRAP_CLAMP);
    return t;
}
RenderTexture2D atlasTarget(int layers,const void* pixels) {
    RenderTexture2D target{};
    target.texture=atlasTexture(kWaterAtlasTile,kWaterAtlasTile*layers,pixels);
    target.id=rlLoadFramebuffer();
    rlFramebufferAttach(target.id,target.texture.id,RL_ATTACHMENT_COLOR_CHANNEL0,RL_ATTACHMENT_TEXTURE2D,0);
    if (!rlFramebufferComplete(target.id)) throw std::runtime_error("Shallow-water framebuffer incomplete");
    rlDisableFramebuffer();
    return target;
}
Shader shallowShader(const char* vertex,const char* fragment,const LandscapeData& data,const char* extra="") {
    const std::string vs=std::string(shallowShaders::common)+extra+(vertex?vertex:"");
    const std::string fs=std::string(shallowShaders::common)+extra+fragment;
    Shader shader=LoadShaderFromMemory(vertex?vs.c_str():nullptr,fs.c_str());
    if (shader.id==rlGetShaderIdDefault()) throw std::runtime_error("Shallow-water shader compilation failed");
    std::array<Vector4,10> info{};
    for (std::size_t i=0;i<data.layers.size();++i) {
        const auto& l=data.layers[i];
        info[i]={l.origin.x,l.origin.y,l.cellSize,static_cast<float>(l.resolution)};
    }
    const int count=static_cast<int>(data.layers.size());
    SetShaderValueV(shader,GetShaderLocation(shader,"tiles"),info.data(),SHADER_UNIFORM_VEC4,10);
    SetShaderValue(shader,GetShaderLocation(shader,"layerCount"),&count,SHADER_UNIFORM_INT);
    SetShaderValue(shader,GetShaderLocation(shader,"dt"),&kWaterStep,SHADER_UNIFORM_FLOAT);
    return shader;
}
void shallowPass(RenderTexture2D target,Shader shader,Texture2D source,
                 std::initializer_list<std::pair<const char*,Texture2D>> inputs) {
    BeginTextureMode(target); BeginShaderMode(shader);
    for (const auto& [name,t]:inputs) SetShaderValueTexture(shader,GetShaderLocation(shader,name),t);
    rlDisableColorBlend();
    DrawTexture(source,0,0,WHITE);
    rlDrawRenderBatchActive(); rlEnableColorBlend();
    EndShaderMode(); EndTextureMode();
}

struct FallVertex { Vector3 position, direction; Vector2 source, path; };
struct WaterRouting {
    std::vector<Vector4> info, edges;
    std::vector<FallVertex> vertices;
    int transfers=0;
};
WaterRouting buildWaterRouting(const LandscapeData& data) {
    WaterRouting result;
    const int count=static_cast<int>(data.layers.size()), atlasWidth=kWaterAtlasTile;
    result.info.resize(atlasWidth*atlasWidth*count);
    std::vector<std::vector<Vector4>> incoming(result.info.size());
    for (int l=0;l<count;++l) {
        const auto& tile=data.layers[l];
        const int n=tile.resolution;
        for (int z=1;z<n-1;++z) for (int x=1;x<n-1;++x) {
            const auto ground=tile.terrain[z*n+x];
            if (ground.z<0.5F) continue;
            for (int d=0;d<4;++d) {
                const auto delta=kDirections[d];
                if (tile.terrain[(z+delta[1])*n+x+delta[0]].z>0.5F) continue;
                const Vector3 lip{tile.origin.x+(x+0.5F+delta[0]*0.5F)*tile.cellSize,ground.x,
                                  tile.origin.y+(z+0.5F+delta[1]*0.5F)*tile.cellSize};
                const float tx=lip.x+delta[0]*tile.cellSize*2, tz=lip.z+delta[1]*tile.cellSize*2;
                float landing=-1800;
                int receiver=-1;
                float receiverSize=1;
                for (int r=0;r<count;++r) if (r!=l) {
                    const auto& target=data.layers[r];
                    const int rx=static_cast<int>(std::floor((tx-target.origin.x)/target.cellSize));
                    const int rz=static_cast<int>(std::floor((tz-target.origin.y)/target.cellSize));
                    if (rx<0 || rz<0 || rx>=target.resolution || rz>=target.resolution) continue;
                    const auto candidate=target.terrain[rz*target.resolution+rx];
                    if (candidate.z>0.5F && candidate.x<ground.x-20 && candidate.x>landing) {
                        receiver=(r*atlasWidth+rz)*atlasWidth+rx; landing=candidate.x; receiverSize=target.cellSize;
                    }
                }
                int fx=x-(d==0?1:0), fz=z-(d==2?1:0);
                const int face=(l*atlasWidth+fz)*atlasWidth+fx;
                if (receiver>=0) {
                    incoming[receiver].push_back(Vector4{static_cast<float>(face),d<2?0.0F:1.0F,
                        (d==0||d==2)?-1.0F:1.0F,tile.cellSize/(receiverSize*receiverSize)});
                    ++result.transfers;
                }
                const Vector2 source{(x+0.5F)/atlasWidth,(l*atlasWidth+z+0.5F)/(atlasWidth*count)};
                const Vector3 tangent{static_cast<float>(-delta[1]),0,static_cast<float>(delta[0])};
                constexpr int segments=20;
                for (int segment=0;segment<segments;++segment) {
                    for (auto corner:std::array<Vector2,6>{{{-0.5F,0},{-0.5F,1},{0.5F,1},{-0.5F,0},{0.5F,1},{0.5F,0}}}) {
                        Vector3 p{lip.x+tangent.x*tile.cellSize*corner.x,lip.y,lip.z+tangent.z*tile.cellSize*corner.x};
                        p.y=layerSample(tile,p.x,p.z).x;
                        result.vertices.push_back(FallVertex{p,Vector3{static_cast<float>(delta[0]),landing+0.2F,static_cast<float>(delta[1])},
                            source,Vector2{static_cast<float>(d),(segment+corner.y)/segments}});
                    }
                }
            }
        }
    }
    for (std::size_t i=0;i<incoming.size();++i) {
        result.info[i]=Vector4{static_cast<float>(result.edges.size()),static_cast<float>(incoming[i].size()),0,0};
        result.edges.insert(result.edges.end(),incoming[i].begin(),incoming[i].end());
    }
    result.edges.resize(std::max<std::size_t>(1,(result.edges.size()+1023)/1024)*1024);
    return result;
}

struct GpuWater {
    Texture2D terrain{}, incomingInfo{}, incomingEdges{};
    std::array<RenderTexture2D,2> state{};
    RenderTexture2D xFlux{}, zFlux{};
    Shader fluxShader{}, integrateShader{}, terrainShader{}, waterShader{}, fallShader{}, rainShader{};
    Model surface{}, falls{}, rain{};
    int current=0;
    float accumulator=0, time=0;
    float rainfall=0.000002F; // 7.2 mm/hour, in metres/second
};

GpuWater createGpuWater(const LandscapeData& data) {
    GpuWater gpu;
    const int count=static_cast<int>(data.layers.size());
    std::vector<Vector4> terrain(kWaterAtlasTile*kWaterAtlasTile*count), initial(terrain.size()), zeros(terrain.size());
    std::vector<SurfaceVertex> waterVertices;
    std::vector<Vector2> waterUV;
    for (int l=0;l<count;++l) {
        const auto& tile=data.layers[l]; const int n=tile.resolution;
        for (int z=0;z<n;++z) for (int x=0;x<n;++x) {
            const int id=(l*kWaterAtlasTile+z)*kWaterAtlasTile+x;
            terrain[id]=tile.terrain[z*n+x]; initial[id]=tile.initial[z*n+x];
        }
        for (int z=0;z<n-1;++z) for (int x=0;x<n-1;++x) {
            if (tile.terrain[z*n+x].z<0.5F && tile.terrain[z*n+x+1].z<0.5F && tile.terrain[(z+1)*n+x].z<0.5F && tile.terrain[(z+1)*n+x+1].z<0.5F) continue;
            for (auto c:std::array<std::array<int,2>,6>{{{0,0},{0,1},{1,1},{0,0},{1,1},{1,0}}}) {
                const float px=x+c[0]+0.5F, pz=z+c[1]+0.5F;
                waterVertices.push_back(SurfaceVertex{Vector3{tile.origin.x+px*tile.cellSize,0,tile.origin.y+pz*tile.cellSize},Vector3{0,1,0}});
                waterUV.push_back(Vector2{px/kWaterAtlasTile,(l*kWaterAtlasTile+pz)/(kWaterAtlasTile*count)});
            }
        }
    }
    gpu.terrain=atlasTexture(kWaterAtlasTile,kWaterAtlasTile*count,terrain.data());
    gpu.state={atlasTarget(count,initial.data()),atlasTarget(count,initial.data())};
    gpu.xFlux=atlasTarget(count,zeros.data()); gpu.zFlux=atlasTarget(count,zeros.data());
    gpu.fluxShader=shallowShader(nullptr,shallowShaders::flux,data);
    gpu.integrateShader=shallowShader(nullptr,shallowShaders::integrate,data);
    gpu.terrainShader=shallowShader(shallowShaders::terrainVertex,shallowShaders::terrainFragment,data);
    gpu.waterShader=shallowShader(shallowShaders::waterVertex,shallowShaders::waterFragment,data);
    gpu.fallShader=shallowShader(shallowShaders::fallVertex,shallowShaders::fallFragment,data);
    gpu.rainShader=shallowShader(shallowShaders::rainVertex,shallowShaders::rainFragment,data,shallowShaders::rainMotion);
    WaterRouting routing=buildWaterRouting(data);
    gpu.incomingInfo=atlasTexture(kWaterAtlasTile,kWaterAtlasTile*count,routing.info.data());
    gpu.incomingEdges=atlasTexture(1024,static_cast<int>(routing.edges.size()/1024),routing.edges.data());
    const auto meshFrom=[&](bool falls) {
        Mesh mesh{};
        mesh.vertexCount=static_cast<int>(falls?routing.vertices.size():waterVertices.size()); mesh.triangleCount=mesh.vertexCount/3;
        if (!mesh.vertexCount) return Model{};
        mesh.vertices=static_cast<float*>(MemAlloc(mesh.vertexCount*3*sizeof(float)));
        mesh.normals=static_cast<float*>(MemAlloc(mesh.vertexCount*3*sizeof(float)));
        mesh.texcoords=static_cast<float*>(MemAlloc(mesh.vertexCount*2*sizeof(float)));
        if (falls) mesh.texcoords2=static_cast<float*>(MemAlloc(mesh.vertexCount*2*sizeof(float)));
        for (int i=0;i<mesh.vertexCount;++i) {
            const auto p=falls?routing.vertices[i].position:waterVertices[i].position;
            const auto n=falls?routing.vertices[i].direction:waterVertices[i].normal;
            const auto uv=falls?routing.vertices[i].source:waterUV[i];
            std::memcpy(mesh.vertices+i*3,&p,sizeof(Vector3)); std::memcpy(mesh.normals+i*3,&n,sizeof(Vector3));
            std::memcpy(mesh.texcoords+i*2,&uv,sizeof(Vector2));
            if (falls) std::memcpy(mesh.texcoords2+i*2,&routing.vertices[i].path,sizeof(Vector2));
        }
        UploadMesh(&mesh,false);
        Model model=LoadModelFromMesh(mesh); model.materials[0].shader=falls?gpu.fallShader:gpu.waterShader;
        return model;
    };
    gpu.surface=meshFrom(false); gpu.falls=meshFrom(true);
    Mesh rain{}; rain.vertexCount=240*6; rain.triangleCount=rain.vertexCount/3;
    rain.vertices=static_cast<float*>(MemAlloc(rain.vertexCount*3*sizeof(float)));
    rain.texcoords=static_cast<float*>(MemAlloc(rain.vertexCount*2*sizeof(float)));
    int rainVertex=0;
    for (int i=0;i<240;++i) for (auto corner:std::array<Vector2,6>{{{-1,0},{1,0},{1,1},{-1,0},{1,1},{-1,1}}}) {
        rain.vertices[rainVertex*3]=static_cast<float>(i);
        rain.vertices[rainVertex*3+1]=rain.vertices[rainVertex*3+2]=0;
        std::memcpy(rain.texcoords+rainVertex*2,&corner,sizeof(Vector2)); ++rainVertex;
    }
    UploadMesh(&rain,false); gpu.rain=LoadModelFromMesh(rain); gpu.rain.materials[0].shader=gpu.rainShader;
    TraceLog(LOG_INFO,"SHALLOW WATER: %d independent metre-scaled tiles, %d downhill inter-island transfers",count,routing.transfers);
    return gpu;
}

void stepGpuWater(GpuWater& gpu) {
    const Texture2D state=gpu.state[gpu.current].texture;
    int axis=0;
    SetShaderValue(gpu.fluxShader,GetShaderLocation(gpu.fluxShader,"axis"),&axis,SHADER_UNIFORM_INT);
    shallowPass(gpu.xFlux,gpu.fluxShader,state,{{"bedTexture",gpu.terrain},{"stateTexture",state}});
    axis=1; SetShaderValue(gpu.fluxShader,GetShaderLocation(gpu.fluxShader,"axis"),&axis,SHADER_UNIFORM_INT);
    shallowPass(gpu.zFlux,gpu.fluxShader,state,{{"bedTexture",gpu.terrain},{"stateTexture",state}});
    SetShaderValue(gpu.integrateShader,GetShaderLocation(gpu.integrateShader,"rainfall"),&gpu.rainfall,SHADER_UNIFORM_FLOAT);
    // The update needs six samplers, beyond raylib's four auxiliary batch slots.
    // Bind them explicitly for this pass after flushing the batch.
    BeginTextureMode(gpu.state[1-gpu.current]); BeginShaderMode(gpu.integrateShader);
    rlDrawRenderBatchActive();
    const std::array<std::pair<const char*,Texture2D>,6> textures{{{"bedTexture",gpu.terrain},{"stateTexture",state},
        {"fluxX",gpu.xFlux.texture},{"fluxZ",gpu.zFlux.texture},{"incomingInfo",gpu.incomingInfo},{"incomingEdges",gpu.incomingEdges}}};
    rlEnableShader(gpu.integrateShader.id);
    for (int i=0;i<6;++i) {
        int unit=i+1; rlActiveTextureSlot(unit); rlEnableTexture(textures[i].second.id);
        rlSetUniform(GetShaderLocation(gpu.integrateShader,textures[i].first),&unit,SHADER_UNIFORM_INT,1);
    }
    rlActiveTextureSlot(0); rlDisableColorBlend(); DrawTexture(state,0,0,WHITE); rlDrawRenderBatchActive();
    rlEnableColorBlend();
    for (int i=1;i<=6;++i) { rlActiveTextureSlot(i); rlDisableTexture(); }
    rlActiveTextureSlot(0); EndShaderMode(); EndTextureMode();
    gpu.current=1-gpu.current;
}
void updateGpuWater(GpuWater& gpu,float delta) {
    gpu.time+=std::min(delta,0.1F); gpu.accumulator+=std::min(delta,0.1F);
    while (gpu.accumulator>=kWaterStep) { stepGpuWater(gpu); gpu.accumulator-=kWaterStep; }
}
void bindWaterTextures(Model& model,GpuWater& gpu,const Camera3D& camera) {
    if (!model.meshCount) return;
    Material& material=model.materials[0];
    const std::array<std::pair<const char*,Texture2D>,4> inputs{{{"bedTexture",gpu.terrain},{"stateTexture",gpu.state[gpu.current].texture},{"fluxX",gpu.xFlux.texture},{"fluxZ",gpu.zFlux.texture}}};
    for (int i=0;i<4;++i) {
        material.maps[i].texture=inputs[i].second;
        material.shader.locs[SHADER_LOC_MAP_DIFFUSE+i]=GetShaderLocation(material.shader,inputs[i].first);
    }
    SetShaderValue(material.shader,GetShaderLocation(material.shader,"time"),&gpu.time,SHADER_UNIFORM_FLOAT);
    SetShaderValue(material.shader,GetShaderLocation(material.shader,"eye"),&camera.position,SHADER_UNIFORM_VEC3);
}
void drawGpuWater(GpuWater& gpu,const Camera3D& camera) {
    rlDrawRenderBatchActive(); rlDisableBackfaceCulling();
    for (Model* model:{&gpu.surface,&gpu.falls}) {
        if (!model->meshCount) continue;
        bindWaterTextures(*model,gpu,camera); DrawModel(*model,Vector3{},1,WHITE);
    }
    bindWaterTextures(gpu.rain,gpu,camera);
    const auto right=normalize(cross(subtract(camera.target,camera.position),camera.up));
    SetShaderValue(gpu.rainShader,GetShaderLocation(gpu.rainShader,"cameraRight"),&right,SHADER_UNIFORM_VEC3);
    rlDisableDepthMask(); DrawModel(gpu.rain,Vector3{},1,WHITE); rlEnableDepthMask();
    rlEnableBackfaceCulling();
}
void unloadGpuWater(GpuWater& gpu) {
    for (Model model:{gpu.surface,gpu.falls,gpu.rain}) if (model.meshCount) UnloadModel(model);
    for (Shader shader:{gpu.fluxShader,gpu.integrateShader,gpu.terrainShader,gpu.waterShader,gpu.fallShader,gpu.rainShader}) if (shader.id) UnloadShader(shader);
    for (Texture2D t:{gpu.terrain,gpu.incomingInfo,gpu.incomingEdges}) if (t.id) UnloadTexture(t);
    for (RenderTexture2D t:{gpu.state[0],gpu.state[1],gpu.xFlux,gpu.zFlux}) if (t.id) UnloadRenderTexture(t);
    gpu={};
}
