#include "../src/particle_fluid.h"
#include "rlgl.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void quad(std::vector<Vector3>& mesh, Vector3 a, Vector3 b, Vector3 c, Vector3 d) {
    for (auto p : {a,b,c,a,c,d}) mesh.push_back(p);
}
void checkBasin() {
    std::vector<Vector3> mesh;
    quad(mesh,{-1.2F,0,-0.6F},{-1.2F,0,0.6F},{1.2F,0,0.6F},{1.2F,0,-0.6F});
    quad(mesh,{-1.2F,0,-0.6F},{-1.2F,2,-0.6F},{-1.2F,2,0.6F},{-1.2F,0,0.6F});
    quad(mesh,{1.2F,0,0.6F},{1.2F,2,0.6F},{1.2F,2,-0.6F},{1.2F,0,-0.6F});
    quad(mesh,{-1.2F,0,0.6F},{-1.2F,2,0.6F},{1.2F,2,0.6F},{1.2F,0,0.6F});
    quad(mesh,{1.2F,0,-0.6F},{1.2F,2,-0.6F},{-1.2F,2,-0.6F},{-1.2F,0,-0.6F});
    std::vector<Vector4> drops;
    for (float center : {-0.65F,0.65F}) for (int y = 0; y < 8; ++y) for (int z = 0; z < 5; ++z) for (int x = 0; x < 5; ++x) {
        drops.push_back(Vector4{center+(x-2)*0.14F,0.2F+y*0.14F,(z-2)*0.14F,1});
    }
    ParticleFluid fluid(mesh,drops,0.14F,false);
    for (int i = 0; i < 480; ++i) fluid.step(1.0F/120.0F);
    const auto positions = fluid.readPositions();
    int bridge = 0;
    float left = 0, right = 0;
    int leftCount = 0, rightCount = 0;
    for (const auto& p : positions) {
        require(std::isfinite(p.y) && p.y >= -0.03F,"Basin water crossed the floor");
        require(std::abs(p.x) < 1.22F && std::abs(p.z) < 0.62F,"Basin water crossed a wall");
        bridge += std::abs(p.x) < 0.18F;
        if (p.x < 0) { left += p.y; ++leftCount; } else { right += p.y; ++rightCount; }
    }
    require(bridge > 15,"Separate PhysX pools failed to merge into the initially dry gap");
    require(leftCount && rightCount && std::abs(left/leftCount-right/rightCount) < 0.08F,"Merged basin did not settle symmetrically");
    require(positions.size() == drops.size(),"Basin lost water particles");

    // Exercise GPU meshing, direct CUDA/OpenGL output, compositing and resize.
    Camera3D camera{{3,3,4},{0,0.25F,0},{0,1,0},45,CAMERA_PERSPECTIVE};
    for (Vector2 size : {Vector2{320,240},Vector2{480,320}}) {
        RenderTexture2D target = LoadRenderTexture(static_cast<int>(size.x),static_cast<int>(size.y));
        BeginTextureMode(target);
        ClearBackground(Color{20,25,30,255});
        BeginMode3D(camera);
        fluid.draw(rlGetMatrixModelview(),rlGetMatrixProjection(),static_cast<int>(size.x),static_cast<int>(size.y));
        EndMode3D();
        EndTextureMode();
        require(fluid.surfaceTriangles() > 100,"PhysX did not generate a water surface mesh");
        Image image = LoadImageFromTexture(target.texture);
        Color* pixels = LoadImageColors(image);
        int changed = 0;
        for (int i = 0; i < image.width*image.height; ++i) changed += pixels[i].b > 40;
        require(changed > 100,"Fluid surface rendering produced an empty image");
        UnloadImageColors(pixels); UnloadImage(image); UnloadRenderTexture(target);
    }
    std::cout << "PhysX pools merge, stay inside 3D walls, conserve particles, and render a GPU-generated mesh after resize\n";
}

void checkStackedSurfacesAndRecycling() {
    std::vector<Vector3> mesh;
    quad(mesh,{-20,0,-20},{-20,0,20},{20,0,20},{20,0,-20});
    quad(mesh,{-10,2,-10},{-10,2,10},{0,2,10},{0,2,-10});
    std::vector<Vector4> drops;
    for (float x : {-3.0F,3.0F}) for (int y = 0; y < 3; ++y) for (int z = 0; z < 4; ++z) for (int i = 0; i < 4; ++i) {
        drops.push_back(Vector4{x+(i-1.5F)*0.14F,3.0F+y*0.14F,(z-1.5F)*0.14F,1});
    }
    {
        ParticleFluid fluid(mesh,drops,0.14F,false);
        for (int i = 0; i < 180; ++i) fluid.step(1.0F/120.0F);
        int upper = 0, lower = 0;
        for (const auto& p : fluid.readPositions()) {
            require(p.y >= -0.03F,"Water crossed lower stacked surface");
            upper += p.x < 0 && p.y > 1.95F && p.y < 2.5F;
            lower += p.x > 0 && p.y < 0.5F;
        }
        require(upper > 20 && lower > 20,"Water failed to collide with separate stacked surfaces");
    }
    {
        const std::vector<Vector4> sources{{0,2,0,1},{2,2,0,1}};
        ParticleFluid fluid({},sources,0.14F,true);
        for (int i = 0; i < 360; ++i) fluid.step(1.0F/120.0F);
        for (const auto& p : fluid.readPositions()) {
            require(std::isfinite(p.y) && p.y > -18.2F && p.y <= 2.01F,"Escaped water was not recycled on GPU");
        }
    }
    std::cout << "PhysX handles stacked terrain, GPU recycling, and repeated scene teardown/recreation\n";
}

int main() {
    if (!std::getenv("DISPLAY")) return 77;
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320,240,"PhysX fluid regression");
    if (!IsWindowReady()) return 77;
    try {
        const std::vector<Vector3> floor{{-20,0,-20},{-20,0,20},{20,0,20}, {-20,0,-20},{20,0,20},{20,0,-20}};
        std::vector<Vector4> drops;
        for (int y = 0; y < 6; ++y) for (int z = 0; z < 10; ++z) for (int x = 0; x < 10; ++x) {
            drops.push_back(Vector4{(x-4.5F)*0.14F,2.0F+y*0.14F,(z-4.5F)*0.14F,1});
        }
        {
            ParticleFluid fluid(floor,drops,0.14F,false);
            for (int i = 0; i < 180; ++i) fluid.step(1.0F/120.0F);
            const auto result = fluid.readPositions();
            float minY = 1000, maxY = -1000, extent = 0;
            for (const auto& p : result) { minY = std::min(minY,p.y); maxY = std::max(maxY,p.y); extent = std::max(extent,std::max(std::abs(p.x),std::abs(p.z))); }
            std::cout << "Fluid bounds: " << minY << " .. " << maxY << ", xz " << extent << '\n';
            float average = 0;
            for (const auto& p : result) {
                if (!std::isfinite(p.y) || p.y < -0.03F) throw std::runtime_error("PhysX fluid penetrated triangle collision floor");
                average += p.y;
            }
            average /= result.size();
            if (average > 0.7F) throw std::runtime_error("PhysX particles failed to fall and settle");
            if (result.size() != drops.size()) throw std::runtime_error("PhysX lost particles");
            std::cout << "PhysX GPU: " << result.size() << " particles fell and settled on triangle geometry; mean height " << average << '\n';
        }
        checkBasin();
        checkStackedSurfacesAndRecycling();
        CloseWindow();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        CloseWindow();
        return 1;
    }
}
