#ifndef LIGHTGAMMA_H
#define LIGHTGAMMA_H

namespace Render
{

// lightgammatable approximation... the real function was too expensive on the X1600,
// no vectorized transcendentals there so the 2x rgb pow() really fucked it up
std::string LightGammaGLSL(float gamma, float lightgamma, float brightness);

}

#endif
