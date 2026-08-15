// this file is included by both c++ code and shaders

// FIXME: bullshit way of doing dlights!!!
#define MAX_SHADER_LIGHTS 4

// already defined, will cause a warning if the definition doesn't match
#define MAX_LIGHTSTYLES 64

// already defined, will cause a warning if the definition doesn't match
#define MAXSTUDIOBONES 128

// used when ubos are not available
// lower than MAXSTUDIOBONES so we can fit into 256 constant registers
#define MAX_BONES_SM3 72

// there's probably an engine constant for this...
#define STUDIO_MAX_ELIGHTS 3

// WARNING: our naive shader processor in shaderembed does not understand
// ifdefs, so it's simply going to pick the last #define, which happens to
// be correct (uniform reflection for glsl 1.20)
#if !defined(__cplusplus)
#if (__VERSION__ >= 140)
#define MAX_SHADER_BONES MAXSTUDIOBONES
#else
#define MAX_SHADER_BONES MAX_BONES_SM3
#endif
#endif
