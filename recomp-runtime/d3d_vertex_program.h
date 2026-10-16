#ifndef DOAXBV_D3D_VERTEX_PROGRAM_H
#define DOAXBV_D3D_VERTEX_PROGRAM_H

#include <cstdint>
#include <string>

/* Translate straight-line Xbox vertex instructions to native HLSL.
   Unsupported instructions fail closed; no guest execution occurs here.
   fvf names the declared inputs: v0 position, v2 normal (0x10), v3 color
   (0x40) and v9 UV (0x100); reading an undeclared input fails closed. */
bool recomp_d3d_vertex_program_source(
    const uint32_t (*tokens)[4], uint32_t count, uint32_t fvf, std::string &body);

#endif
