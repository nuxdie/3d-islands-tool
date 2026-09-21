// Landscape geometry retains the closed volumetric island shells, but maps
// their upper surfaces to metre-scale watersheds. Each island has its own tile.
constexpr int kWaterAtlasTile = 512;
constexpr float kDryBed = -100000.0F;

struct LandscapeLayer {
    Vector2 origin{};
    float cellSize = 1;
    int resolution = 256;
    float width = 10000;
    float elevation = 0;
    std::vector<Vector4> terrain; // bed, original upper surface, validity, source m/s
    std::vector<Vector4> initial; // depth, x momentum, z momentum, wetness
    int firstMesh = 0;
};
struct LandscapeData {
    std::vector<LandscapeLayer> layers;
    std::uint32_t seed = 0;
};

Vector4 layerSample(const LandscapeLayer& layer, float x, float z) {
    const float gx = std::clamp((x-layer.origin.x)/layer.cellSize-0.5F,0.0F,static_cast<float>(layer.resolution-1));
    const float gz = std::clamp((z-layer.origin.y)/layer.cellSize-0.5F,0.0F,static_cast<float>(layer.resolution-1));
    const int ix = static_cast<int>(gx), iz = static_cast<int>(gz);
    const int jx = std::min(ix+1,layer.resolution-1), jz = std::min(iz+1,layer.resolution-1);
    const auto blend = [](Vector4 a, Vector4 b, float t) {
        return Vector4{mix(a.x,b.x,t),mix(a.y,b.y,t),mix(a.z,b.z,t),mix(a.w,b.w,t)};
    };
    return blend(blend(layer.terrain[iz*layer.resolution+ix],layer.terrain[iz*layer.resolution+jx],gx-ix),
                 blend(layer.terrain[jz*layer.resolution+ix],layer.terrain[jz*layer.resolution+jx],gx-ix),gz-iz);
}

Mesh uploadSurface(const std::vector<SurfaceVertex>& surface, const LandscapeLayer& layer, int layerIndex, int layerCount) {
    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(surface.size());
    mesh.triangleCount = mesh.vertexCount/3;
    mesh.vertices = static_cast<float*>(MemAlloc(mesh.vertexCount*3*sizeof(float)));
    mesh.normals = static_cast<float*>(MemAlloc(mesh.vertexCount*3*sizeof(float)));
    mesh.texcoords = static_cast<float*>(MemAlloc(mesh.vertexCount*2*sizeof(float)));
    for (int i = 0; i < mesh.vertexCount; ++i) {
        const auto& v = surface[i];
        std::memcpy(mesh.vertices+i*3,&v.position,sizeof(Vector3));
        std::memcpy(mesh.normals+i*3,&v.normal,sizeof(Vector3));
        mesh.texcoords[i*2] = (v.position.x-layer.origin.x)/(layer.cellSize*kWaterAtlasTile);
        mesh.texcoords[i*2+1] = (layerIndex*kWaterAtlasTile+(v.position.z-layer.origin.y)/layer.cellSize)/(kWaterAtlasTile*static_cast<float>(layerCount));
    }
    UploadMesh(&mesh,false);
    return mesh;
}

Model createLandscape(std::uint32_t seed, const GenerationSettings& settings,
                      int& triangleCount, int& islandCount, LandscapeData& data) {
    data = {};
    data.seed = seed;
    islandCount = settings.islandCount;
    data.layers.resize(islandCount);
    Model model{};
    model.transform = MatrixIdentity();
    model.meshCount = islandCount*4;
    model.materialCount = 1;
    model.meshes = static_cast<Mesh*>(MemAlloc(model.meshCount*sizeof(Mesh)));
    std::memset(model.meshes,0,model.meshCount*sizeof(Mesh));
    model.meshMaterial = static_cast<int*>(MemAlloc(model.meshCount*sizeof(int)));
    std::memset(model.meshMaterial,0,model.meshCount*sizeof(int));
    model.materials = static_cast<Material*>(MemAlloc(sizeof(Material)));
    model.materials[0] = LoadMaterialDefault();
    triangleCount = 0;
    for (int index = 0; index < islandCount; ++index) {
        const std::uint32_t localSeed = seed+static_cast<std::uint32_t>(index)*7919U;
        GenerationSettings local = settings;
        local.islandCount = 1;
        local.islandScale = 0.9F;
        HydrologyMap raw;
        int ignored = 0;
        auto shell = extractIslands(localSeed,local,ignored,raw);
        Vector3 lo{1000,1000,1000}, hi{-1000,-1000,-1000};
        for (const auto& v : shell) {
            lo = Vector3{std::min(lo.x,v.position.x),std::min(lo.y,v.position.y),std::min(lo.z,v.position.z)};
            hi = Vector3{std::max(hi.x,v.position.x),std::max(hi.y,v.position.y),std::max(hi.z,v.position.z)};
        }
        auto& layer = data.layers[index];
        layer.resolution = index == 0 ? 512 : 256;
        // Eight percent padding leaves dry boundary cells around a genuinely
        // 10,000-metre-wide main landmass, not a 10 km label on a small mesh.
        layer.width = (index == 0 ? 10000.0F : 2400.0F+randomAt(index,1,0,seed)*1400.0F)*settings.islandScale/0.92F;
        layer.cellSize = layer.width/layer.resolution;
        const float angle = index*2.399963F;
        const Vector2 center = index == 0 ? Vector2{0,0} : Vector2{std::cos(angle)*2900.0F,std::sin(angle)*2900.0F};
        layer.origin = Vector2{center.x-layer.width*0.5F,center.y-layer.width*0.5F};
        layer.elevation = index == 0 ? 400.0F : 1700.0F+index*260.0F;
        layer.firstMesh = index*4;
        const float sx = layer.width*0.92F/(hi.x-lo.x), sz = layer.width*0.92F/(hi.z-lo.z);
        const float sy = (index == 0 ? 750.0F : 420.0F)*settings.verticalScale/(hi.y-lo.y);
        const Vector2 rawCenter{(lo.x+hi.x)*0.5F,(lo.z+hi.z)*0.5F};
        const float phase = randomAt(index,0,0,seed)*6.283185F;
        const auto riverX = [&](float t) { return center.x+layer.width*(0.075F*std::sin(t*7+phase)+0.028F*std::sin(t*19+phase)); };
        const auto riverBed = [&](float t) { return layer.elevation+150.0F*(1.0F-t)*settings.verticalScale; };
        const float lakeT = 0.59F;
        const float lakeX = riverX(lakeT), lakeZ = layer.origin.y+lakeT*layer.width;
        const float lakeLevel = riverBed(lakeT+0.065F);
        const int n = layer.resolution;
        layer.terrain.resize(n*n);
        layer.initial.resize(n*n);
        for (int z = 0; z < n; ++z) for (int x = 0; x < n; ++x) {
            const float wx = layer.origin.x+(x+0.5F)*layer.cellSize;
            const float wz = layer.origin.y+(z+0.5F)*layer.cellSize;
            const float rx = rawCenter.x+(wx-center.x)/sx, rz = rawCenter.y+(wz-center.y)/sz;
            float top = kVolumeMin.y-1;
            const bool valid = terrainHeight(raw,rx,rz,kVolumeMax.y,top);
            const float t = (z+0.5F)/n;
            const float width = std::max(layer.cellSize*2.2F,layer.width*0.006F);
            const float d = std::abs(wx-riverX(t));
            const float noise = fractalNoise3d(wx*0.0012F,0,wz*0.0012F,localSeed,5);
            const float ridges = std::abs(fractalNoise3d(wx*0.0007F,0,wz*0.0007F,localSeed+17,4)-0.5F)*2;
            float bed = riverBed(t)+60+settings.roughness*(noise*500+ridges*420)*settings.verticalScale;
            const float channel = riverBed(t)+3.0F*(d/width)*(d/width);
            bed = mix(std::min(bed,channel),bed,smoothstep(width*2,width*8,d));
            const float lx = (wx-lakeX)/(layer.width*0.045F);
            const float lz = (wz-lakeZ)/(layer.width*0.085F);
            const float lr = std::sqrt(lx*lx+lz*lz);
            if (lr < 1.1F) bed = std::min(bed,lakeLevel-12.0F+14.0F*lr*lr);
            float water = std::max(0.0F,riverBed(t)+0.8F-bed);
            // A lake has one surface elevation; do not fill its upstream end
            // to the higher river head and create an initial artificial surge.
            if (lr < 1.1F) water = std::max(0.0F,lakeLevel-bed);
            const float sourceDistance = std::hypot(wx-riverX(0.25F),wz-(layer.origin.y+0.25F*layer.width));
            const float sourceRadius = width*2.5F;
            const float source = sourceDistance < sourceRadius ? 12.0F/(3.141593F*sourceRadius*sourceRadius) : 0;
            layer.terrain[z*n+x] = Vector4{bed,top,valid ? 1.0F : 0.0F,valid ? source : 0.0F};
            layer.initial[z*n+x] = valid ? Vector4{water,0,water>0 && lr>1 ? water*0.4F : 0,water>0 ? 1.0F : 0.0F} : Vector4{};
        }
        // Fill raw-top values outside the silhouette from nearby valid samples.
        // This keeps the volumetric shell warp continuous at its boundary.
        for (int pass = 0; pass < 4; ++pass) {
            auto next = layer.terrain;
            for (int z = 1; z < n-1; ++z) for (int x = 1; x < n-1; ++x) if (layer.terrain[z*n+x].y < kVolumeMin.y) {
                float sum = 0; int count = 0;
                for (int o : {-1,1,-n,n}) if (layer.terrain[z*n+x+o].y > kVolumeMin.y) { sum += layer.terrain[z*n+x+o].y; ++count; }
                if (count) next[z*n+x].y = sum/count;
            }
            layer.terrain.swap(next);
        }
        auto warp = [&](SurfaceVertex v) {
            const float x = center.x+(v.position.x-rawCenter.x)*sx;
            const float z = center.y+(v.position.z-rawCenter.y)*sz;
            auto h = layerSample(layer,x,z);
            if (h.y < kVolumeMin.y) h.y = v.position.y;
            const auto heightOffset = [&](float px,float pz) { auto s = layerSample(layer,px,pz); return s.x-sy*s.y; };
            const float e = layer.cellSize;
            const float dx = (heightOffset(x+e,z)-heightOffset(x-e,z))/(2*e);
            const float dz = (heightOffset(x,z+e)-heightOffset(x,z-e))/(2*e);
            v.normal = normalize(Vector3{v.normal.x/sx-dx*v.normal.y/sy,v.normal.y/sy,v.normal.z/sz-dz*v.normal.y/sy});
            v.position = Vector3{x,h.x+std::min(0.0F,v.position.y-h.y)*sy-layer.cellSize*1.5F,z};
            return v;
        };
        // Three whole-island LODs avoid cracks between independently tessellated
        // volumetric chunks. Water resolution is independent of geometry LOD.
        std::vector<SurfaceVertex> refined = shell;
        for (int level = 0; level <= 3; ++level) {
            if (level >= 1) {
                std::vector<SurfaceVertex> mapped;
                mapped.reserve(refined.size());
                for (auto v : refined) mapped.push_back(warp(v));
                model.meshes[index*4+(3-level)] = uploadSurface(mapped,layer,index,islandCount);
                if (level == 3) triangleCount += static_cast<int>(mapped.size()/3);
            }
            if (level == 3) break;
            std::vector<SurfaceVertex> next;
            next.reserve(refined.size()*4);
            const auto midpoint = [](SurfaceVertex a,SurfaceVertex b) { return SurfaceVertex{mix(a.position,b.position,0.5F),normalize(mix(a.normal,b.normal,0.5F))}; };
            for (std::size_t i = 0; i < refined.size(); i += 3) {
                auto a = refined[i], b = refined[i+1], c = refined[i+2];
                auto ab = midpoint(a,b), bc = midpoint(b,c), ca = midpoint(c,a);
                for (auto v : {a,ab,ca,ab,b,bc,ca,bc,c,ab,bc,ca}) next.push_back(v);
            }
            refined.swap(next);
        }
        // The watershed cap shares the exact grid/triangulation used by water.
        // This resolves narrow beds instead of letting coarse shell triangles
        // bridge over them. The closed volumetric shell remains underneath.
        std::vector<SurfaceVertex> cap;
        for (int z=0;z<n-1;++z) for (int x=0;x<n-1;++x) {
            if (layer.terrain[z*n+x].z<0.5F && layer.terrain[z*n+x+1].z<0.5F &&
                layer.terrain[(z+1)*n+x].z<0.5F && layer.terrain[(z+1)*n+x+1].z<0.5F) continue;
            for (auto offset:std::array<std::array<int,2>,6>{{{0,0},{0,1},{1,1},{0,0},{1,1},{1,0}}}) {
                const int px=x+offset[0], pz=z+offset[1];
                const auto bed=layer.terrain[pz*n+px];
                const float dx=(layer.terrain[pz*n+std::min(px+1,n-1)].x-layer.terrain[pz*n+std::max(px-1,0)].x)/(2*layer.cellSize);
                const float dz=(layer.terrain[std::min(pz+1,n-1)*n+px].x-layer.terrain[std::max(pz-1,0)*n+px].x)/(2*layer.cellSize);
                cap.push_back(SurfaceVertex{Vector3{layer.origin.x+(px+0.5F)*layer.cellSize,
                    bed.x-(bed.z<0.5F?layer.cellSize*2.0F:0.0F),layer.origin.y+(pz+0.5F)*layer.cellSize},normalize(Vector3{-dx,1,-dz})});
            }
        }
        model.meshes[index*4+3]=uploadSurface(cap,layer,index,islandCount);
    }
    return model;
}

void drawLandscape(const Model& model, const LandscapeData& data, const Camera3D& camera, bool wireframe, int& triangles) {
    triangles = 0;
    for (std::size_t i = 0; i < data.layers.size(); ++i) {
        const auto& layer = data.layers[i];
        const Vector3 center{layer.origin.x+layer.width*0.5F,layer.elevation,layer.origin.y+layer.width*0.5F};
        const auto delta = subtract(camera.position,center);
        const float distance = std::sqrt(dot(delta,delta));
        int lod = distance < layer.width*2.0F ? 0 : distance < layer.width*4.0F ? 1 : 2;
        if (camera.position.y>layer.elevation+1200) lod=std::max(lod,1);
        const Mesh& mesh = model.meshes[layer.firstMesh+lod];
        if (wireframe) rlEnableWireMode();
        DrawMesh(mesh,model.materials[0],MatrixIdentity());
        if (wireframe) rlDisableWireMode();
        triangles += mesh.triangleCount;
        const auto& cap=model.meshes[layer.firstMesh+3];
        if (wireframe) rlEnableWireMode();
        DrawMesh(cap,model.materials[0],MatrixIdentity());
        if (wireframe) rlDisableWireMode();
        triangles+=cap.triangleCount;
    }
}
