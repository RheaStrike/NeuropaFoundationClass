#include "framework.h"
#include "Renderer.h"
#include "GdiRenderer.h"

namespace nfc {

std::unique_ptr<Renderer> Renderer::Create(RenderBackend backend) {
    switch (backend) {
    case RENDER_GDIPLUS:
        return std::unique_ptr<Renderer>(new GdiRenderer());
    case RENDER_D2D: // TODO: D2DRenderer (심화). 지금은 없음.
    case RENDER_D3D: // TODO: D3DRenderer (심화). 지금은 없음.
    default:
        return std::unique_ptr<Renderer>();
    }
}

} // namespace nfc
