#pragma once
namespace shallowShaders {
constexpr const char* common = R"GLSL(#version 330
uniform sampler2D bedTexture;
uniform sampler2D stateTexture;
uniform sampler2D fluxX;
uniform sampler2D fluxZ;
uniform vec4 tiles[10]; // origin x/z, cell width in metres, cell count
uniform int layerCount;
uniform float dt;
uniform float time;
uniform vec3 eye;
const int tileSize = 512;
const float gravity = 9.81;
int layerOf(ivec2 p) { return p.y/tileSize; }
bool inTile(ivec2 p, int l) {
    return l>=0 && l<layerCount && p.x>=0 && p.x<int(tiles[l].w) && p.y>=l*tileSize && p.y<l*tileSize+int(tiles[l].w);
}
vec4 bedAt(ivec2 p,int l) { return inTile(p,l) ? texelFetch(bedTexture,p,0) : vec4(0); }
vec4 stateAt(ivec2 p,int l) { return inTile(p,l) ? texelFetch(stateTexture,p,0) : vec4(0); }
vec2 worldAt(ivec2 p,int l) { return tiles[l].xy+(vec2(p.x,p.y-l*tileSize)+0.5)*tiles[l].z; }
vec3 haze(vec3 color,vec3 p) {
    float fog = 1.0-exp(-length(p-eye)/22000.0);
    return mix(color,vec3(0.67,0.76,0.80),fog);
}
)GLSL";

// Hydrostatic reconstruction + Rusanov finite-volume flux. The reconstruction
// and pressure corrections in integrate preserve a lake at rest on uneven bed.
constexpr const char* flux = R"GLSL(
uniform int axis;
out vec4 finalColor;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int l = layerOf(p);
    if (l>=layerCount) { finalColor=vec4(0); return; }
    ivec2 q = p+(axis==0 ? ivec2(1,0) : ivec2(0,1));
    vec4 bl=bedAt(p,l), br=bedAt(q,l);
    if (bl.b<0.5 && br.b<0.5) { finalColor=vec4(0); return; }
    vec3 L = bl.b>0.5 ? stateAt(p,l).rgb : vec3(0);
    vec3 R = br.b>0.5 ? stateAt(q,l).rgb : vec3(0);
    if (bl.b<0.5) bl.r=br.r;
    if (br.b<0.5) br.r=bl.r;
    float z=max(bl.r,br.r);
    float hL=max(0.0,bl.r+L.x-z), hR=max(0.0,br.r+R.x-z);
    vec2 vL=L.x>0.00001 ? L.yz/L.x : vec2(0);
    vec2 vR=R.x>0.00001 ? R.yz/R.x : vec2(0);
    vec3 uL=vec3(hL,hL*vL), uR=vec3(hR,hR*vR);
    float nL=vL[axis], nR=vR[axis];
    vec3 fL=uL*nL, fR=uR*nR;
    fL[axis+1]+=0.5*gravity*hL*hL;
    fR[axis+1]+=0.5*gravity*hR*hR;
    float speed=max(abs(nL)+sqrt(gravity*hL),abs(nR)+sqrt(gravity*hR));
    vec3 flow=0.5*(fL+fR)-0.5*speed*(uR-uL);
    // Shared donor limiter: at most one quarter of a cell's water per face.
    // Both adjacent cells consume this same flux, so volume is conserved.
    float available=(flow.x>=0.0 ? L.x : R.x)*tiles[l].z/(4.0*dt);
    float scale=min(1.0,available/max(abs(flow.x),1e-12));
    finalColor=vec4(flow*scale,0);
}
)GLSL";
constexpr const char* integrate = R"GLSL(
uniform sampler2D incomingInfo;
uniform sampler2D incomingEdges;
uniform float rainfall;
out vec4 finalColor;
float star(float eta,vec4 b,vec4 neighbor) { return max(0.0,eta-max(b.r,neighbor.b>0.5 ? neighbor.r : b.r)); }
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy); int l=layerOf(p);
    vec4 bed=bedAt(p,l), old=stateAt(p,l);
    if (bed.b<0.5) { finalColor=vec4(0); return; }
    vec3 right=texelFetch(fluxX,p,0).rgb;
    vec3 left=p.x>0 ? texelFetch(fluxX,p-ivec2(1,0),0).rgb : vec3(0);
    vec3 front=texelFetch(fluxZ,p,0).rgb;
    vec3 back=p.y>l*tileSize ? texelFetch(fluxZ,p-ivec2(0,1),0).rgb : vec3(0);
    float eta=bed.r+old.r;
    float hR=star(eta,bed,bedAt(p+ivec2(1,0),l));
    float hL=star(eta,bed,bedAt(p-ivec2(1,0),l));
    float hF=star(eta,bed,bedAt(p+ivec2(0,1),l));
    float hB=star(eta,bed,bedAt(p-ivec2(0,1),l));
    vec3 correction=vec3(0,0.5*gravity*(hL*hL-hR*hR),0.5*gravity*(hB*hB-hF*hF));
    vec3 next=old.rgb-dt/tiles[l].z*(right-left+front-back+correction);
    vec2 entries=texelFetch(incomingInfo,p,0).rg;
    for (int i=0;i<int(entries.y);++i) {
        int j=int(entries.x)+i;
        vec4 edge=texelFetch(incomingEdges,ivec2(j%1024,j/1024),0);
        int face=int(edge.x);
        ivec2 f=ivec2(face%tileSize,face/tileSize);
        float q=edge.y<0.5 ? texelFetch(fluxX,f,0).r : texelFetch(fluxZ,f,0).r;
        next.r+=max(0.0,q*edge.z)*edge.w*dt;
    }
    next.r=max(0.0,next.r+dt*(bed.a+rainfall));
    if (next.r<0.0001) next.yz=vec2(0);
    else {
        vec2 velocity=next.yz/next.r;
        float speed=length(velocity);
        next.yz/=1.0+gravity*0.035*0.035*dt*speed/pow(max(next.r,0.01),1.333333);
        float limitedSpeed=length(next.yz)/next.r;
        if (limitedSpeed>25.0) next.yz*=25.0/limitedSpeed;
    }
    float wet=clamp(old.a+dt*(min(next.r,1.0)+rainfall*1000.0)-dt*0.001,0.0,1.0);
    finalColor=vec4(next,wet);
}
)GLSL";

constexpr const char* terrainVertex = R"GLSL(
in vec3 vertexPosition; in vec3 vertexNormal; in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec3 world; out vec3 normal; out vec2 uv;
void main() { world=vertexPosition; normal=vertexNormal; uv=vertexTexCoord; gl_Position=mvp*vec4(world,1); }
)GLSL";
constexpr const char* terrainFragment = R"GLSL(
in vec3 world; in vec3 normal; in vec2 uv;
out vec4 finalColor;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
float noise(vec2 p) {
    vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+1.0),f.x),f.y);
}
void main() {
    vec3 n=normalize(normal);
    float slope=clamp(n.y,0.0,1.0);
    float forest=noise(world.xz/140.0)*noise(world.xz/37.0);
    vec3 rock=mix(vec3(0.27,0.28,0.25),vec3(0.48,0.43,0.34),noise(world.xz/65.0));
    float strata=sin(world.y*0.17+noise(world.xz/80.0)*3.0)*0.025;
    rock+=strata;
    vec3 grass=mix(vec3(0.16,0.25,0.14),vec3(0.37,0.42,0.21),forest);
    grass*=0.87+0.2*noise(world.xz/5.0);
    vec3 color=mix(rock,grass,smoothstep(0.55,0.9,slope));
    float wet=texture(stateTexture,uv).a*slope;
    color*=1.0-wet*0.16;
    float light=0.48+0.52*max(0.0,dot(n,normalize(vec3(-0.4,0.8,-0.3))));
    finalColor=vec4(haze(color*light,world),1);
}
)GLSL";
constexpr const char* waterVertex = R"GLSL(
in vec3 vertexPosition; in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec3 world; out vec2 uv; out float depth;
void main() {
    uv=vertexTexCoord;
    vec4 bed=texture(bedTexture,uv), water=texture(stateTexture,uv);
    depth=water.r;
    float level=bed.r+water.r;
    if (water.r<0.025) {
        ivec2 p=ivec2(uv*vec2(tileSize,tileSize*layerCount)); int l=layerOf(p);
        float total=0, weights=0;
        for (int z=-1;z<=1;++z) for (int x=-1;x<=1;++x) {
            ivec2 q=p+ivec2(x,z); vec4 b=bedAt(q,l), s=stateAt(q,l);
            if (b.b>0.5 && s.r>0.025) {
                float w=1.0/(1.0+float(x*x+z*z)); total+=(b.r+s.r)*w; weights+=w;
            }
        }
        if (weights>0) level=total/weights;
    }
    world=vec3(vertexPosition.x,level+0.025,vertexPosition.z);
    gl_Position=mvp*vec4(world,1);
}
)GLSL";
constexpr const char* waterFragment = R"GLSL(
in vec3 world; in vec2 uv; in float depth;
out vec4 finalColor;
void main() {
    vec4 bed=texture(bedTexture,uv), water=texture(stateTexture,uv);
    if (bed.b<0.99 || world.y-bed.r<0.025 || water.r<0.015) discard;
    float distance=length(world-eye);
    float detail=1.0-smoothstep(1000.0,10000.0,distance);
    float wave=sin(world.x*0.22+time*0.9)*sin(world.z*0.19-time*0.6)*detail;
    vec3 n=normalize(vec3(wave*0.06,1,cos(world.z*0.16+time)*0.035*detail));
    float fresnel=pow(1.0-max(dot(n,normalize(eye-world)),0.0),4.0);
    vec3 color=mix(vec3(0.12,0.34,0.36),vec3(0.055,0.20,0.27),1.0-exp(-depth/4.0));
    color=mix(color,vec3(0.66,0.79,0.83),fresnel*0.65);
    float speed=length(water.gb)/max(water.r,0.01);
    float foam=smoothstep(2.0,8.0,speed)*(0.5+0.5*sin(world.x*0.8+world.z*0.6-time*3.0));
    color=mix(color,vec3(0.72,0.83,0.82),foam*0.25);
    finalColor=vec4(haze(color,world),0.94);
}
)GLSL";
constexpr const char* fallVertex = R"GLSL(
in vec3 vertexPosition; in vec3 vertexNormal; in vec2 vertexTexCoord; in vec2 vertexTexCoord2;
uniform mat4 mvp;
out vec3 world; out float flow; out float along;
void main() {
    ivec2 p=ivec2(vertexTexCoord*vec2(tileSize,tileSize*layerCount));
    int l=layerOf(p), direction=int(vertexTexCoord2.x);
    ivec2 face=p;
    if (direction==0) face.x-=1;
    if (direction==2) face.y-=1;
    float discharge=direction<2 ? texelFetch(fluxX,face,0).r : texelFetch(fluxZ,face,0).r;
    flow=max(0.0,discharge*((direction==0 || direction==2) ? -1.0 : 1.0));
    float h=texelFetch(stateTexture,p,0).r;
    float t=vertexTexCoord2.y;
    world=vertexPosition;
    world.xz+=vertexNormal.xz*tiles[l].z*2.0*t;
    world.y=mix(vertexPosition.y+h,vertexNormal.y,t*t);
    along=t;
    gl_Position=flow>0.02 && h>0.015 ? mvp*vec4(world,1) : vec4(2,2,2,1);
}
)GLSL";
constexpr const char* fallFragment = R"GLSL(
in vec3 world; in float flow; in float along;
out vec4 finalColor;
void main() {
    if (flow<0.02) discard;
    float foam=0.5+0.5*sin(world.y*0.13+time*5.0+world.x*0.4);
    vec3 water=mix(vec3(0.29,0.55,0.62),vec3(0.77,0.87,0.88),foam*0.45);
    finalColor=vec4(haze(water,world),0.75);
}
)GLSL";
constexpr const char* rainMotion = R"GLSL(
float rainHash(float n) { return fract(sin(n*127.1)*43758.5453); }
vec3 rainPosition(float id,vec3 camera,float clock) {
    float speed=9.0+6.0*rainHash(id+19.0);
    return vec3(camera.x+40.0-mod(camera.x+rainHash(id+1.0)*80.0,80.0),
                camera.y+25.0-mod(camera.y+clock*speed+rainHash(id+7.0)*50.0,50.0),
                camera.z+40.0-mod(camera.z+rainHash(id+13.0)*80.0,80.0));
}
)GLSL";
constexpr const char* rainVertex = R"GLSL(
in vec3 vertexPosition; in vec2 vertexTexCoord;
uniform mat4 mvp; uniform vec3 cameraRight;
out float visibility;
void main() {
    float ground=-1800;
    for (int l=0;l<layerCount;++l) {
        ivec2 p=ivec2(floor((eye.xz-tiles[l].xy)/tiles[l].z))+ivec2(0,l*tileSize);
        vec4 bed=bedAt(p,l);
        if (bed.b>0.5 && bed.r<eye.y) ground=max(ground,bed.r);
    }
    visibility=1.0-smoothstep(350.0,1000.0,eye.y-ground);
    vec3 p=rainPosition(vertexPosition.x,eye,time);
    p+=cameraRight*vertexTexCoord.x*0.012+vec3(0,vertexTexCoord.y*0.6,0);
    gl_Position=visibility>0 ? mvp*vec4(p,1) : vec4(2,2,2,1);
}
)GLSL";
constexpr const char* rainFragment = R"GLSL(
in float visibility; out vec4 finalColor;
void main() { finalColor=vec4(0.67,0.78,0.84,visibility*0.32); }
)GLSL";
}
