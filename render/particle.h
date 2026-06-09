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

// for beams
void particleFreeDead(particle_t **head);

}

#endif
