#include "stdafx.h"
#include "studio_cache.h"
#include "studio_misc.h"
#include <meshoptimizer.h>
#include "memory.h"

namespace Render
{

// max models cached, models are never freed so this should be large
constexpr int StudioCacheMaxBits = 12; // 4096 entries

static int s_cacheCount;
static StudioCache s_caches[1 << StudioCacheMaxBits];

// studiohdr_t::name
struct NameField
{
    char name[60];
    uint16_t magic; // NameFieldMagic
    uint16_t index; // indexes into s_caches
};

constexpr uint16_t NameFieldMagic = (0 << 0) | (3 << 8);

// intrinsic not worth it since we can't POPCNT anyway
static int PopCount(uint32_t word)
{
    word = word - ((word >> 1) & 0x55555555u);
    word = (word & 0x33333333u) + ((word >> 2) & 0x33333333u);
    word = (word + (word >> 4)) & 0x0f0f0f0fu;
    return static_cast<int>((word * 0x01010101u) >> 24);
}

// std::bitset won't do because of its nontrivial constructor
struct BoneSet
{
    static constexpr int WordBits = 32;
    static constexpr int WordCount = MAXSTUDIOBONES / WordBits;

    uint32_t words[WordCount];

    bool Test(int bone) const
    {
        GL3_ASSERT(bone >= 0 && bone < MAXSTUDIOBONES);
        return (words[bone / WordBits] & (1u << (bone % WordBits))) != 0;
    }

    void Set(int bone)
    {
        GL3_ASSERT(bone >= 0 && bone < MAXSTUDIOBONES);
        words[bone / WordBits] |= (1u << (bone % WordBits));
    }

    int Count() const
    {
        int result = 0;

        for (int i = 0; i < WordCount; i++)
        {
            result += PopCount(words[i]);
        }

        return result;
    }

    // returns (this | other).Count()
    int OrCount(const BoneSet &other) const
    {
        int result = 0;

        for (int i = 0; i < WordCount; i++)
        {
            result += PopCount(words[i] | other.words[i]);
        }

        return result;
    }

    void Or(const BoneSet &other)
    {
        for (int i = 0; i < BoneSet::WordCount; i++)
        {
            words[i] |= other.words[i];
        }
    }
};

// fat version of StudioSubMesh for building
struct BuildMesh : StudioSubMesh
{
    BoneSet bones;
    int vertexStart;
    int vertexCount;
};

struct BuildBuffer
{
    StudioVertex *vertices;
    int vertexCount;

    GLushort *indices;
    int indexCount;

    BuildMesh *meshes;
    int meshCount;

    // current base vertex, only increment when we actually have to
    int baseVertex;
};

struct BoneRemap
{
    uint8_t bones[MAXSTUDIOBONES];
};

// tricmd vertex
struct Vertex
{
    short position;
    short normal;
    short s, t;
};

struct Triangle
{
    Vertex verts[3];
};

struct MeshData
{
    // pulled from the submodel
    Vector3 *positions;
    Vector3 *normals;
    byte *vertinfo;

    // computed by us per submodel
    Vector3 *smoothNormals;

    // per mesh
    Vector2 texCoordScale;
};

struct BuildSubModel
{
    StudioSubModel *model;
    int firstSubMesh;
    int subMeshCount;
};

// read tricmds into triangle soup
static void MakeTriangleSoup(Triangle *soup, const short *tricmds)
{
    while (1)
    {
        int count = *tricmds++;
        if (!count)
        {
            break;
        }

        bool trifan = false;

        if (count < 0)
        {
            trifan = true;
            count = -count;
        }

        const Vertex *vertices = reinterpret_cast<const Vertex *>(tricmds);
        tricmds += (4 * count);

        if (trifan)
        {
            for (int i = 2; i < count; i++)
            {
                Triangle *triangle = soup++;
                triangle->verts[0] = vertices[0];
                triangle->verts[1] = vertices[i - 1];
                triangle->verts[2] = vertices[i];
            }
        }
        else
        {
            for (int i = 2; i < count; i++)
            {
                if (!(i % 2))
                {
                    Triangle *triangle = soup++;
                    triangle->verts[0] = vertices[i - 2];
                    triangle->verts[1] = vertices[i - 1];
                    triangle->verts[2] = vertices[i];
                }
                else
                {
                    Triangle *triangle = soup++;
                    triangle->verts[0] = vertices[i - 1];
                    triangle->verts[1] = vertices[i - 2];
                    triangle->verts[2] = vertices[i];
                }
            }
        }
    }
}

// union of every bone the triangle skins
static BoneSet TriangleBoneSet(const Triangle &triangle, const byte *vertinfo)
{
    BoneSet bones{};

    for (const Vertex &vert : triangle.verts)
    {
        bones.Set(vertinfo[vert.position]);
    }

    return bones;
}

// union of every bone the soup skins
static BoneSet SoupBoneSet(const Triangle *soup, int triangleCount, const byte *vertinfo)
{
    BoneSet bones{};

    for (int i = 0; i < triangleCount; i++)
    {
        for (const Vertex &vert : soup[i].verts)
        {
            bones.Set(vertinfo[vert.position]);
        }
    }

    return bones;
}

// use sorted bones as a key for sorting
static uint32_t TriangleBoneKey(const Triangle &triangle, const byte *vertinfo)
{
    uint8_t bones[3];
    for (int i = 0; i < 3; i++)
    {
        bones[i] = vertinfo[triangle.verts[i].position];
    }

    if (bones[0] > bones[1])
    {
        std::swap(bones[0], bones[1]);
    }

    if (bones[1] > bones[2])
    {
        std::swap(bones[1], bones[2]);
    }

    if (bones[0] > bones[1])
    {
        std::swap(bones[0], bones[1]);
    }

    return (bones[0] << 16) | (bones[1] << 8) | bones[2];
}

// group triangles that skin the same bones together (also
// tested without this, palette count blows up with some models)
static void SortTriangleSoup(Triangle *soup, int triangleCount, const byte *vertinfo)
{
    auto compare = [vertinfo](const Triangle &a, const Triangle &b)
    {
        return TriangleBoneKey(a, vertinfo) < TriangleBoneKey(b, vertinfo);
    };

    std::sort(soup, soup + triangleCount, compare);
}

static void PackNormal(int8_t (&dest)[4], const Vector3 &source)
{
    GL3_ASSERT(source.x >= -1 && source.x <= 1);
    GL3_ASSERT(source.y >= -1 && source.y <= 1);
    GL3_ASSERT(source.z >= -1 && source.z <= 1);

    dest[0] = (int8_t)Lerp(INT8_MIN, INT8_MAX, 0.5f + (source.x * 0.5f));
    dest[1] = (int8_t)Lerp(INT8_MIN, INT8_MAX, 0.5f + (source.y * 0.5f));
    dest[2] = (int8_t)Lerp(INT8_MIN, INT8_MAX, 0.5f + (source.z * 0.5f));
    dest[3] = 0;
}

// running meshoptimizer afterwards is quite important now that we're raping the meshes for processing
// does the whole meshoptimizer juggle, returns unique vertex count and fills the index buffer
static int OptimizeMesh(StudioVertex *vertices, int vertexCount, unsigned *indices, unsigned *remap)
{
    size_t uniqueVertexCount = meshopt_generateVertexRemap(
        remap,
        nullptr,
        vertexCount,
        vertices,
        vertexCount,
        sizeof(*vertices));

    meshopt_remapIndexBuffer(
        indices,
        nullptr,
        vertexCount,
        remap);

    meshopt_remapVertexBuffer(
        vertices,
        vertices,
        vertexCount,
        sizeof(*vertices),
        remap);

    meshopt_optimizeVertexCache(
        indices,
        indices,
        vertexCount,
        uniqueVertexCount);

    uniqueVertexCount = meshopt_optimizeVertexFetch(
        vertices,
        indices,
        vertexCount,
        vertices,
        uniqueVertexCount,
        sizeof(*vertices));

    return static_cast<int>(uniqueVertexCount);
}

static void AppendBuildMesh(
    BuildBuffer &build,
    const MeshData &data,
    const Triangle *triangles,
    int triangleCount,
    const BoneSet &bones,
    int skinref)
{
    GL3_ASSERT(triangleCount > 0);

    TempMemoryScope temp;

    int indexStart = build.indexCount;
    int vertexStart = build.vertexCount;

    // get at the underlying vertcies
    static_assert(sizeof(Vertex) * 3 == sizeof(Triangle), "bruh");
    const Vertex *soup = &triangles[0].verts[0];
    int soupSize = triangleCount * 3;

    for (int i = 0; i < soupSize; i++)
    {
        const Vertex &trivert = soup[i];
        StudioVertex vert{};

        vert.position = data.positions[trivert.position];
        PackNormal(vert.normal, data.normals[trivert.normal]);
        PackNormal(vert.smoothNormal, data.smoothNormals[trivert.position]);

        vert.texCoord.x = data.texCoordScale.x * trivert.s;
        vert.texCoord.y = data.texCoordScale.y * trivert.t;

        uint8_t bone = data.vertinfo[trivert.position];
        vert.bone = bone;

        build.vertices[build.vertexCount++] = vert;
    }

    // one index per soup vertex
    unsigned *indices = temp.Alloc<unsigned>(soupSize);
    unsigned *remap = temp.Alloc<unsigned>(soupSize);

    int uniqueVertexCount = OptimizeMesh(&build.vertices[vertexStart], soupSize, indices, remap);

    // should never happen, but check for completeness sake
    int maxVertexCount = UINT16_MAX + 1u;
    if (uniqueVertexCount > maxVertexCount)
    {
        // FIXME: completely useless error message
        platformError("Too many vertices in a mesh (%d, max %d)", uniqueVertexCount, maxVertexCount);
    }

    // update vertex count to deduped value
    build.vertexCount = vertexStart + uniqueVertexCount;

    if (build.vertexCount - build.baseVertex > maxVertexCount)
    {
        // this mesh won't fit at the current one
        build.baseVertex = vertexStart;
    }

    // append indices remapped onto baseVertex
    int delta = vertexStart - build.baseVertex;

    for (int i = 0; i < soupSize; i++)
    {
        build.indices[build.indexCount++] = static_cast<GLushort>(indices[i] + delta);
    }

    BuildMesh &dest = build.meshes[build.meshCount++];
    dest.skinref = skinref;
    dest.indexOffsetInBytes = static_cast<unsigned>(indexStart * sizeof(GLushort));
    dest.indexCount = static_cast<unsigned>(build.indexCount - indexStart);
    dest.baseVertex = static_cast<unsigned>(build.baseVertex);
    dest.bones = bones;
    dest.vertexStart = vertexStart;
    dest.vertexCount = uniqueVertexCount;
}

// splits meshes if they use too many bones
static void MakeBuildMeshes(
    BuildBuffer &build,
    const MeshData &source,
    Triangle *triangleSoup,
    int triangleCount,
    int maxBones,
    int skinref)
{
    GL3_ASSERT(triangleCount > 0);

    BoneSet meshBones = SoupBoneSet(triangleSoup, triangleCount, source.vertinfo);
    if (meshBones.Count() <= maxBones)
    {
        // no need to do any of this shit, thank fuck for that
        AppendBuildMesh(build, source, triangleSoup, triangleCount, meshBones, skinref);
        return;
    }

    SortTriangleSoup(triangleSoup, triangleCount, source.vertinfo);

    int chunkStart = 0;
    BoneSet chunkBones{};

    for (int i = 0; i < triangleCount; i++)
    {
        BoneSet triangleBones = TriangleBoneSet(triangleSoup[i], source.vertinfo);

        BoneSet combinedBones = chunkBones;
        combinedBones.Or(triangleBones);

        if (combinedBones.Count() > maxBones)
        {
            GL3_ASSERT(i > chunkStart);
            AppendBuildMesh(build, source, &triangleSoup[chunkStart], i - chunkStart, chunkBones, skinref);
            chunkStart = i;
            chunkBones = triangleBones;
        }
        else
        {
            chunkBones = combinedBones;
        }
    }

    GL3_ASSERT(chunkStart < triangleCount);
    AppendBuildMesh(build, source, &triangleSoup[chunkStart], triangleCount - chunkStart, chunkBones, skinref);
}

static void AccumulateSmoothNormals(const short *tricmds, const Vector3 *normals, Vector3 *smoothNormals)
{
    while (1)
    {
        int count = *tricmds++;
        if (!count)
        {
            break;
        }

        if (count < 0)
        {
            count = -count;
        }

        for (int i = 0; i < count; i++)
        {
            smoothNormals[tricmds[0]] += normals[tricmds[1]];
            tricmds += 4;
        }
    }
}

static bool PaletteFits(const BoneSet &palette, const BoneSet &bones, int maxBones)
{
    return palette.OrCount(bones) <= maxBones;
}

// build palettes and assign their indices to meshes
static int BuildBonePalettes(BuildMesh *meshes, int meshCount, BoneSet *palettes, int maxBones)
{
    int paletteCount = 0;
    int current = -1;

    for (int i = 0; i < meshCount; i++)
    {
        BuildMesh &mesh = meshes[i];

        if (current < 0 || !PaletteFits(palettes[current], mesh.bones, maxBones))
        {
            current = -1;

            for (int j = 0; j < paletteCount; j++)
            {
                if (PaletteFits(palettes[j], mesh.bones, maxBones))
                {
                    current = j;
                    break;
                }
            }

            if (current < 0)
            {
                current = paletteCount++;
            }
        }

        palettes[current].Or(mesh.bones);
        mesh.bonePalette = current;
    }

    GL3_ASSERT(paletteCount > 0);
    return paletteCount;
}

static void AssignBonePalettes(StudioCache *cache, BuildBuffer &build, int maxBones)
{
    TempMemoryScope temp;

    // overkill allocation since we possibly can't know
    BoneSet *paletteSets = temp.Alloc<BoneSet>(build.meshCount);

    cache->paletteCount = BuildBonePalettes(build.meshes, build.meshCount, paletteSets, maxBones);
    cache->palettes = memoryStaticAlloc<StudioBonePalette>(cache->paletteCount);

    BoneRemap *boneRemaps = temp.Alloc<BoneRemap>(cache->paletteCount);

    for (int i = 0; i < cache->paletteCount; i++)
    {
        GL3_ASSERT(paletteSets[i].Count() <= maxBones);

        StudioBonePalette &palette = cache->palettes[i];
        BoneRemap &boneRemap = boneRemaps[i];

        for (int j = 0; j < MAXSTUDIOBONES; j++)
        {
            if (paletteSets[i].Test(j))
            {
                boneRemap.bones[j] = static_cast<uint8_t>(palette.boneCount);
                palette.bones[palette.boneCount++] = static_cast<uint8_t>(j);
            }
        }
    }

    // update vertex bone indices
    for (int i = 0; i < build.meshCount; i++)
    {
        const BuildMesh &mesh = build.meshes[i];

        int paletteIndex = mesh.bonePalette;
        GL3_ASSERT(paletteIndex >= 0 && paletteIndex < cache->paletteCount);
        const BoneRemap &boneRemap = boneRemaps[paletteIndex];

        int begin = mesh.vertexStart;
        int end = begin + mesh.vertexCount;

        for (int j = begin; j < end; j++)
        {
            float &bone = build.vertices[j].bone;
            bone = boneRemap.bones[static_cast<int>(bone)];
        }
    }
}

static void CountSubModelsAndTriangles(studiohdr_t *header, mstudiobodyparts_t *bodyparts, int &totalSubModels, int &totalTriangles)
{
    totalSubModels = 0;
    totalTriangles = 0;

    for (int i = 0; i < header->numbodyparts; i++)
    {
        mstudiobodyparts_t *bodypart = &bodyparts[i];
        mstudiomodel_t *models = (mstudiomodel_t *)((byte *)header + bodypart->modelindex);

        totalSubModels += bodypart->nummodels;

        for (int j = 0; j < bodypart->nummodels; j++)
        {
            mstudiomodel_t *submodel = &models[j];
            mstudiomesh_t *meshes = (mstudiomesh_t *)((byte *)header + submodel->meshindex);

            for (int k = 0; k < submodel->nummesh; k++)
            {
                // FIXME: does studiomdl actually write numtris? bruh
                totalTriangles += meshes[k].numtris;
            }
        }
    }
}

// returns memory allocated with the provided scope
static Vector3 *ComputeSmoothNormals(TempMemoryScope &temp, studiohdr_t *header, mstudiomodel_t *submodel)
{
    mstudiomesh_t *meshes = (mstudiomesh_t *)((byte *)header + submodel->meshindex);
    Vector3 *normals = (Vector3 *)((byte *)header + submodel->normindex);

    Vector3 *smoothNormals = temp.Alloc<Vector3>(submodel->numverts);

    for (int k = 0; k < submodel->nummesh; k++)
    {
        mstudiomesh_t *mesh = &meshes[k];
        short *tricmds = (short *)((byte *)header + mesh->triindex);
        AccumulateSmoothNormals(tricmds, normals, smoothNormals);
    }

    for (int k = 0; k < submodel->numverts; k++)
    {
        VectorNormalize(smoothNormals[k]);
    }

    return smoothNormals;
}

static void BuildStudioVertexBuffer(StudioCache *cache, model_t *model, studiohdr_t *header)
{
    mstudiobodyparts_t *bodyparts = (mstudiobodyparts_t *)((byte *)header + header->bodypartindex);

    // count total submodels and triangles in the model for allocations
    int totalSubModels, totalTriangles;
    CountSubModelsAndTriangles(header, bodyparts, totalSubModels, totalTriangles);

    TempMemoryScope temp;

    BuildBuffer build{};
    build.vertices = temp.Alloc<StudioVertex>(3 * totalTriangles);
    build.indices = temp.Alloc<GLushort>(3 * totalTriangles);
    build.meshes = temp.Alloc<BuildMesh>(totalTriangles); // way too big

    studiohdr_t *textureheader = studioTextureHeader(model, header);
    short *skins = (short *)((byte *)textureheader + textureheader->skinindex);
    mstudiotexture_t *textures = (mstudiotexture_t *)((byte *)textureheader + textureheader->textureindex);

    cache->bodyparts = memoryStaticAlloc<StudioBodypart>(header->numbodyparts);

    int maxBones = GLAD_GL_ARB_uniform_buffer_object ? MAXSTUDIOBONES : MAX_BONES_SM3;

    int subModelCount = 0;
    BuildSubModel *subModels = temp.Alloc<BuildSubModel>(totalSubModels);

    for (int i = 0; i < header->numbodyparts; i++)
    {
        mstudiobodyparts_t *bodypart = &bodyparts[i];
        mstudiomodel_t *models = (mstudiomodel_t *)((byte *)header + bodypart->modelindex);

        StudioBodypart *mem_bodypart = &cache->bodyparts[i];
        mem_bodypart->models = memoryStaticAlloc<StudioSubModel>(bodypart->nummodels);

        for (int j = 0; j < bodypart->nummodels; j++)
        {
            mstudiomodel_t *submodel = &models[j];
            mstudiomesh_t *meshes = (mstudiomesh_t *)((byte *)header + submodel->meshindex);

            // temporay working memory for computing smooth normals for glowshell
            TempMemoryScope submodelTemp;

            // most of the mesh data is per submodel, so set it up here
            MeshData data{};
            data.positions = (Vector3 *)((byte *)header + submodel->vertindex);
            data.normals = (Vector3 *)((byte *)header + submodel->normindex);
            data.vertinfo = (byte *)((byte *)header + submodel->vertinfoindex);
            data.smoothNormals = ComputeSmoothNormals(submodelTemp, header, submodel);

            int meshStart = build.meshCount;

            for (int k = 0; k < submodel->nummesh; k++)
            {
                mstudiomesh_t *mesh = &meshes[k];
                mstudiotexture_t *texture = &textures[skins[mesh->skinref]];
                short *tricmds = (short *)((byte *)header + mesh->triindex);

                // NOTE: this assumes that all skingroup textures are the same size
                data.texCoordScale = Vector2{
                    1.0f / (float)texture->width,
                    1.0f / (float)texture->height
                };

                // temporary working memory for the traingle soup...
                TempMemoryScope meshTemp;
                Triangle *triangleSoup = meshTemp.Alloc<Triangle>(mesh->numtris);
                MakeTriangleSoup(triangleSoup, tricmds);

                MakeBuildMeshes(build, data, triangleSoup, mesh->numtris, maxBones, mesh->skinref);
            }

            BuildSubModel &dest = subModels[subModelCount++];
            dest.model = &mem_bodypart->models[j];
            dest.firstSubMesh = meshStart;
            dest.subMeshCount = build.meshCount - meshStart;
        }
    }

    AssignBonePalettes(cache, build, maxBones);

    for (int i = 0; i < subModelCount; i++)
    {
        const BuildSubModel &subModel = subModels[i];
        StudioSubModel *dest = subModel.model;

        dest->subMeshCount = subModel.subMeshCount;
        dest->subMeshes = memoryStaticAlloc<StudioSubMesh>(subModel.subMeshCount);

        for (int j = 0; j < subModel.subMeshCount; j++)
        {
            // slice it off
            StudioSubMesh &mesh = build.meshes[subModel.firstSubMesh + j];
            dest->subMeshes[j] = mesh;
        }
    }

    glGenBuffers(1, &cache->vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, cache->vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, build.vertexCount * sizeof(StudioVertex), build.vertices, GL_STATIC_DRAW);

    glGenBuffers(1, &cache->indexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cache->indexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, build.indexCount * sizeof(GLushort), build.indices, GL_STATIC_DRAW);
}

static void BuildStudioCache(StudioCache *cache, model_t *model, studiohdr_t *header)
{
    Q_strcpy(cache->fileName, model->name);
    cache->fileLength = header->length;

    // build vertex and index buffer
    BuildStudioVertexBuffer(cache, model, header);
}

static NameField *GetNameField(studiohdr_t *header)
{
    return reinterpret_cast<NameField *>(header->name);
}

static int GetCacheIndex(studiohdr_t *header)
{
    NameField *field = GetNameField(header);
    if (field->magic != NameFieldMagic)
    {
        return -1;
    }

    return field->index;
}

static void SetCacheIndex(studiohdr_t *header, int index)
{
    NameField *field = GetNameField(header);
    field->magic = NameFieldMagic;
    field->index = static_cast<uint16_t>(index);
}

static void ReleaseCache(StudioCache *cache)
{
    // we can't free the memory allocated with memoryStaticAlloc, but
    // those allocations are very small so doesn't matter
    // free the gpu buffers though
    glDeleteBuffers(1, &cache->vertexBuffer);
    glDeleteBuffers(1, &cache->indexBuffer);
    memset(cache, 0, sizeof(*cache));
}

StudioCache *studioCacheGet(model_t *model, studiohdr_t *header)
{
    // see if the cache index is in the header
    int cacheIndex = GetCacheIndex(header);
    if (cacheIndex != -1)
    {
        return &s_caches[cacheIndex];
    }

    // it was not, fuck
    GL3_ASSERT(model->name[0]);

    uint32_t hash = HashString(model->name);
    uint32_t mask = (1 << StudioCacheMaxBits) - 1;
    uint32_t step = (hash >> (32 - StudioCacheMaxBits)) | 1;

    for (int i = (hash + step) & mask;; i = (i + step) & mask)
    {
        StudioCache *cache = &s_caches[i];
        if (!cache->fileName[0])
        {
            if (s_cacheCount + 1 >= (1 << StudioCacheMaxBits))
            {
                platformError("Studio model cache full");
            }

            s_cacheCount++;

            BuildStudioCache(cache, model, header);
            SetCacheIndex(header, i);
            return cache;
        }

        if (!strcmp(model->name, cache->fileName))
        {
            // see if the model has changed (flush command)
            if (header->length != cache->fileLength)
            {
                ReleaseCache(cache);
                BuildStudioCache(cache, model, header);
            }

            // update the header
            SetCacheIndex(header, i);
            return cache;
        }
    }
}

StudioCache *studioCacheGet(cl_entity_t *entity)
{
    model_t *model = entity->model;
    if (!model)
        return nullptr;

    if (model->type != mod_studio)
        return nullptr;

    studiohdr_t *studiohdr = static_cast<studiohdr_t *>(g_engineStudio.Mod_Extradata(model));
    if (!studiohdr)
        return nullptr;

    return studioCacheGet(model, studiohdr);
}

void studioCacheTouchAll()
{
    for (int i = 2;; i++)
    {
        model_t *model = g_engineStudio.GetModelByIndex(i);
        if (!model)
        {
            break;
        }

        if (model->type != mod_studio)
        {
            continue;
        }

        studiohdr_t *studiohdr = static_cast<studiohdr_t *>(g_engineStudio.Mod_Extradata(model));
        if (studiohdr)
        {
            studioCacheGet(model, studiohdr);
        }
    }
}

}
