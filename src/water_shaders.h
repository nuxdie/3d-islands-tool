#pragma once

namespace waterShaders {
constexpr const char* common = R"GLSL(#version 330
uniform vec2 gridSize;
uniform vec3 volumeMin;
uniform vec3 volumeMax;
uniform sampler2D terrain;
uniform sampler2D state;
uniform float time;
uniform float dt;
vec2 gridAt(vec2 xz) { return (xz-volumeMin.xz)/(volumeMax.xz-volumeMin.xz)*(gridSize-1.0); }
vec2 uvAt(vec2 xz) { return (gridAt(xz)+0.5)/gridSize; }
)GLSL";

// This texture tracks visual wetness only. Water dynamics are solved by PhysX.
constexpr const char* wetness = R"GLSL(
uniform vec4 clouds[10];
uniform int cloudCount;
out vec4 finalColor;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float bed = texelFetch(terrain,p,0).r;
    if (bed <= volumeMin.y) { finalColor = vec4(0); return; }
    vec2 world = mix(volumeMin.xz,volumeMax.xz,vec2(p)/(gridSize-1.0));
    float rain = 0.0;
    for (int i = 0; i < cloudCount; ++i) {
        vec2 relative = (world-clouds[i].xy)/(clouds[i].z*vec2(1.45,1.0));
        rain += max(0.0,1.0-dot(relative,relative))*clouds[i].w;
    }
    float wet = clamp(texelFetch(state,p,0).g + rain*8.0*dt-dt*0.002,0.0,1.0);
    finalColor = vec4(0,wet,0,1);
}
)GLSL";
constexpr const char* terrainVertex = R"GLSL(
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
out vec4 color;
out vec3 world;
out float upward;
void main() {
    world = vertexPosition;
    upward = vertexNormal.y;
    color = vertexColor;
    gl_Position = mvp*vec4(vertexPosition,1);
}
)GLSL";
constexpr const char* terrainFragment = R"GLSL(
in vec4 color;
in vec3 world;
in float upward;
out vec4 finalColor;
void main() {
    vec2 uv = uvAt(world.xz);
    float bed = texture(terrain,uv).r;
    float wet = texture(state,uv).g*clamp(upward*1.6,0.0,1.0)*clamp(1.0-abs(bed-world.y)/1.8,0.0,1.0);
    finalColor = vec4(color.rgb*vec3(1.0-wet*0.42,1.0-wet*0.42,1.0-wet*0.2)+vec3(0,0,wet*0.078),1);
}
)GLSL";
constexpr const char* rainMotion = R"GLSL(
float rainHash(float n) {
    uint v = uint(n)+0x9e3779b9u;
    v = (v^(v>>16u))*0x7feb352du;
    v = (v^(v>>15u))*0x846ca68bu;
    v ^= v>>16u;
    return float(v&0x00ffffffu)/16777216.0;
}
// Lifetime/emission height stay fixed while the visual cloud bobs.
vec4 rainAt(vec4 cloud, float id, float clock, out float age) {
    float top = cloud.y-cloud.w*0.42;
    float speed = 10.0+6.0*rainHash(id+7.0);
    float lifetime = max(top-volumeMin.y,1.0)/speed;
    float cycles = clock/lifetime+rainHash(id+30.0);
    float generation = floor(cycles);
    age = fract(cycles)*lifetime;
    float seed = id+generation*67.0;
    float angle = rainHash(seed+1.0)*6.283185;
    float radius = sqrt(rainHash(seed+12.0))*cloud.w;
    vec2 xz = cloud.xz+vec2(cos(angle)*1.45,sin(angle))*radius;
    return vec4(xz.x,top-speed*age,xz.y,generation);
}
)GLSL";
constexpr const char* rainVertex = R"GLSL(
in vec3 vertexPosition;
in vec2 vertexTexCoord;
uniform vec4 rainClouds[10];
uniform vec3 cameraRight;
uniform mat4 mvp;
uniform sampler2D detailedHeight;
out float visibility;
void main() {
    int cloud = int(vertexPosition.x);
    float id = vertexPosition.y+float(cloud)*193.0;
    float age;
    vec3 drop = rainAt(rainClouds[cloud],id,time,age).xyz;
    vec2 uv = (drop.xz-volumeMin.xz)/(volumeMax.xz-volumeMin.xz);
    vec2 size = vec2(textureSize(detailedHeight,0));
    float bed = max(volumeMin.y,texture(detailedHeight,(uv*(size-1.0)+0.5)/size).r);
    visibility = smoothstep(0.0,0.04,age)*smoothstep(0.0,0.25,drop.y-bed);
    vec3 p = drop+vec3(0,vertexPosition.z*0.5,0)+cameraRight*vertexTexCoord.x*0.012;
    gl_Position = drop.y > bed ? mvp*vec4(p,1) : vec4(2,2,2,1);
}
)GLSL";
constexpr const char* rainFragment = R"GLSL(
in float visibility;
out vec4 finalColor;
void main() { finalColor = vec4(0.59,0.8,0.87,0.55*visibility); }
)GLSL";
} // namespace waterShaders
