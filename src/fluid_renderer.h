// Private implementation included by particle_fluid.cpp. PhysX reconstructs and
// smooths a triangle surface directly into CUDA/OpenGL-shared vertex buffers.
namespace {
constexpr const char* fluidMeshVertex = R"GLSL(#version 330
layout(location=0) in vec4 position;
layout(location=1) in vec4 normal;
uniform mat4 view;
uniform mat4 projection;
out vec3 eyePosition;
out vec3 eyeNormal;
void main() {
    eyePosition = (view*vec4(position.xyz,1)).xyz;
    eyeNormal = mat3(view)*normal.xyz;
    gl_Position = projection*vec4(eyePosition,1);
}
)GLSL";
constexpr const char* fluidMeshDepth = R"GLSL(#version 330
uniform sampler2D opaqueDepth;
uniform vec2 screenSize;
in vec3 eyePosition;
in vec3 eyeNormal;
out vec4 finalColor;
void main() {
    if (gl_FragCoord.z > texture(opaqueDepth,gl_FragCoord.xy/screenSize).r) discard;
    finalColor = vec4(-eyePosition.z,normalize(eyeNormal));
}
)GLSL";
constexpr const char* fluidVertex = R"GLSL(#version 330
uniform samplerBuffer positions;
uniform mat4 view;
uniform mat4 projection;
uniform float radius;
out vec2 disk;
flat out vec3 center;
void main() {
    vec2 corners[4] = vec2[4](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(1,1));
    disk = corners[gl_VertexID];
    center = (view * vec4(texelFetch(positions, gl_InstanceID).xyz, 1)).xyz;
    gl_Position = projection * vec4(center + vec3(disk * radius, 0), 1);
}
)GLSL";
constexpr const char* fluidDepth = R"GLSL(#version 330
uniform mat4 projection;
uniform float radius;
uniform sampler2D opaqueDepth;
uniform vec2 screenSize;
uniform int thicknessPass;
in vec2 disk;
flat in vec3 center;
out vec4 finalColor;
void main() {
    float r2 = dot(disk,disk);
    if (r2 > 1.0 || center.z >= -0.05) discard;
    float cap = sqrt(1.0-r2) * radius;
    vec3 eye = center + vec3(disk * radius, cap);
    vec4 clip = projection * vec4(eye,1);
    float z = clip.z / clip.w * 0.5 + 0.5;
    if (z > texture(opaqueDepth, gl_FragCoord.xy / screenSize).r) discard;
    gl_FragDepth = z;
    finalColor = vec4(thicknessPass == 1 ? cap * 2.0 : -eye.z, 0, 0, 1);
}
)GLSL";
constexpr const char* fullScreenVertex = R"GLSL(#version 330
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0, 1);
}
)GLSL";
constexpr const char* shadeFluid = R"GLSL(#version 330
uniform sampler2D surfaceDepth;
uniform sampler2D thickness;
uniform sampler2D opaqueColor;
uniform sampler2D opaqueDepth;
uniform mat4 inverseProjection;
uniform mat4 inverseView;
uniform mat4 projection;
uniform vec2 screenSize;
in vec2 uv;
out vec4 finalColor;
vec3 positionAt(vec2 coord, float depth) {
    vec4 ray = inverseProjection * vec4(coord * 2.0 - 1.0, 1,1);
    return ray.xyz * (-depth / ray.z);
}
void main() {
    float depth = texture(surfaceDepth,uv).r;
    if (depth <= 0.0) discard;
    vec3 p = positionAt(uv,depth);
    vec4 clip = projection * vec4(p,1);
    float z = clip.z / clip.w * 0.5 + 0.5;
    if (z > texture(opaqueDepth,uv).r) discard;
    vec3 n = normalize(texture(surfaceDepth,uv).gba);
    if (n.z < 0.0) n = -n;
    vec3 v = normalize(-p);
    float fresnel = 0.04 + 0.65 * pow(1.0-max(dot(n,v),0.0),5.0);
    float amount = max(texture(thickness,uv).r,0.02);
    vec2 refractedUV = clamp(uv + n.xy * min(amount,0.7) * 0.008,vec2(0),vec2(1));
    // Avoid refracting foreground rock into water behind it.
    if (texture(opaqueDepth,refractedUV).r < z) refractedUV = uv;
    vec3 background = texture(opaqueColor,refractedUV).rgb;
    vec3 absorption = exp(-vec3(1.7,0.65,0.35) * amount * 2.0);
    vec3 transmission = background * absorption + vec3(0.055,0.28,0.36) * (1.0-absorption);
    vec3 worldNormal = mat3(inverseView) * n;
    vec3 reflection = mix(vec3(0.10,0.20,0.26),vec3(0.58,0.76,0.83),clamp(worldNormal.y*0.5+0.5,0.0,1.0));
    vec3 light = normalize(vec3(-0.4,0.75,0.5));
    float specular = pow(max(dot(n,normalize(light+v)),0.0),80.0) * 0.45;
    finalColor = vec4(mix(transmission,reflection,fresnel) + specular,1);
    gl_FragDepth = z;
}
)GLSL";

Shader fluidShader(const char* vertex, const char* fragment) {
    Shader shader = LoadShaderFromMemory(vertex, fragment);
    if (shader.id == rlGetShaderIdDefault()) throw std::runtime_error("Fluid renderer shader compilation failed");
    return shader;
}
void uniformMatrix(GLuint shader, const char* name, Matrix matrix) {
    const auto values = MatrixToFloatV(matrix);
    glUniformMatrix4fv(glGetUniformLocation(shader,name),1,GL_FALSE,values.v);
}
void sampler(GLuint shader, const char* name, GLuint texture, int slot, GLenum type = GL_TEXTURE_2D) {
    glActiveTexture(GL_TEXTURE0+slot);
    glBindTexture(type,texture);
    glUniform1i(glGetUniformLocation(shader,name),slot);
}
}

struct ParticleFluid::Impl::Renderer {
    Impl& owner;
    PxIsosurfaceExtractor* extractor = nullptr;
    std::array<GLuint,3> meshBuffers{};
    std::array<CUgraphicsResource,3> meshInterop{};
    GLuint meshVao = 0;
    unsigned int meshTriangles = 0;
    Shader meshShader = fluidShader(fluidMeshVertex,fluidMeshDepth);
    Shader particles = fluidShader(fluidVertex,fluidDepth);
    Shader shade = fluidShader(fullScreenVertex,shadeFluid);
    std::array<GLuint,2> fbo{}, texture{}; // surface depth/normal and thickness
    GLuint depthBuffer = 0, opaqueColor = 0, opaqueDepth = 0;
    int width = 0, height = 0;
    explicit Renderer(Impl& f) : owner(f) {
        constexpr unsigned int maxVertices = 524288, maxTriangles = 1048576;
        glGenBuffers(3,meshBuffers.data());
        for (int i = 0; i < 3; ++i) {
            glBindBuffer(GL_ARRAY_BUFFER,meshBuffers[i]);
            glBufferData(GL_ARRAY_BUFFER,i < 2 ? maxVertices*sizeof(PxVec4) : maxTriangles*3*sizeof(PxU32),nullptr,GL_DYNAMIC_DRAW);
        }
        glGenVertexArrays(1,&meshVao);
        glBindVertexArray(meshVao);
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_ARRAY_BUFFER,meshBuffers[i]);
            glVertexAttribPointer(i,4,GL_FLOAT,GL_FALSE,sizeof(PxVec4),nullptr);
            glEnableVertexAttribArray(i);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,meshBuffers[2]);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER,0);
        PxScopedCudaLock lock(*f.cuda);
        for (int i = 0; i < 3; ++i) check(f.registerBuffer(&meshInterop[i],meshBuffers[i],2),"Register fluid mesh buffer");
        PxSparseGridParams grid;
        grid.gridSpacing = std::max(f.spacing,0.16F);
        grid.maxNumSubgrids = 2048;
        grid.subgridSizeX = grid.subgridSizeY = grid.subgridSizeZ = 16;
        PxIsosurfaceParams params;
        params.particleCenterToIsosurfaceDistance = f.spacing*1.15F;
        params.gridSmoothingRadius = f.spacing;
        params.numMeshSmoothingPasses = 6;
        params.numMeshNormalSmoothingPasses = 6;
        extractor = PxGetPhysicsGpu()->createSparseGridIsosurfaceExtractor(f.cuda,grid,params,f.size,maxVertices,maxTriangles);
        if (!extractor) throw std::runtime_error("PhysX GPU fluid surface extractor initialization failed");
    }
    void extract() {
        auto& f = owner;
        PxScopedCudaLock lock(*f.cuda);
        check(f.mapResources(3,meshInterop.data(),nullptr),"Map fluid mesh for PhysX");
        std::array<CUdeviceptr,3> device{};
        for (int i = 0; i < 3; ++i) {
            size_t bytes = 0;
            check(f.mappedPointer(&device[i],&bytes,meshInterop[i]),"Map fluid surface output");
        }
        extractor->setResultBufferDevice(reinterpret_cast<PxVec4*>(device[0]),reinterpret_cast<PxU32*>(device[2]),reinterpret_cast<PxVec4*>(device[1]));
        if (f.simulated) {
            extractor->extractIsosurface(reinterpret_cast<PxVec4*>(f.smoothed),f.size,nullptr,nullptr,
                PxParticlePhaseFlag::eParticlePhaseFluid,nullptr,reinterpret_cast<PxVec4*>(f.axes[0]),
                reinterpret_cast<PxVec4*>(f.axes[1]),reinterpret_cast<PxVec4*>(f.axes[2]),f.spacing/0.6F);
        } else extractor->extractIsosurface(f.particles->getPositionInvMasses(),f.size,nullptr);
        check(f.cuda->getCudaContext()->streamSynchronize(nullptr),"Finish PhysX fluid surface extraction");
        meshTriangles = extractor->getNumTriangles();
        check(f.unmapResources(3,meshInterop.data(),nullptr),"Unmap PhysX surface for drawing");
        if (meshTriangles >= extractor->getMaxTriangles() || extractor->getNumVertices() >= extractor->getMaxVertices()) {
            throw std::runtime_error("PhysX fluid surface exceeded its geometry budget");
        }
    }
    void clearTargets() {
        glDeleteFramebuffers(2,fbo.data());
        glDeleteTextures(2,texture.data());
        glDeleteRenderbuffers(1,&depthBuffer);
        glDeleteTextures(1,&opaqueColor);
        glDeleteTextures(1,&opaqueDepth);
        fbo = {}; texture = {};
        depthBuffer = opaqueColor = opaqueDepth = 0;
    }
    static void allocateTexture(GLuint name, GLenum internal, GLenum format, int w, int h) {
        glBindTexture(GL_TEXTURE_2D,name);
        glTexImage2D(GL_TEXTURE_2D,0,internal,w,h,0,format,GL_FLOAT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    void resize(int w, int h) {
        if (w == width && h == height) return;
        clearTargets(); width = w; height = h;
        glGenFramebuffers(2,fbo.data()); glGenTextures(2,texture.data());
        glGenRenderbuffers(1,&depthBuffer);
        glBindRenderbuffer(GL_RENDERBUFFER,depthBuffer);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,w,h);
        for (int i = 0; i < 2; ++i) {
            allocateTexture(texture[i],i == 0 ? GL_RGBA32F : GL_R32F,i == 0 ? GL_RGBA : GL_RED,w,h);
            glBindFramebuffer(GL_FRAMEBUFFER,fbo[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture[i],0);
            if (i == 0) glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depthBuffer);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("Fluid surface framebuffer incomplete");
        }
        glGenTextures(1,&opaqueColor);
        allocateTexture(opaqueColor,GL_RGBA8,GL_RGBA,w,h);
        glGenTextures(1,&opaqueDepth);
        allocateTexture(opaqueDepth,GL_DEPTH_COMPONENT32F,GL_DEPTH_COMPONENT,w,h);
    }
    ~Renderer() {
        {
            PxScopedCudaLock lock(*owner.cuda);
            if (extractor) { extractor->release(); delete extractor; }
            for (auto resource : meshInterop) if (resource) owner.unregisterResource(resource);
        }
        glDeleteBuffers(3,meshBuffers.data()); glDeleteVertexArrays(1,&meshVao);
        clearTargets(); UnloadShader(particles); UnloadShader(shade); UnloadShader(meshShader);
    }
};

void ParticleFluid::draw(Matrix view, Matrix projection, int width, int height) {
    auto& f = *impl;
    rlDrawRenderBatchActive();
    f.copyToOpenGL();
    GLint target = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&target);
    if (!f.renderer) f.renderer = std::make_unique<Impl::Renderer>(f);
    auto& r = *f.renderer;
    r.resize(width,height);
    r.extract();
    glBindFramebuffer(GL_FRAMEBUFFER,target);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,r.opaqueColor);
    glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,width,height);
    glBindTexture(GL_TEXTURE_2D,r.opaqueDepth);
    glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,width,height);
    glViewport(0,0,width,height);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindVertexArray(f.vao);
    glBindFramebuffer(GL_FRAMEBUFFER,r.fbo[0]);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const GLuint particles = r.particles.id;
    glBindVertexArray(r.meshVao);
    glUseProgram(r.meshShader.id);
    uniformMatrix(r.meshShader.id,"view",view);
    uniformMatrix(r.meshShader.id,"projection",projection);
    glUniform2f(glGetUniformLocation(r.meshShader.id,"screenSize"),static_cast<float>(width),static_cast<float>(height));
    sampler(r.meshShader.id,"opaqueDepth",r.opaqueDepth,1);
    glDrawElements(GL_TRIANGLES,r.meshTriangles*3,GL_UNSIGNED_INT,nullptr);
    glBindVertexArray(f.vao);
    glUseProgram(particles);
    uniformMatrix(particles,"view",view);
    uniformMatrix(particles,"projection",projection);
    const float radius = f.spacing * 0.95F;
    glUniform1f(glGetUniformLocation(particles,"radius"),radius);
    glUniform2f(glGetUniformLocation(particles,"screenSize"),static_cast<float>(width),static_cast<float>(height));
    sampler(particles,"positions",f.positionsTexture,0,GL_TEXTURE_BUFFER);
    sampler(particles,"opaqueDepth",r.opaqueDepth,1);
    glBindFramebuffer(GL_FRAMEBUFFER,r.fbo[1]);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE,GL_ONE);
    glUniform1i(glGetUniformLocation(particles,"thicknessPass"),1);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP,0,4,f.size);
    glDisable(GL_BLEND);
    GLuint source = r.texture[0];
    glBindFramebuffer(GL_FRAMEBUFFER,target);
    const GLuint shade = r.shade.id;
    glUseProgram(shade);
    uniformMatrix(shade,"projection",projection);
    uniformMatrix(shade,"inverseProjection",MatrixInvert(projection));
    uniformMatrix(shade,"inverseView",MatrixInvert(view));
    glUniform2f(glGetUniformLocation(shade,"screenSize"),static_cast<float>(width),static_cast<float>(height));
    sampler(shade,"surfaceDepth",source,0);
    sampler(shade,"thickness",r.texture[1],1);
    sampler(shade,"opaqueColor",r.opaqueColor,2);
    sampler(shade,"opaqueDepth",r.opaqueDepth,3);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDrawArrays(GL_TRIANGLES,0,3);
    glUseProgram(0);
    glBindVertexArray(0);
    for (int slot = 0; slot < 4; ++slot) { glActiveTexture(GL_TEXTURE0+slot); glBindTexture(GL_TEXTURE_2D,0); }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER,0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) throw std::runtime_error("Fluid rendering OpenGL error " + std::to_string(error));
}
