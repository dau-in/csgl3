#ifndef PARTICLE_H
#define PARTICLE_H

namespace Render
{

void particleInit();
void particleClear();

// FIXME: decouple update from rendering...
void particleDraw();

// effects need these
particle_t *particleAllocate();
particle_t *particleAllocateTracer();

// for beams, which own their particles instead of putting them on the active list
particle_t *particleAllocateInto(particle_t **head);
void particleFreeDead(particle_t **head);
void particleFreeList(particle_t **head);

}

#endif
