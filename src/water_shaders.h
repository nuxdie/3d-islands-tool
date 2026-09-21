#pragma once

// OpenGL 3.3 fragment passes keep simulation textures resident on the GPU.
namespace waterShaders {
constexpr const char* common = R"GLSL(#version 330
uniform vec2 gridSize;
uniform vec3 volumeMin;
uniform vec3 volumeMax;
uniform sampler2D terrain;
uniform sampler2D state;
uniform sampler2D flux;
uniform float time;
uniform float dt;
const ivec2 offsets[4] = ivec2[4](ivec2(-1,0), ivec2(1,0), ivec2(0,-1), ivec2(0,1));
ivec2 cellAt(int id) { return ivec2(id % int(gridSize.x), id / int(gridSize.x)); }
vec2 gridAt(vec2 xz) { return (xz - volumeMin.xz) / (volumeMax.xz - volumeMin.xz) * (gridSize - 1.0); }
vec2 uvAt(vec2 xz) { return (gridAt(xz) + 0.5) / gridSize; }
bool valid(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, ivec2(gridSize))); }
)GLSL";

constexpr const char* flux = R"GLSL(
uniform sampler2D routes;
out vec4 finalColor;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float bed = texelFetch(terrain, p, 0).r;
    float water = texelFetch(state, p, 0).r;
    if (bed <= volumeMin.y || water <= 0.0) { finalColor = vec4(0); return; }
    float head = bed + water;
    vec4 outflow = vec4(0);
    vec4 destinations = texelFetch(routes, p, 0);
    for (int d = 0; d < 4; ++d) {
        if (destinations[d] < -1.5) continue; // blocked route
        ivec2 q = p + offsets[d];
        float other = volumeMin.y - 1.0;
        if (valid(q)) other = texelFetch(terrain, q, 0).r + texelFetch(state, q, 0).r;
        // Simultaneous, symmetric head equalization in all four directions.
        outflow[d] = max(0.0, head - other) * (1.0 - exp(-8.0 * dt)) * 0.24;
    }
    float total = dot(outflow, vec4(1));
    // Drain slopes quickly, but never export more than the source contains.
    outflow *= min(1.0, water / max(total, 1e-12));
    finalColor = outflow;
}
)GLSL";

constexpr const char* integrate = R"GLSL(
uniform sampler2D incomingEdges;
uniform vec4 clouds[10]; // x, z, radius, rainfall depth/second at center
uniform int cloudCount;
uniform float evaporation;
out vec4 finalColor;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 ground = texelFetch(terrain, p, 0);
    if (ground.r <= volumeMin.y) { finalColor = vec4(0); return; }
    vec4 old = texelFetch(state, p, 0);
    vec4 outgoing = texelFetch(flux, p, 0);
    float water = old.r - dot(outgoing, vec4(1));
    int width = textureSize(incomingEdges, 0).x;
    int start = int(ground.g);
    int count = int(ground.b);
    for (int i = 0; i < count; ++i) {
        int edge = start + i;
        vec2 source = texelFetch(incomingEdges, ivec2(edge % width, edge / width), 0).rg;
        water += texelFetch(flux, cellAt(int(source.x)), 0)[int(source.y)];
    }
    vec2 world = mix(volumeMin.xz, volumeMax.xz, vec2(p) / (gridSize - 1.0));
    float rain = 0.0;
    for (int i = 0; i < cloudCount; ++i) {
        vec2 relative = (world - clouds[i].xy) / (clouds[i].z * vec2(1.45, 1.0));
        rain += max(0.0, 1.0 - dot(relative, relative)) * clouds[i].w;
    }
    water = max(0.0, water + rain * dt) * exp(-evaporation * dt);
    float wet = clamp(old.g + (rain * 8.0 + min(water * 3.0, 1.0)) * dt - dt * 0.002, 0.0, 1.0);
    finalColor = vec4(water, wet, max(old.b * exp(-2.2 * dt), dot(outgoing, vec4(1)) / dt), 1);
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
    gl_Position = mvp * vec4(vertexPosition, 1);
}
)GLSL";
constexpr const char* terrainFragment = R"GLSL(
in vec4 color;
in vec3 world;
in float upward;
out vec4 finalColor;
void main() {
    vec2 uv = uvAt(world.xz);
    float bed = texture(terrain, uv).r;
    float wet = texture(state, uv).g * clamp(upward * 1.6, 0.0, 1.0) *
                clamp(1.0 - abs(bed - world.y) / 1.8, 0.0, 1.0);
    finalColor = vec4(color.rgb * vec3(1.0 - wet * 0.42, 1.0 - wet * 0.42, 1.0 - wet * 0.2)
                      + vec3(0, 0, wet * 0.078), 1);
}
)GLSL";

constexpr const char* lakeSampling = R"GLSL(
// Wet-weighted water elevations let a shoreline cross dry nodes without
// pulling the pool up to the dry bank's elevation. Shared vertices join pools.
vec2 lakeAt(vec2 xz) {
    vec2 g = clamp(gridAt(xz), vec2(0), gridSize - 1.001);
    ivec2 base = ivec2(floor(g));
    vec2 f = fract(g);
    float level = 0.0, weight = 0.0, depth = 0.0;
    for (int z = 0; z < 2; ++z) for (int x = 0; x < 2; ++x) {
        ivec2 p = base + ivec2(x,z);
        float h = texelFetch(terrain, p, 0).r;
        float w = texelFetch(state, p, 0).r;
        float b = (x == 0 ? 1.0-f.x : f.x) * (z == 0 ? 1.0-f.y : f.y);
        float wet = b * smoothstep(0.008, 0.04, w) * step(volumeMin.y + 0.001, h);
        level += (h + w) * wet;
        weight += wet;
        depth += b * w;
    }
    return vec2(weight > 1e-6 ? level / weight : volumeMin.y - 1.0, depth);
}
)GLSL";
constexpr const char* lakeVertex = R"GLSL(
in vec3 vertexPosition;
uniform mat4 mvp;
out vec3 world;
void main() {
    vec2 lake = lakeAt(vertexPosition.xz);
    world = vec3(vertexPosition.x, lake.x + 0.012, vertexPosition.z);
    gl_Position = mvp * vec4(world, 1);
}
)GLSL";
constexpr const char* lakeFragment = R"GLSL(
uniform sampler2D detailedHeight;
in vec3 world;
out vec4 finalColor;
void main() {
    vec2 uv = (world.xz - volumeMin.xz) / (volumeMax.xz - volumeMin.xz);
    vec2 size = vec2(textureSize(detailedHeight, 0));
    float bed = texture(detailedHeight, (uv * (size - 1.0) + 0.5) / size).r;
    vec2 lake = lakeAt(world.xz);
    float depth = world.y - bed;
    if (bed <= volumeMin.y || depth < 0.018 || lake.y < 0.025) discard;
    float ripple = sin(world.x * 3.4 + time * 1.7) * sin(world.z * 3.1 - time * 1.2);
    vec3 water = mix(vec3(0.17,0.48,0.59), vec3(0.07,0.27,0.40), clamp(depth * 0.5, 0.0, 1.0));
    float shore = 1.0 - smoothstep(0.025, 0.14, depth);
    finalColor = vec4(water + ripple * 0.018 + shore * vec3(0.12,0.2,0.2), 0.87);
}
)GLSL";

constexpr const char* riverVertex = R"GLSL(
in vec3 vertexPosition;
in vec3 vertexNormal; // preprojected bank offset, not a lighting normal
in vec2 vertexTexCoord; // source cell center
in vec2 vertexTexCoord2; // direction channel, distance along path
in vec4 vertexColor; // r: bank side
uniform mat4 mvp;
out float activity;
out vec2 ribbon;
void main() {
    ivec2 cell = ivec2(vertexTexCoord * gridSize);
    activity = texelFetch(flux, cell, 0)[int(vertexTexCoord2.x)] / dt;
    float width = clamp(sqrt(max(activity - 0.18, 0.0)) * 0.9, 0.0, 1.0);
    vec3 p = vertexPosition + vertexNormal * width;
    ribbon = vec2(vertexColor.r, vertexTexCoord2.y);
    gl_Position = activity > 0.2 ? mvp * vec4(p, 1) : vec4(2, 2, 2, 1);
}
)GLSL";
constexpr const char* riverFragment = R"GLSL(
in float activity;
in vec2 ribbon;
out vec4 finalColor;
void main() {
    if (activity <= 0.2) discard;
    float foam = pow(max(0.0, sin(ribbon.y * 10.0 - time * 9.0)), 12.0);
    float bank = smoothstep(0.0, 0.12, ribbon.x) * smoothstep(0.0, 0.12, 1.0-ribbon.x);
    finalColor = vec4(mix(vec3(0.21,0.55,0.69), vec3(0.68,0.85,0.89), foam * 0.4), bank * 0.86);
}
)GLSL";

constexpr const char* rainMotion = R"GLSL(
float rainHash(float n) {
    uint v = uint(n) + 0x9e3779b9u;
    v = (v ^ (v >> 16u)) * 0x7feb352du;
    v = (v ^ (v >> 15u)) * 0x846ca68bu;
    v ^= v >> 16u;
    return float(v & 0x00ffffffu) / 16777216.0;
}
// Cloud xyz is the stable emission origin, not its animated visual center.
// Lifetime depends only on that origin and the volume floor. Terrain clips a
// drop at impact; it must never rescale the age of a drop already in flight.
vec4 rainAt(vec4 cloud, float id, float clock, out float age) {
    float top = cloud.y - cloud.w * 0.42;
    float speed = 10.0 + 6.0 * rainHash(id + 7.0);
    float lifetime = max(top - volumeMin.y, 1.0) / speed;
    float cycles = clock / lifetime + rainHash(id + 30.0);
    float generation = floor(cycles);
    age = fract(cycles) * lifetime;
    float seed = id + generation * 67.0;
    float angle = rainHash(seed + 1.0) * 6.283185;
    float radius = sqrt(rainHash(seed + 12.0)) * cloud.w;
    vec2 xz = cloud.xz + vec2(cos(angle) * 1.45, sin(angle)) * radius;
    return vec4(xz.x, top - speed * age, xz.y, generation);
}
)GLSL";

constexpr const char* rainVertex = R"GLSL(
in vec3 vertexPosition; // cloud index, drop index, vertical tip
in vec2 vertexTexCoord; // side, unused
uniform vec4 rainClouds[10]; // xyz, radius
uniform vec3 cameraRight;
uniform mat4 mvp;
uniform sampler2D detailedHeight;
out float visibility;
void main() {
    int cloud = int(vertexPosition.x);
    float id = vertexPosition.y + float(cloud) * 193.0;
    float age;
    vec3 drop = rainAt(rainClouds[cloud], id, time, age).xyz;
    vec2 uv = (drop.xz - volumeMin.xz) / (volumeMax.xz - volumeMin.xz);
    vec2 size = vec2(textureSize(detailedHeight, 0));
    float bed = texture(detailedHeight, (uv * (size - 1.0) + 0.5) / size).r;
    float water = texture(state, uvAt(drop.xz)).r;
    if (water > 0.025) bed = max(bed, texture(terrain, uvAt(drop.xz)).r + water);
    bed = max(bed, volumeMin.y);
    visibility = smoothstep(0.0, 0.04, age) * smoothstep(0.0, 0.25, drop.y - bed);
    vec3 p = drop + vec3(0, vertexPosition.z * 0.5, 0) + cameraRight * vertexTexCoord.x * 0.012;
    gl_Position = drop.y > bed ? mvp * vec4(p, 1) : vec4(2, 2, 2, 1);
}
)GLSL";
constexpr const char* rainFragment = R"GLSL(
in float visibility;
out vec4 finalColor;
void main() { finalColor = vec4(0.59, 0.8, 0.87, 0.55 * visibility); }
)GLSL";
} // namespace waterShaders
