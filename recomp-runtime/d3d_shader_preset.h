#ifndef DOA2U_RECOMP_D3D_SHADER_PRESET_H
#define DOA2U_RECOMP_D3D_SHADER_PRESET_H

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

/* The RetroArch slang preset (a .slangp: CRT, scanlines and the rest) the window's picture is drawn through by
   the SDL_GPU presenter (d3d_shader_chain.h); NULL or "" for none. Any thread: the presenter takes it at its next
   present. RECOMP_SHADER_PRESET names one from the start, and wins over this. */
void recomp_d3d_set_shader_preset(const char *path);

/* Whether the preset asked for is drawing: 1 yes, 0 none asked for (or not yet tried), -1 it could not be made
   (the picture is then shown as it is). */
int recomp_d3d_shader_state(void);

#ifdef __cplusplus
}

/* (the presenter's) The preset asked for, and a number that changes with each ask. */
std::string recomp_d3d_shader_preset(unsigned &generation);
void recomp_d3d_shader_report(int state);
#endif

#endif
