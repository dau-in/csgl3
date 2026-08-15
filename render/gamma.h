#ifndef GAMMA_H
#define GAMMA_H

namespace Render
{

extern byte g_gammaTextureTable[256];
extern byte g_gammaLinearTable[256];
extern float g_gamma; // bruh

void gammaInit();
void gammaUpdate();

}

#endif // GAMMA_H
