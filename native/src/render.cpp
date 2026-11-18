#include "render.hpp"

#include <utility>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include "globals.hpp"
#include "src/helpers/time/Time.hpp"
#include "types.hpp"

using namespace Render;
// Note: box is relative to (0, 0), not monitor
void render_window_at_box(PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, CBox box) {
    if (!window || !monitor)
        return;

    box.x -= monitor->m_position.x;
    box.y -= monitor->m_position.y;

    const float scale = box.w / window->sizeAnimation()->value().x;
    const Vector2D transform =
        (monitor->m_position - window->positionAnimation()->value() + box.pos() / scale)
        * monitor->m_scale;

    SRenderModifData data {};
    data.modifs.push_back({SRenderModifData::eRenderModifType::RMOD_TYPE_TRANSLATE, transform});
    data.modifs.push_back({SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE, scale});
    g_pHyprRenderer->m_renderPass.add(
        makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData {data})
    );

    g_pHyprRenderer->damageWindow(window);
    ((render_window_t)render_window)(
        g_pHyprRenderer.get(),
        window,
        monitor,
        time,
        true,
        RENDER_PASS_MAIN,
        false,
        true
    );

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(
        CRendererHintsPassElement::SData {SRenderModifData {}}
    ));
}

void render_workspace_at_box(
    PHLMONITOR monitor,
    PHLWORKSPACE workspace,
    const Time::steady_tp& time,
    CBox box
) {
    if (!monitor)
        return;

    // renderWorkspace derives scale = box.w / pixelSize and translate = box.pos, and
    // applies the translate BEFORE the scale, so pre-divide the position by the scale.
    // Geometry is in pixel (untransformed) space, so swap w/h for rotated monitors.
    const float ws_scale = box.w / monitor->m_transformedSize.x;
    CBox render_box = {box.pos() / ws_scale, box.size()};
    if (monitor->m_transform % 2 == 1)
        std::swap(render_box.w, render_box.h);

    // Do not swap the monitor's active workspace here. Focus dispatchers and
    // click-to-focus read m_activeWorkspace, and a transient swap can make
    // SUPER+N resolve the wrong page. Instead hide every other visible
    // workspace for the duration of this render: Hyprland's shouldRenderWindow()
    // treats any visible workspace's windows as renderable regardless of the
    // workspace passed to renderWorkspace().
    std::vector<std::pair<PHLWORKSPACE, bool>> hidden_workspaces;
    for (const auto& other : State::workspaceState()->workspacesCopy()) {
        if (other == nullptr || other == workspace || other->m_isSpecialWorkspace
            || other->monitorID() != monitor->m_id || !other->m_visible)
            continue;
        hidden_workspaces.emplace_back(other, true);
        other->m_visible = false;
    }

    const bool saved_visible = workspace != nullptr && workspace->m_visible;
    const bool saved_force = workspace != nullptr && workspace->m_forceRendering;
    const float saved_alpha_value = workspace != nullptr ? workspace->m_alpha->value() : 0.f;
    const float saved_alpha_goal = workspace != nullptr ? workspace->m_alpha->goal() : 0.f;
    const Vector2D saved_offset_value =
        workspace != nullptr ? workspace->m_renderOffset->value() : Vector2D {};
    const Vector2D saved_offset_goal =
        workspace != nullptr ? workspace->m_renderOffset->goal() : Vector2D {};

    if (workspace != nullptr) {
        workspace->m_visible = true;
        workspace->m_forceRendering = true;
        workspace->m_alpha->setValueAndWarp(1.f);
        *workspace->m_alpha = 1.f;
        workspace->m_renderOffset->setValueAndWarp(Vector2D {});
        *workspace->m_renderOffset = Vector2D {};
    }

    ((render_workspace_t)(render_workspace_hook->m_original))(
        g_pHyprRenderer.get(), monitor, workspace, time, render_box
    );

    if (workspace != nullptr) {
        workspace->m_visible = saved_visible;
        workspace->m_forceRendering = saved_force;
        workspace->m_alpha->setValueAndWarp(saved_alpha_value);
        *workspace->m_alpha = saved_alpha_goal;
        workspace->m_renderOffset->setValueAndWarp(saved_offset_value);
        *workspace->m_renderOffset = saved_offset_goal;
    }

    for (auto& [other, was_visible] : hidden_workspaces) {
        if (other != nullptr)
            other->m_visible = was_visible;
    }
}
