#pragma once

namespace Render
{

void detailInit();

// called after map load
void detailInvalidate();

bool detailIsActive();

void detailSetTextureAndScale(int textureIndex, GLint scaleLocation);
void detailClearTexture();

}
