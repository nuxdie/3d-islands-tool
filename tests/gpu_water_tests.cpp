#define main islandsApplicationMain
#include "../src/main.cpp"
#undef main
#include <iostream>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void checkRain() {
    RenderTexture2D target = floatTarget(), source = floatTarget();
    Shader motion = waterShader(nullptr,R"GLSL(
        out vec4 finalColor;
        void main() {
            float id = floor(gl_FragCoord.y);
            vec4 cloud = vec4(0,20.0+mod(id,10.0),0,3.0);
            float age;
            finalColor = rainAt(cloud,id,time+floor(gl_FragCoord.x)/60.0,age);
        }
    )GLSL",waterShaders::rainMotion);
    int respawns = 0, changedOrigins = 0;
    for (float clock : {0.0F,180.0F,3600.0F,21600.0F}) {
        SetShaderValue(motion,GetShaderLocation(motion,"time"),&clock,SHADER_UNIFORM_FLOAT);
        simulationPass(target,motion,source.texture,{});
        auto* samples = static_cast<Vector4*>(rlReadTexturePixels(target.texture.id,kGridX,kGridZ,target.texture.format));
        require(samples != nullptr,"Rain motion readback failed");
        for (int z = 0; z < kGridZ; ++z) for (int x = 1; x < kGridX; ++x) {
            const Vector4 a = samples[z*kGridX+x-1], b = samples[z*kGridX+x];
            require(std::isfinite(b.y),"Nonfinite rain position");
            if (a.w == b.w) {
                require(b.y < a.y,"An in-flight raindrop moved upward");
                require(a.x == b.x && a.z == b.z,"Rain origin moved during its lifetime");
            } else { ++respawns; changedOrigins += a.x != b.x || a.z != b.z; }
        }
        MemFree(samples);
    }
    require(respawns > 0 && changedOrigins > respawns*3/4,"Rain repeats the same columns on respawn");
    UnloadShader(motion);
    UnloadRenderTexture(target); UnloadRenderTexture(source);
    std::cout << "GPU rain falls monotonically, including after six hours\n";
}
int main() {
    if (!std::getenv("DISPLAY")) return 77;
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(128,128,"GPU rain regression");
    if (!IsWindowReady()) return 77;
    try { checkRain(); CloseWindow(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; CloseWindow(); return 1; }
}
