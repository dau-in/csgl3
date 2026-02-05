#include "stdafx.h"
#include "beam.h"
#include "entity.h"
#include "immediate.h"
#include "random.h"
#include "particle.h"
#include "internal.h"
#include "hudgl3.h"
#include "triapigl3.h"

#define NOT_IMPL() \
    do \
    { \
        static bool bitched; \
        if (!bitched) \
            g_engfuncs.Con_Printf("%s not implemented\n", __FUNCTION__); \
        bitched = true; \
    } while (0)

namespace Render
{

constexpr auto MaxBeams = 128;

static BEAM s_beams[MaxBeams];

// need to use these shitty lists to stay compatible
static BEAM *s_freeBeams;
static BEAM *s_activeBeams;

constexpr int NoiseCount = 128;
static float s_noise[NoiseCount + 1];

static cvar_t *tracerlength;

void beamInit()
{
    tracerlength = g_engfuncs.pfnGetCvarPointer("tracerlength");
}

void beamClear()
{
    s_activeBeams = nullptr;

    s_freeBeams = &s_beams[0];

    for (int i = 0; i < MaxBeams; i++)
    {
        s_beams[i].next = &s_beams[i + 1];
    }

    s_beams[MaxBeams - 1].next = nullptr;
}

BEAM *beamAllocate()
{
    BEAM *beam = s_freeBeams;
    if (!beam)
    {
        return nullptr;
    }

    s_freeBeams = s_freeBeams->next;

    beam->next = s_activeBeams;
    s_activeBeams = beam;

    return beam;
}

void beamSetup(
    BEAM &beam,
    const Vector3 &start,
    const Vector3 &end,
    int modelIndex,
    float life,
    float width,
    float amplitude,
    float brightness,
    float speed)
{
    model_t *model = g_engfuncs.hudGetModelByIndex(modelIndex);
    if (!model)
    {
        return;
    }

    float clientTime = g_engfuncs.GetClientTime();

    beam.type = TE_BEAMPOINTS;
    beam.modelIndex = modelIndex;
    beam.frame = 0;
    beam.frameRate = 0;
    beam.frameCount = model->numframes; // would break with studio models
    beam.source = start;
    beam.target = end;
    beam.delta = end - start;
    beam.freq = clientTime * speed;
    beam.die = clientTime + life;
    beam.width = width;
    beam.amplitude = amplitude;
    beam.speed = speed;
    beam.brightness = brightness;

    if (amplitude >= 0.5f)
    {
        beam.segments = (int)(VectorLength(beam.delta) * 0.25f + 3.0f);
    }
    else
    {
        beam.segments = (int)(VectorLength(beam.delta) * 0.075f + 3.0f);
    }

    beam.flags = 0;
    beam.pFollowModel = nullptr;
}

static void FractalNoise(float *noise, int current)
{
    if (current < 2)
    {
        return;
    }

    int next = current / 2;

    float jitter = randomFloat(-0.125f, 0.125f) * (float)current;
    noise[next] = (noise[0] + noise[current]) * 0.5f + jitter;

    FractalNoise(&noise[next], next);
    FractalNoise(noise, next);
}

static void SineNoise(float *dest, int count)
{
    float a = 0;
    float b = F_PI / count;

    for (int i = 0; i < count; i++)
    {
        dest[i] = sinf(a);
        a += b;
    }
}

static cl_entity_t *GetBeamEntity(int index)
{
    if (index < 0)
    {
        // not used by COUNTERSTRIKE so not implemented
        //return cl_funcs.pGetUserEntity(BEAMENT_ENTITY(-index));
        return nullptr;
    }

    return g_engfuncs.GetEntityByIndex(BEAMENT_ENTITY(index));
}

bool beamCull(const Vector3 &start, const Vector3 &end)
{
    // don't perform a line cull, it causes popping
    // use sloppy sphere test, although it can be inefficient...
    Vector3 center = VectorLerp(start, end, 0.5f);
    float radius = PointDistance(center, start);
    return g_state.viewFrustum.CullSphere(center, radius);
}

static Vector3 ScreenTransform(const Vector3 &world)
{
    float x, y;
    hudWorldToScreen(world, x, y);
    return { x, y, 0 };
}

// biirredää vähä segsiä :DDD
static void DrawSegs(const Vector3 &source, const Vector3 &delta, float width, float scale, float freq, float speed, int segments, int flags)
{
    if (segments <= 1)
    {
        return;
    }

    segments = Q_min(segments, NoiseCount);

    float length = Q_max(VectorLength(delta) * 0.01f, 0.5f);

    float div = 1.0f / (float)(segments - 1);
    float vStep = length * div;

    float vLast = fmodf(freq * speed, 1.0f);

    if (flags & FBEAM_SINENOISE)
    {
        segments = Q_max(segments, 16);
        length = (float)segments * 0.1f;
        scale *= 100.0f;
    }
    else
    {
        scale *= length;
    }

    Vector3 last1, last2;
    Vector3 screen, screenLast;
    {
        screenLast = ScreenTransform(source);
        screen = ScreenTransform(source + delta * div);

        Vector3 tmp = screen - screenLast;
        VectorNormalize(tmp);

        Vector3 normal = (g_state.viewUp * tmp.x) - (g_state.viewRight * tmp.y);

        last1 = source + normal * width;
        last2 = source - normal * width;
    }

    int noiseIndex = (flags & FBEAM_SINENOISE) ? 0 : (int)(div * NoiseCount * 65536.0f);
    float brightness = (flags & FBEAM_SHADEIN) ? 0.0f : 1.0f;

    for (int i = 1; i < segments; i++)
    {
        float fraction = (float)(i * div);

        g_triapiGL3.Brightness(brightness);
        g_triapiGL3.TexCoord2f(0, vLast);
        g_triapiGL3.Vertex3fv(&last1.x);

        g_triapiGL3.Brightness(brightness);
        g_triapiGL3.TexCoord2f(1, vLast);
        g_triapiGL3.Vertex3fv(&last2.x);

        if (flags & FBEAM_SHADEIN)
        {
            brightness = fraction;
        }
        else if (flags & FBEAM_SHADEOUT)
        {
            brightness = 1.0f - fraction;
        }

        Vector3 point = source + delta * fraction;

        if (scale != 0.0f)
        {
            float jitter = scale * s_noise[noiseIndex / 65536];
            if (flags & FBEAM_SINENOISE)
            {
                point += (g_state.viewUp * sinf(F_PI * fraction * length + freq) * jitter);
                point += (g_state.viewRight * cosf(F_PI * fraction * length + freq) * jitter);
            }
            else
            {
                point += (g_state.viewUp * jitter);
                point += (g_state.viewRight * cosf(F_PI * fraction * 3.0f + freq) * jitter);
            }
        }

        screen = ScreenTransform(&point.x);

        Vector3 tmp = screen - screenLast;
        VectorNormalize(tmp);

        screenLast = screen;

        Vector3 normal = (g_state.viewUp * tmp.x) - (g_state.viewRight * tmp.y);

        last1 = point + normal * width;
        last2 = point - normal * width;

        vLast = vLast + vStep;
        g_triapiGL3.Brightness(brightness);
        g_triapiGL3.TexCoord2f(1, vLast);
        g_triapiGL3.Vertex3fv(&last2.x);
        g_triapiGL3.Brightness(brightness);
        g_triapiGL3.TexCoord2f(0, vLast);
        g_triapiGL3.Vertex3fv(&last1.x);

        vLast = fmodf(vLast, 1.0f);
        noiseIndex += (int)(div * NoiseCount * 65536.0f);
    }
}

static void DrawTorus(const Vector3 &source, const Vector3 &delta, float width, float scale, float freq, float speed, int segments)
{
    NOT_IMPL();
}

static void DrawDisk(const Vector3 &source, const Vector3 &delta, float width, float scale, float freq, float speed, int segments)
{
    NOT_IMPL();
}

static void DrawCylinder(const Vector3 &source, const Vector3 &delta, float width, float scale, float freq, float speed, int segments)
{
    NOT_IMPL();
}

static void DrawBeamFollow(BEAM &beam)
{
    NOT_IMPL();
}

static void DrawRing(const Vector3 &source, const Vector3 &delta, float width, float amplitude, float freq, float speed, int segments)
{
    NOT_IMPL();
}

static Vector3 GetAttachmentPoint(cl_entity_t *entity, int attachmentIndex)
{
    if (attachmentIndex)
    {
        return entity->attachment[attachmentIndex - 1];
    }

    if (entity == g_engfuncs.GetLocalPlayer())
    {
        // FIXME: correct? try irl
        return g_state.simOrigin;
    }

    return entity->origin;
}

static void DrawBeam(BEAM &beam, float frametime)
{
    float clientTime = g_engfuncs.GetClientTime();

    model_t *model = g_engfuncs.hudGetModelByIndex(beam.modelIndex);
    if (!model)
    {
        return;
    }

    if (beam.flags & FBEAM_SOLID)
    {
        g_triapiGL3.RenderMode(kRenderNormal);
    }
    else
    {
        g_triapiGL3.RenderMode(kRenderTransAdd);
    }

    beam.freq += frametime;

    s_noise[0] = 0;
    s_noise[NoiseCount] = 0;

    if (beam.amplitude != 0.0f)
    {
        if (beam.flags & FBEAM_SINENOISE)
        {
            SineNoise(s_noise, NoiseCount);
        }
        else
        {
            FractalNoise(s_noise, NoiseCount);
        }
    }

    if (beam.flags & (FBEAM_STARTENTITY | FBEAM_ENDENTITY))
    {
        if (beam.flags & FBEAM_STARTENTITY)
        {
            cl_entity_t *startEntity = GetBeamEntity(beam.startEntity);
            if (!startEntity)
            {
                return;
            }

            if (startEntity->model && (!beam.pFollowModel || beam.pFollowModel == startEntity->model))
            {
                beam.source = GetAttachmentPoint(startEntity, BEAMENT_ATTACHMENT(beam.startEntity));
                beam.flags |= FBEAM_STARTVISIBLE;

                if (!beam.pFollowModel)
                {
                    beam.pFollowModel = startEntity->model;
                }
            }
            else
            {
                if (!(beam.flags & FBEAM_FOREVER))
                {
                    beam.flags &= ~FBEAM_STARTENTITY;
                }
            }
        }

        if (beam.flags & FBEAM_ENDENTITY)
        {
            cl_entity_t *endEntity = GetBeamEntity(beam.endEntity);
            if (!endEntity)
            {
                return;
            }

            if (endEntity->model)
            {
                beam.target = GetAttachmentPoint(endEntity, BEAMENT_ATTACHMENT(beam.endEntity));
                beam.flags |= FBEAM_ENDVISIBLE;
            }
            else
            {
                if (!(beam.flags & FBEAM_FOREVER))
                {
                    beam.flags &= ~FBEAM_ENDENTITY;
                    beam.die = clientTime;
                }

                return;
            }
        }

        if ((beam.flags & (FBEAM_STARTENTITY | FBEAM_STARTVISIBLE)) == FBEAM_STARTENTITY)
        {
            return;
        }

        Vector3 delta = beam.target - beam.source;
        float deltaLength = VectorLength(delta);

        if (deltaLength > 1e-7f)
        {
            beam.delta = delta;
        }
        else
        {
            deltaLength = VectorLength(beam.delta);
        }

        /* FIXME: dupe code? */
        if (beam.amplitude < 0.5f)
        {
            beam.segments = static_cast<int>(deltaLength * 0.075f + 3.0f);
        }
        else
        {
            beam.segments = static_cast<int>(deltaLength * 0.25f + 3.0f);
        }
    }

    if (beam.type == TE_BEAMPOINTS && beamCull(beam.source, beam.target))
    {
        return;
    }

    int frame = static_cast<int>(beam.frameRate * clientTime + beam.frame) % beam.frameCount;
    if (!g_triapiGL3.SpriteTexture(model, frame))
    {
        return;
    }

    beam.t = (beam.die - clientTime) + beam.freq;
    if (beam.t != 0.0f)
    {
        beam.t = 1.0f - (beam.freq / beam.t);
    }

    if (beam.flags & FBEAM_FADEIN)
    {
        g_triapiGL3.Color4f(beam.r, beam.g, beam.b, beam.t * beam.brightness);
    }
    else if (beam.flags & FBEAM_FADEOUT)
    {
        g_triapiGL3.Color4f(beam.r, beam.g, beam.b, (1.0f - beam.t) * beam.brightness);
    }
    else
    {
        g_triapiGL3.Color4f(beam.r, beam.g, beam.b, beam.brightness);
    }

    switch (beam.type)
    {
    case TE_BEAMPOINTS:
        g_triapiGL3.Begin(TRI_QUADS);
        DrawSegs(
            beam.source,
            beam.delta,
            beam.width,
            beam.amplitude,
            beam.freq,
            beam.speed,
            beam.segments,
            beam.flags);
        g_triapiGL3.End();
        break;

    case TE_BEAMTORUS:
        g_triapiGL3.Begin(TRI_QUAD_STRIP);
        DrawTorus(
            beam.source,
            beam.delta,
            beam.width,
            beam.amplitude,
            beam.freq,
            beam.speed,
            beam.segments);
        g_triapiGL3.End();
        break;

    case TE_BEAMDISK:
        g_triapiGL3.Begin(TRI_QUAD_STRIP);
        DrawDisk(
            beam.source,
            beam.delta,
            beam.width,
            beam.amplitude,
            beam.freq,
            beam.speed,
            beam.segments);
        g_triapiGL3.End();
        break;

    case TE_BEAMCYLINDER:
        g_triapiGL3.Begin(TRI_QUAD_STRIP);
        DrawCylinder(
            beam.source,
            beam.delta,
            beam.width,
            beam.amplitude,
            beam.freq,
            beam.speed,
            beam.segments);
        g_triapiGL3.End();
        break;

    case TE_BEAMFOLLOW:
        g_triapiGL3.Begin(TRI_QUADS);
        DrawBeamFollow(beam);
        g_triapiGL3.End();
        break;

    case TE_BEAMRING:
        g_triapiGL3.Begin(TRI_QUAD_STRIP);
        DrawRing(
            beam.source,
            beam.delta,
            beam.width,
            beam.amplitude,
            beam.freq,
            beam.speed,
            beam.segments);
        g_triapiGL3.End();
        break;
    }
}

static void DrawEntity(cl_entity_t *entity, float frametime)
{
    float amplitude = (float)entity->curstate.body * 0.01f;
    float brightness = (float)entityUpdateRenderAmt(entity, g_state.viewOrigin, g_state.viewForward) / 255.0f;
    float speed = entity->curstate.animtime;

    BEAM beam{};
    beamSetup(beam,
        entity->origin,
        entity->curstate.angles,
        entity->curstate.movetype,
        0.0f,
        entity->curstate.scale,
        amplitude,
        brightness,
        speed);

    beam.frame = (float)(int)entity->curstate.frame;
    beam.r = entity->curstate.rendercolor.r / 255.0f;
    beam.g = entity->curstate.rendercolor.g / 255.0f;
    beam.b = entity->curstate.rendercolor.b / 255.0f;

    // FIXME: no constant for the mask?
    int type = entity->curstate.rendermode & 0xF;

    if (type == BEAM_ENTPOINT)
    {
        beam.type = TE_BEAMPOINTS;
        beam.flags = FBEAM_ENDENTITY;
        beam.startEntity = 0;
        beam.endEntity = entity->curstate.skin;
    }
    else if (type == BEAM_ENTS)
    {
        beam.type = TE_BEAMPOINTS;
        beam.flags = FBEAM_STARTENTITY | FBEAM_ENDENTITY;
        beam.startEntity = entity->curstate.sequence;
        beam.endEntity = entity->curstate.skin;
    }

    int flags = entity->curstate.rendermode;

    if (flags & BEAM_FSINE)
    {
        beam.flags |= FBEAM_SINENOISE;
    }

    if (flags & BEAM_FSOLID)
    {
        beam.flags |= FBEAM_SOLID;
    }

    if (flags & BEAM_FSHADEIN)
    {
        beam.flags |= FBEAM_SHADEIN;
    }

    if (flags & BEAM_FSHADEOUT)
    {
        beam.flags |= FBEAM_SHADEOUT;
    }

    if (beam.modelIndex >= 0)
    {
        DrawBeam(beam, frametime);
    }
}

// got confused by valve's shitty code so this was rewritten, in practice should work the same
static void FreeDeadBeams(float clientTime)
{
    BEAM *prev = nullptr;
    BEAM *current = s_activeBeams;

    while (current)
    {
        if (current->die < clientTime && !(current->flags & FBEAM_FOREVER))
        {
            BEAM *next = current->next;

            if (prev)
            {
                prev->next = next;
            }
            else
            {
                s_activeBeams = next;
            }

            current->next = s_freeBeams;
            s_freeBeams = current;

            current = next;
        }
        else
        {
            prev = current;
            current = current->next;
        }
    }
}

void beamDraw()
{
    int entityCount;
    cl_entity_t **entities = entityGetBeams(entityCount);
    if (!entityCount && !s_activeBeams)
    {
        return;
    }

    // FIXME: won't work with older builds (hudGetClientOldTime doesn't exist)
    float clientTime = g_engfuncs.GetClientTime();
    float frametime = clientTime - g_engfuncs.hudGetClientOldTime();

    triapiBegin();
    g_triapiGL3.CullFace(TRI_NONE);

    FreeDeadBeams(clientTime);

    for (BEAM *beam = s_activeBeams; beam; beam = beam->next)
    {
        if (beam->modelIndex < 0)
        {
            // FIXME: does this actually happen? weird
            beam->die = clientTime;
            continue;
        }

        DrawBeam(*beam, frametime);
    }

    for (int i = 0; i < entityCount; i++)
    {
        DrawEntity(entities[i], frametime);
    }

    /* currently immediateDrawEnd resets state */
    triapiEnd();
}

}
