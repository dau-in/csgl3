#ifndef R_DECALS_H
#define R_DECALS_H

namespace Render
{

struct gl3_worldmodel_t;
struct gl3_surface_t;
struct gl3_brushvert_t;

// oversized on purpose
constexpr int MaxDecals = 512;

// rough approximation, gets checked so we won't overflow
constexpr int MaxDecalVertices = MaxDecals * 4;
constexpr int MaxDecalIndices = MaxDecalVertices * 3;

void decalInit();

// queue decals associated with this surface for rendering
void decalAddFromSurface(gl3_worldmodel_t *model, gl3_surface_t *surface);

// draw the queued decals, called for each brush model
// relies on state set by the brush renderer (shaders, etc.)
int decalDrawAll(uint16_t *spanData, int spanOffsetBytes, int curIndexCount);

// callback from internalSurfaceDecals
void decalAdd(GLuint textureName, const gl3_brushvert_t *vertices, int vertexCount);

}

#endif // R_DECALS_H
