#ifndef STUDIOCACHE_H
#define STUDIOCACHE_H

namespace Render
{

struct StudioSubMesh
{
    int bonePalette;
    int skinref;

    unsigned indexOffsetInBytes;
    unsigned indexCount;
    unsigned baseVertex;
};

struct StudioSubModel
{
    int subMeshCount;
    StudioSubMesh *subMeshes;
};

struct StudioBodypart
{
    StudioSubModel *models;
};

struct StudioVertex
{
    Vector3 position;
    Vector2 texCoord;

    // store the bone as a float so we don't have to use glVertexAttribIPointer
    float bone;

    // pack normals to 24 bits... GL_INT_2_10_10_10_REV not available
    // and this is generally enough resolution (valve studiomdl quantizes
    // to 2 degrees of accuracy, int8 component should have around 0.6)
    int8_t normal[4];

    // only used for glowshell
    int8_t smoothNormal[4];
};

struct StudioBonePalette
{
    int boneCount;
    uint8_t bones[MAXSTUDIOBONES];
};

struct StudioCache
{
    char fileName[64];
    int fileLength;

    StudioBodypart *bodyparts;

    GLuint vertexBuffer;
    GLuint indexBuffer;

    // needed for when UBOs are not available and we encounter
    // a model that uses more than 72 bones for skinning...
    int paletteCount;
    StudioBonePalette *palettes;
};

StudioCache *studioCacheGet(model_t *model, studiohdr_t *header);
StudioCache *studioCacheGet(cl_entity_t *entity);

void studioCacheTouchAll();

}

#endif
