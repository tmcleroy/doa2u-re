#ifndef DOAXBV_RECOMP_D3D_PRESENTER_D3D11_BACKEND_H
#define DOAXBV_RECOMP_D3D_PRESENTER_D3D11_BACKEND_H

#include "d3d_presenter.h"

RecompD3dPresenterError d3d11_backend_create(
    const RecompD3dPresenterConfig *config, RecompD3dPresenter **presenter);
RecompD3dPresenterError d3d11_backend_submit(
    RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command);
/* Counts native raster coverage without shading or target writes. Errors leave
   covered true; an open guest visibility query cannot include this probe. */
RecompD3dPresenterError d3d11_backend_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered);
RecompD3dPresenterError d3d11_backend_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size);
RecompD3dPresenterError d3d11_backend_destroy(RecompD3dPresenter **presenter);
void d3d11_backend_set_immediate_present(bool enabled);
void d3d11_backend_report_draw_textures();

enum D3d11VisibilityPoll {
    D3D11_VISIBILITY_IDLE,
    D3D11_VISIBILITY_PENDING,
    D3D11_VISIBILITY_READY,
    D3D11_VISIBILITY_FAILED,
};
/* Resolves the oldest ended visibility query without waiting; READY fills
   its index and count. Queries resolve in submission order. */
D3d11VisibilityPoll d3d11_backend_poll_visibility(
    RecompD3dPresenter *presenter, uint32_t *index, uint32_t *pixels);

#endif
