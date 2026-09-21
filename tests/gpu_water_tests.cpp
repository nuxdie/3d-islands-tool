#define main islandsApplicationMain
#include "../src/main.cpp"
#undef main
#include <iostream>

void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
LandscapeLayer fixture(int n,float dx) {
    LandscapeLayer l; l.resolution=n; l.cellSize=dx; l.width=n*dx; l.origin={-l.width/2,-l.width/2};
    l.terrain.assign(n*n,Vector4{20,0,1,0}); l.initial.resize(n*n); return l;
}
std::vector<Vector4> readState(const GpuWater& gpu) {
    const auto texture=gpu.state[gpu.current].texture;
    auto* pixels=static_cast<Vector4*>(rlReadTexturePixels(texture.id,texture.width,texture.height,texture.format));
    require(pixels,"GPU readback failed");
    std::vector<Vector4> result(pixels,pixels+texture.width*texture.height); MemFree(pixels); return result;
}
double volume(const std::vector<Vector4>& state,const LandscapeData& data) {
    double sum=0;
    for (std::size_t l=0;l<data.layers.size();++l) {
        const auto& tile=data.layers[l];
        for (int z=0;z<tile.resolution;++z) for (int x=0;x<tile.resolution;++x) {
            const auto p=state[(l*kWaterAtlasTile+z)*kWaterAtlasTile+x];
            require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&p.x>=0,"Invalid shallow-water state");
            sum+=p.x*tile.cellSize*tile.cellSize;
        }
    }
    return sum;
}
void lakeAtRest() {
    LandscapeData data; data.layers.push_back(fixture(20,3));
    auto& l=data.layers[0];
    for (int z=3;z<17;++z) for (int x=3;x<17;++x) {
        const float bed=1.5F+std::sin(x*0.8F)*std::cos(z*0.6F);
        l.terrain[z*20+x].x=bed; l.initial[z*20+x]={5-bed,0,0,1};
    }
    GpuWater gpu=createGpuWater(data); gpu.rainfall=0;
    const auto initial=readState(gpu); const double before=volume(initial,data);
    for (int i=0;i<180;++i) stepGpuWater(gpu);
    const auto final=readState(gpu);
    require(std::abs(volume(final,data)-before)<before*0.00002,"Lake at rest lost water");
    for (int z=3;z<17;++z) for (int x=3;x<17;++x) {
        const auto s=final[z*kWaterAtlasTile+x];
        require(std::abs(s.x+l.terrain[z*20+x].x-5)<0.0005,"Hydrostatic lake surface drifted");
        require(std::abs(s.y)+std::abs(s.z)<0.002,"Lake at rest developed artificial currents");
    }
    unloadGpuWater(gpu);
    std::cout<<"Well-balanced lake stays level over uneven terrain\n";
}
void mergingAndMomentum() {
    LandscapeData data; data.layers.push_back(fixture(20,2)); auto& l=data.layers[0];
    for (int z=3;z<17;++z) for (int x=3;x<17;++x) {
        l.terrain[z*20+x].x=(x==10?0.25F:0);
        if ((x<8||x>12)&&z>=6&&z<=13) l.initial[z*20+x].x=3;
    }
    GpuWater gpu=createGpuWater(data); gpu.rainfall=0;
    const double before=volume(readState(gpu),data);
    for (int i=0;i<20;++i) stepGpuWater(gpu);
    double momentum=0;
    for (const auto& s:readState(gpu)) momentum+=std::abs(s.y)+std::abs(s.z);
    require(momentum>1,"Solver has no transported water momentum");
    for (int i=0;i<600;++i) stepGpuWater(gpu);
    const auto result=readState(gpu);
    require(std::abs(volume(result,data)-before)<before*0.0001,"Basin transfer did not conserve volume");
    require(result[10*kWaterAtlasTile+10].x>0.15F,"Pools did not merge across their saddle");
    Camera3D camera{{30,35,35},{0,1,0},{0,1,0},45,CAMERA_PERSPECTIVE};
    RenderTexture2D target=LoadRenderTexture(320,240);
    BeginTextureMode(target); ClearBackground(BLACK); BeginMode3D(camera);
    drawGpuWater(gpu,camera); EndMode3D(); EndTextureMode();
    Image image=LoadImageFromTexture(target.texture); Color* colors=LoadImageColors(image); int visible=0;
    for (int i=0;i<image.width*image.height;++i) visible+=colors[i].b>20;
    require(visible>200,"Connected water surface did not render");
    UnloadImageColors(colors); UnloadImage(image); UnloadRenderTexture(target); unloadGpuWater(gpu);
    std::cout<<"Pools merge, carry momentum, conserve volume and render\n";
}
void layerTransfers() {
    LandscapeData data; data.layers.push_back(fixture(24,4)); data.layers.push_back(fixture(20,2));
    auto& lower=data.layers[0]; auto& upper=data.layers[1];
    for (int z=2;z<22;++z) for (int x=2;x<22;++x) lower.terrain[z*24+x].x=0;
    for (auto& p:upper.terrain) p={100,0,0,0};
    for (int z=8;z<12;++z) for (int x=8;x<12;++x) { upper.terrain[z*20+x].z=1; upper.initial[z*20+x].x=1; }
    const auto routing=buildWaterRouting(data); require(routing.transfers>0,"No cross-layer outlets were found");
    GpuWater gpu=createGpuWater(data); gpu.rainfall=0;
    const double before=volume(readState(gpu),data);
    for (int i=0;i<120;++i) stepGpuWater(gpu);
    const auto result=readState(gpu); double received=0;
    for (int z=0;z<24;++z) for (int x=0;x<24;++x) received+=result[z*kWaterAtlasTile+x].x*16;
    require(received>1,"Upper island did not feed the receiving island");
    require(std::abs(volume(result,data)-before)<before*0.0001,"Waterfall transfer between different cell sizes lost volume");
    unloadGpuWater(gpu); std::cout<<"Different-resolution island layers exchange waterfall volume conservatively\n";
}
void sourcesAndRain() {
    LandscapeData data; data.layers.push_back(fixture(16,3)); auto& l=data.layers[0];
    for (int z=3;z<13;++z) for (int x=3;x<13;++x) l.terrain[z*16+x].x=0;
    l.terrain[8*16+8].w=0.02F;
    GpuWater gpu=createGpuWater(data); gpu.rainfall=0;
    for (int i=0;i<60;++i) stepGpuWater(gpu);
    require(std::abs(volume(readState(gpu),data)-0.02*9.0)<0.00005,"Source rate did not use metres, square metres and seconds");
    unloadGpuWater(gpu);

    Shader rain=shallowShader(nullptr,R"GLSL(
        out vec4 finalColor;
        void main() {
            float id=floor(gl_FragCoord.y);
            finalColor=vec4(rainPosition(id,vec3(0,150,0),time+floor(gl_FragCoord.x)/60.0),1);
        }
    )GLSL",data,shallowShaders::rainMotion);
    std::vector<Vector4> zeros(kWaterAtlasTile*kWaterAtlasTile);
    auto output=atlasTarget(1,zeros.data()), input=atlasTarget(1,zeros.data());
    for (float time:{0.0F,3600.0F,21600.0F}) {
        SetShaderValue(rain,GetShaderLocation(rain,"time"),&time,SHADER_UNIFORM_FLOAT);
        shallowPass(output,rain,input.texture,{});
        auto* p=static_cast<Vector4*>(rlReadTexturePixels(output.texture.id,kWaterAtlasTile,kWaterAtlasTile,output.texture.format));
        require(p,"Rain position readback failed");
        for (int y=0;y<100;++y) for (int x=1;x<200;++x) {
            const float dy=p[y*kWaterAtlasTile+x].y-p[y*kWaterAtlasTile+x-1].y;
            require(dy<0 || dy>45,"Rain moved upward without a volume-wrap respawn");
        }
        MemFree(p);
    }
    UnloadRenderTexture(output); UnloadRenderTexture(input); UnloadShader(rain);
    std::cout<<"Source volumes use SI units; camera-local rain falls downward after hours\n";
}
void landscapeScale() {
    LandscapeData data; GenerationSettings settings; settings.islandCount=1;
    int triangles=0,count=0;
    Model model=createLandscape(42,settings,triangles,count,data);
    require(count==1 && data.layers[0].resolution==512,"Landscape tile dimensions incorrect");
    const auto& body=model.meshes[0]; float low=1e10F,high=-1e10F;
    for (int i=0;i<body.vertexCount;++i) {
        low=std::min(low,body.vertices[i*3]); high=std::max(high,body.vertices[i*3]);
        require(std::isfinite(body.vertices[i*3+1]),"Landscape warp generated a nonfinite height");
    }
    require(std::abs(high-low-10000)<0.01F,"Main landmass is not 10 km wide");
    require(model.meshCount==4 && model.meshes[3].vertexCount>0,"Missing volumetric LODs or matched watershed cap");
    int wet=0;
    for (auto s:data.layers[0].initial) { require(s.x>=0 && s.x<20,"Invalid initial lake depth"); wet+=s.x>0.1F; }
    require(wet>100,"Generated watershed has no rivers or lakes");
    UnloadModel(model); std::cout<<"Main landmass is 10 km wide with metre-scale water depths and matched terrain/water topology\n";
}
int main() {
    if (!std::getenv("DISPLAY")) return 77;
    SetConfigFlags(FLAG_WINDOW_HIDDEN); InitWindow(320,240,"Shallow-water regression");
    if (!IsWindowReady()) return 77;
    try { lakeAtRest(); mergingAndMomentum(); layerTransfers(); sourcesAndRain(); landscapeScale(); CloseWindow(); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; CloseWindow(); return 1; }
}
