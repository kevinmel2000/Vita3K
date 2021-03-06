// Vita3K emulator project
// Copyright (C) 2018 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <host/functions.h>

#include <host/app.h>
#include <host/import_fn.h>
#include <host/state.h>
#include <host/version.h>

#include <audio/functions.h>
#include <cpu/functions.h>
#include <glutil/gl.h>
#include <io/functions.h>
#include <kernel/functions.h>
#include <kernel/thread/thread_state.h>
#include <nids/functions.h>
#include <rtc/rtc.h>
#include <util/lock_and_find.h>
#include <util/log.h>

#include <SDL_events.h>
#include <SDL_filesystem.h>
#include <SDL_video.h>

#include <glbinding-aux/types_to_string.h>
#include <glbinding/Binding.h>
#include <microprofile.h>

#include <cassert>
#include <iomanip>
#include <iostream>
#include <sstream>

// clang-format off
#include <imgui.h>
#include <gui/imgui_impl_sdl_gl3.h>
// clang-format on

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dirent.h>
#include <util/string_convert.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace glbinding;

static const bool LOG_IMPORT_CALLS = false;

#define NID(name, nid) extern const ImportFn import_##name;
#include <nids/nids.h>
#undef NID

static ImportFn resolve_import(uint32_t nid) {
    switch (nid) {
#define NID(name, nid) \
    case nid:          \
        return import_##name;
#include <nids/nids.h>
#undef NID
    }

    return ImportFn();
}

void before_callback(const glbinding::FunctionCall &fn) {
#if MICROPROFILE_ENABLED
    const MicroProfileToken token = MicroProfileGetToken("OpenGL", fn.function->name(), MP_CYAN, MicroProfileTokenTypeCpu);
    MICROPROFILE_ENTER_TOKEN(token);
#endif // MICROPROFILE_ENABLED
}

void after_callback(const glbinding::FunctionCall &fn) {
    MICROPROFILE_LEAVE();
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
        std::stringstream gl_error;
        gl_error << error;
        LOG_ERROR("OpenGL: {} set error {}.", fn.function->name(), gl_error.str());
        assert(false);
    }
}

bool init(HostState &state) {
    const std::unique_ptr<char, void (&)(void *)> base_path(SDL_GetBasePath(), SDL_free);
    const std::unique_ptr<char, void (&)(void *)> pref_path(SDL_GetPrefPath(org_name, app_name), SDL_free);

    const ResumeAudioThread resume_thread = [&state](SceUID thread_id) {
        const ThreadStatePtr thread = lock_and_find(thread_id, state.kernel.threads, state.kernel.mutex);
        const std::lock_guard<std::mutex> lock(thread->mutex);
        if (thread->to_do == ThreadToDo::wait) {
            thread->to_do = ThreadToDo::run;
        }
        thread->something_to_do.notify_all();
    };

    state.base_path = base_path.get();
    state.pref_path = pref_path.get();
    state.display.set_dims(DEFAULT_RES_WIDTH, DEFAULT_RES_HEIGHT, WINDOW_BORDER_WIDTH, WINDOW_BORDER_HEIGHT);
    state.window = WindowPtr(SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, state.display.window_size.width, state.display.window_size.height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE), SDL_DestroyWindow);
    if (!state.window || !init(state.mem) || !init(state.audio, resume_thread) || !init(state.io, state.pref_path.c_str())) {
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    state.glcontext = GLContextPtr(SDL_GL_CreateContext(state.window.get()), SDL_GL_DeleteContext);
    if (!state.glcontext) {
        LOG_ERROR("Could not create OpenGL context.");
        return false;
    }

    // Try adaptive vsync first, falling back to regular vsync.
    if (SDL_GL_SetSwapInterval(-1) < 0) {
        SDL_GL_SetSwapInterval(1);
    }
    LOG_INFO("Swap interval = {}", SDL_GL_GetSwapInterval());

    const glbinding::GetProcAddress get_proc_address = [](const char *name) {
        return reinterpret_cast<ProcAddress>(SDL_GL_GetProcAddress(name));
    };
    Binding::initialize(get_proc_address, false);
    Binding::setCallbackMaskExcept(CallbackMask::Before | CallbackMask::After, { "glGetError" });
#if MICROPROFILE_ENABLED != 0
    Binding::setBeforeCallback(before_callback);
#endif // MICROPROFILE_ENABLED
    Binding::setAfterCallback(after_callback);

    state.kernel.base_tick = { rtc_base_ticks() };

    std::string dir_path = state.pref_path + "ux0/app";
#ifdef WIN32
    _WDIR *d = _wopendir((utf_to_wide(dir_path)).c_str());
    _wdirent *dp;
#else
    DIR *d = opendir(dir_path.c_str());
    dirent *dp;
#endif
    do {
        std::string d_name_utf8;
#ifdef WIN32
        if ((dp = _wreaddir(d)) != NULL) {
            d_name_utf8 = wide_to_utf(dp->d_name);
#else
        if ((dp = readdir(d)) != NULL) {
            d_name_utf8 = dp->d_name;
#endif
            if ((strcmp(d_name_utf8.c_str(), ".")) && (strcmp(d_name_utf8.c_str(), ".."))) {
                Buffer params;
                state.io.title_id = d_name_utf8;
                if (read_file_from_disk(params, "sce_sys/param.sfo", state)) {
                    SfoFile sfo_handle;
                    load_sfo(sfo_handle, params);
                    find_data(state.game_title, sfo_handle, "TITLE");
                    state.gui.game_selector.title_ids.push_back(std::string(state.io.title_id));
                    state.gui.game_selector.titles.push_back(std::string(state.game_title));
                }
            }
        }
    } while (dp);

#ifdef WIN32
    _wclosedir(d);
#else
    closedir(d);
#endif

    return true;
}

bool handle_events(HostState &host) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSdlGL3_ProcessEvent(&event);
        if (event.type == SDL_QUIT) {
            stop_all_threads(host.kernel);
            host.gxm.display_queue.abort();
            host.display.abort.exchange(true);
            host.display.condvar.notify_all();
            return false;
        }

        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_g) {
            auto &display = host.display;

            // toggle gui state
            bool old_imgui_render = display.imgui_render.load();
            while (!display.imgui_render.compare_exchange_weak(old_imgui_render, !old_imgui_render)) {
            }

            if (old_imgui_render) {
                display.set_dims(DEFAULT_RES_WIDTH, DEFAULT_RES_HEIGHT, 0, 0);
                SDL_SetWindowResizable(host.window.get(), SDL_FALSE);
            } else {
                display.set_dims(DEFAULT_RES_WIDTH, DEFAULT_RES_HEIGHT, WINDOW_BORDER_WIDTH, WINDOW_BORDER_HEIGHT);
                SDL_SetWindowResizable(host.window.get(), SDL_TRUE);
            }

            SDL_SetWindowSize(host.window.get(), display.window_size.width, display.window_size.height);
        }
    }

    return true;
}

/**
 * \brief Resolves a function imported from a loaded module.
 * \param kernel Kernel state struct
 * \param nid NID to resolve
 * \return Resolved address, 0 if not found
 */
Address resolve_export(KernelState &kernel, uint32_t nid) {
    const ExportNids::iterator export_address = kernel.export_nids.find(nid);
    if (export_address == kernel.export_nids.end()) {
        return 0;
    }

    return export_address->second;
}

void call_import(HostState &host, CPUState &cpu, uint32_t nid, SceUID thread_id) {
    Address export_pc = resolve_export(host.kernel, nid);

    if (!export_pc) {
        // HLE - call our C++ function

        if (LOG_IMPORT_CALLS) {
            const char *const name = import_name(nid);
            LOG_TRACE("THREAD_ID {} NID {} ({}) called", thread_id, log_hex(nid), name);
        }
        const ImportFn fn = resolve_import(nid);
        if (fn) {
            fn(host, cpu, thread_id);
        }
    } else {
        // LLE - directly run ARM code imported from some loaded module

        if (LOG_IMPORT_CALLS) {
            const char *const name = import_name(nid);
            LOG_TRACE("THREAD_ID {} EXPORTED NID {} at {} ({})) called", thread_id, log_hex(nid), log_hex(export_pc), name);
        }
        const ThreadStatePtr thread = lock_and_find(thread_id, host.kernel.threads, host.kernel.mutex);
        const std::lock_guard<std::mutex> lock(thread->mutex);
        write_pc(*thread->cpu, export_pc);
    }
}
