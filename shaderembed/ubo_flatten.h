// code for flattening ubos to plain uniforms and converting the shader to glsl 1.20,
// and emitting reflection structures used at runtime for mapping ubo data to uniform locations
#ifndef UBO_FLATTEN_H
#define UBO_FLATTEN_H

#include <iostream>
#include <string>

bool process_reflection(const char *path, const std::string &pretty, const std::string &raw, std::string &flattened, int &uboCount);
void write_reflection(std::ostream &out);
std::string lower_to_120(bool is_vertex, const std::string &src);

#endif // UBO_FLATTEN_H
