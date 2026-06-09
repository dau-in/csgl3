#ifndef R_BEAM_H
#define R_BEAM_H

namespace Render
{

void beamInit();
void beamClear();

BEAM *beamAllocate();

void beamSetup(
    BEAM &beam,
    const Vector3 &start,
    const Vector3 &end,
    int modelIndex,
    float life,
    float width,
    float amplitude,
    float brightness,
    float speed);

cl_entity_t *beamGetBeamEntity(int index);

void beamDraw();

}

#endif // R_BEAM_H
