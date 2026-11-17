#pragma once

#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/View.hpp>
#include <hyprutils/signal/Listener.hpp>

#include <vector>

#include "manager.hpp"

inline constexpr auto LOG = Hyprutils::CLI::LOG_DEBUG;
inline constexpr auto ERR = Hyprutils::CLI::LOG_ERR;

inline HANDLE PHANDLE = nullptr;

inline CFunctionHook* render_workspace_hook = nullptr;
inline CFunctionHook* render_texture_hook = nullptr;
inline CFunctionHook* render_border_hook = nullptr;
inline CFunctionHook* render_border2_hook = nullptr;
inline CFunctionHook* blur_optimizations_hook = nullptr;
inline CFunctionHook* should_render_window_hook = nullptr;
inline CFunctionHook* is_solitary_blocked_hook = nullptr;
typedef uint32_t (*origIsSolitaryBlocked)(void*, bool);
inline void* render_window = nullptr;

inline std::unique_ptr<HTManager> ht_manager;

// Event-bus listeners are not removed automatically when the plugin unloads.
// Own them here and clear them in PLUGIN_EXIT so a reload cannot leave a
// callback pointing into the old shared object.
inline std::vector<Hyprutils::Signal::CHyprSignalListener> g_eventListeners;

template<typename... Args>
inline void fail_exit(const std::format_string<Args...>& fmt, Args... args) {
    std::string err_string =
        "[Tilesgrid] " + std::vformat(fmt.get(), std::make_format_args(args...));

    HyprlandAPI::addNotification(PHANDLE, err_string, CHyprColor {1.0, 0.2, 0.2, 1.0}, 5000);
    throw std::runtime_error(err_string);
}
