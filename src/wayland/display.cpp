#include "wayland/display.hpp"

#include "core/clock.hpp"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.hpp"
#include "fractional-scale-v1.h"
#include "linux-dmabuf-v1.h"
#include "viewporter.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"
// The generated header names a parameter "namespace", a C++ keyword.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1.h"
#undef namespace

namespace glint::wl {

namespace {

// Versions glint is written against; higher ones from the compositor are capped here, so
// events from newer versions never arrive.
constexpr std::uint32_t compositor_version = 4;
constexpr std::uint32_t output_version = 4;  // name and description
constexpr std::uint32_t seat_version = 5;
constexpr std::uint32_t layer_shell_version = 4;
constexpr std::uint32_t toplevel_version = 3;
constexpr std::uint32_t dmabuf_version = 4;  // default feedback

// zwlr_foreign_toplevel_handle_v1 state values.
constexpr std::uint32_t toplevel_state_minimized = 1;
constexpr std::uint32_t toplevel_state_activated = 2;
constexpr std::uint32_t toplevel_state_fullscreen = 3;

template <typename T>
T* bind(wl_registry* registry, std::uint32_t name, const wl_interface* interface,
        std::uint32_t offered, std::uint32_t wanted) {
    return static_cast<T*>(wl_registry_bind(registry, name, interface, std::min(offered, wanted)));
}

}  // namespace

// ---- Output ------------------------------------------------------------------------------

struct OutputListeners {
    static void geometry(void* data, wl_output*, std::int32_t x, std::int32_t y, std::int32_t,
                         std::int32_t, std::int32_t, const char* make, const char* model,
                         std::int32_t) {
        auto* self = static_cast<Output*>(data);
        self->x_ = x;
        self->y_ = y;
        self->make_ = make;
        self->model_ = model;
    }
    static void mode(void* data, wl_output*, std::uint32_t flags, std::int32_t width,
                     std::int32_t height, std::int32_t refresh) {
        if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) return;
        auto* self = static_cast<Output*>(data);
        if (width != self->mode_width_ || height != self->mode_height_) self->preferred120_ = 0;
        self->mode_width_ = width;
        self->mode_height_ = height;
        self->refresh_mhz_ = refresh;
    }
    static void done(void* data, wl_output*) {
        auto* self = static_cast<Output*>(data);
        if (self->name_.empty()) return;
        if (self->surface_ == nullptr) {
            self->create_surface();
        } else if (self->update_scale() && self->configured_) {
            self->display_.listener().output_resized(*self);
        }
    }
    static void scale(void* data, wl_output*, std::int32_t factor) {
        static_cast<Output*>(data)->integer_scale_ = std::max(factor, 1);
    }
    static void name(void* data, wl_output*, const char* name) {
        static_cast<Output*>(data)->name_ = name;
    }
    static void description(void* data, wl_output*, const char* description) {
        static_cast<Output*>(data)->description_ = description;
    }

    static constexpr wl_output_listener output = {
        .geometry = geometry,
        .mode = mode,
        .done = done,
        .scale = scale,
        .name = name,
        .description = description,
    };

    static void configure(void* data, zwlr_layer_surface_v1* layer, std::uint32_t serial,
                          std::uint32_t width, std::uint32_t height) {
        auto* self = static_cast<Output*>(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        bool first = !self->configured_;
        bool changed = width != self->logical_width_ || height != self->logical_height_;
        if (changed) self->preferred120_ = 0;
        self->logical_width_ = width;
        self->logical_height_ = height;
        self->configured_ = true;
        changed = self->update_scale() || changed;
        if (first) {
            self->announced_ = true;
            self->display_.recompute_covered();
            self->display_.listener().output_added(*self);
        } else if (changed) {
            self->display_.listener().output_resized(*self);
        }
    }
    static void closed(void* data, zwlr_layer_surface_v1*) {
        // The compositor took the surface away (output going, or a policy). Start over with
        // a fresh one; if the output is going, global_remove follows and ends it.
        auto* self = static_cast<Output*>(data);
        log::info("{}: layer surface closed by the compositor", self->name_);
        self->destroy_surface();
        self->create_surface();
    }

    static constexpr zwlr_layer_surface_v1_listener layer = {
        .configure = configure,
        .closed = closed,
    };

    static void preferred_scale(void* data, wp_fractional_scale_v1*, std::uint32_t scale) {
        auto* self = static_cast<Output*>(data);
        self->preferred120_ = scale;
        if (self->update_scale() && self->configured_) {
            self->display_.listener().output_resized(*self);
        }
    }

    static constexpr wp_fractional_scale_v1_listener fractional = {
        .preferred_scale = preferred_scale,
    };

    static void frame_done(void* data, wl_callback* callback, std::uint32_t time_ms) {
        auto* self = static_cast<Output*>(data);
        wl_callback_destroy(callback);
        if (self->frame_ == callback) self->frame_ = nullptr;
        self->frame_done_ns_ = clock::now_ns();
        self->display_.listener().output_frame(*self, time_ms);
    }

    static constexpr wl_callback_listener frame = {
        .done = frame_done,
    };
};

Output::Output(Display& display, std::uint32_t global, wl_output* output)
    : display_(display), global_(global), output_(output) {
    wl_output_add_listener(output_, &OutputListeners::output, this);
}

Output::~Output() {
    destroy_surface();
    if (wl_output_get_version(output_) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
        wl_output_release(output_);
    } else {
        wl_output_destroy(output_);
    }
}

bool Output::update_scale() {
    // The compositor's preferred scale wins once it has sent one for the current geometry.
    // Hyprland sends it only after the surface shows on the output, that is after the first
    // buffer, and not again while the surface is unmapped. Until then: the surface covers the
    // whole output, so the mode size over the logical size is the scale (either way round, for
    // rotated outputs), and the integer wl_output scale if that does not add up.
    std::uint32_t scale120 = static_cast<std::uint32_t>(integer_scale_) * 120;
    if (preferred120_ != 0) {
        scale120 = preferred120_;
    } else if (logical_width_ > 0 && logical_height_ > 0 && mode_width_ > 0 && mode_height_ > 0) {
        auto ratio = [](std::int32_t mode, std::uint32_t logical) {
            return static_cast<std::int64_t>(std::lround(120.0 * mode / logical));
        };
        std::int64_t a = ratio(mode_width_, logical_width_);
        std::int64_t b = ratio(mode_height_, logical_height_);
        std::int64_t c = ratio(mode_height_, logical_width_);
        std::int64_t d = ratio(mode_width_, logical_height_);
        if (std::abs(a - b) <= 1) {
            scale120 = static_cast<std::uint32_t>(a);
        } else if (std::abs(c - d) <= 1) {
            scale120 = static_cast<std::uint32_t>(c);
        }
    }
    scale120 = std::max<std::uint32_t>(scale120, 30);
    if (scale120 == scale120_) return false;
    scale120_ = scale120;
    return true;
}

std::uint32_t Output::pixel_width() const {
    return static_cast<std::uint32_t>(std::lround(logical_width_ * scale120_ / 120.0));
}

std::uint32_t Output::pixel_height() const {
    return static_cast<std::uint32_t>(std::lround(logical_height_ * scale120_ / 120.0));
}

void Output::create_surface() {
    surface_ = wl_compositor_create_surface(display_.compositor());
    layer_ = zwlr_layer_shell_v1_get_layer_surface(display_.layer_shell(), surface_, output_,
                                                   ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "glint");
    zwlr_layer_surface_v1_add_listener(layer_, &OutputListeners::layer, this);
    zwlr_layer_surface_v1_set_anchor(
        layer_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer_, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    if (display_.viewporter() != nullptr) {
        viewport_ = wp_viewporter_get_viewport(display_.viewporter(), surface_);
    }
    if (display_.fractional_manager() != nullptr) {
        fractional_ = wp_fractional_scale_manager_v1_get_fractional_scale(
            display_.fractional_manager(), surface_);
        wp_fractional_scale_v1_add_listener(fractional_, &OutputListeners::fractional, this);
    }
    preferred120_ = 0;
    update_scale();
    bool input = pointer_input_;
    pointer_input_ = !input;
    set_pointer_input(input);
    wl_surface_commit(surface_);
}

void Output::destroy_surface() {
    if (announced_) {
        announced_ = false;
        display_.listener().output_removed(*this);
    }
    if (frame_ != nullptr) wl_callback_destroy(frame_);
    if (fractional_ != nullptr) wp_fractional_scale_v1_destroy(fractional_);
    if (viewport_ != nullptr) wp_viewport_destroy(viewport_);
    if (layer_ != nullptr) zwlr_layer_surface_v1_destroy(layer_);
    if (surface_ != nullptr) wl_surface_destroy(surface_);
    frame_ = nullptr;
    fractional_ = nullptr;
    viewport_ = nullptr;
    layer_ = nullptr;
    surface_ = nullptr;
    configured_ = false;
    logical_width_ = logical_height_ = 0;
}

void Output::set_pointer_input(bool on) {
    if (on == pointer_input_ || surface_ == nullptr) return;
    pointer_input_ = on;
    if (on) {
        wl_surface_set_input_region(surface_, nullptr);  // the whole surface
    } else {
        wl_region* empty = wl_compositor_create_region(display_.compositor());
        wl_surface_set_input_region(surface_, empty);
        wl_region_destroy(empty);
    }
}

void Output::request_frame() {
    if (frame_ != nullptr || surface_ == nullptr) return;
    frame_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_, &OutputListeners::frame, this);
}

void Output::apply_surface_state() {
    if (surface_ == nullptr || !configured_) return;
    if (viewport_ != nullptr) {
        wp_viewport_set_destination(viewport_, static_cast<std::int32_t>(logical_width_),
                                    static_cast<std::int32_t>(logical_height_));
    } else {
        wl_surface_set_buffer_scale(surface_, static_cast<std::int32_t>(scale120_ / 120));
    }
    wl_region* region = wl_compositor_create_region(display_.compositor());
    wl_region_add(region, 0, 0, static_cast<std::int32_t>(logical_width_),
                  static_cast<std::int32_t>(logical_height_));
    wl_surface_set_opaque_region(surface_, region);
    wl_region_destroy(region);
}

// ---- Display -----------------------------------------------------------------------------

struct DisplayListeners {
    static void global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                       std::uint32_t version) {
        auto* self = static_cast<Display*>(data);
        std::string_view iface = interface;
        if (iface == wl_compositor_interface.name) {
            self->compositor_ = bind<wl_compositor>(registry, name, &wl_compositor_interface,
                                                    version, compositor_version);
        } else if (iface == zwlr_layer_shell_v1_interface.name) {
            self->layer_shell_ = bind<zwlr_layer_shell_v1>(
                registry, name, &zwlr_layer_shell_v1_interface, version, layer_shell_version);
        } else if (iface == wp_viewporter_interface.name) {
            self->viewporter_ =
                bind<wp_viewporter>(registry, name, &wp_viewporter_interface, version, 1);
        } else if (iface == wp_fractional_scale_manager_v1_interface.name) {
            self->fractional_manager_ = bind<wp_fractional_scale_manager_v1>(
                registry, name, &wp_fractional_scale_manager_v1_interface, version, 1);
        } else if (iface == zwlr_foreign_toplevel_manager_v1_interface.name) {
            // Bound only while something on screen animates, see track_toplevels().
            self->toplevel_global_ = name;
            self->toplevel_offered_ = version;
        } else if (iface == zwp_linux_dmabuf_v1_interface.name && version >= dmabuf_version) {
            self->dmabuf_ = bind<zwp_linux_dmabuf_v1>(
                registry, name, &zwp_linux_dmabuf_v1_interface, version, dmabuf_version);
            self->feedback_ = zwp_linux_dmabuf_v1_get_default_feedback(self->dmabuf_);
            zwp_linux_dmabuf_feedback_v1_add_listener(self->feedback_, &feedback, self);
        } else if (iface == wl_seat_interface.name && self->seat_ == nullptr) {
            self->seat_ = bind<wl_seat>(registry, name, &wl_seat_interface, version, seat_version);
            self->seat_global_ = name;
            wl_seat_add_listener(self->seat_, &seat, self);
        } else if (iface == wl_output_interface.name) {
            if (version < output_version) {
                log::warn("wl_output version {} has no names; output ignored", version);
                return;
            }
            auto* output =
                bind<wl_output>(registry, name, &wl_output_interface, version, output_version);
            self->outputs_.push_back(std::make_unique<Output>(*self, name, output));
        }
    }

    static void global_remove(void* data, wl_registry*, std::uint32_t name) {
        auto* self = static_cast<Display*>(data);
        if (name == self->seat_global_ && self->seat_ != nullptr) {
            if (self->pointer_ != nullptr) wl_pointer_release(self->pointer_);
            wl_seat_release(self->seat_);
            self->pointer_ = nullptr;
            self->seat_ = nullptr;
            self->pointer_output_ = nullptr;
            return;
        }
        self->remove_output(name);
    }

    static constexpr wl_registry_listener registry = {
        .global = global,
        .global_remove = global_remove,
    };

    // Foreign toplevels: only fullscreen, minimized and the outputs matter.
    static void toplevel(void* data, zwlr_foreign_toplevel_manager_v1*,
                         zwlr_foreign_toplevel_handle_v1* handle) {
        auto* self = static_cast<Display*>(data);
        auto entry = std::make_unique<Display::Toplevel>();
        entry->display = self;
        entry->handle = handle;
        zwlr_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_handle, entry.get());
        self->toplevels_.push_back(std::move(entry));
    }
    static void finished(void* data, zwlr_foreign_toplevel_manager_v1* manager) {
        auto* self = static_cast<Display*>(data);
        zwlr_foreign_toplevel_manager_v1_destroy(manager);
        self->toplevel_manager_ = nullptr;
    }

    static constexpr zwlr_foreign_toplevel_manager_v1_listener manager = {
        .toplevel = toplevel,
        .finished = finished,
    };

    static void title(void*, zwlr_foreign_toplevel_handle_v1*, const char*) {}
    static void app_id(void*, zwlr_foreign_toplevel_handle_v1*, const char*) {}
    static void output_enter(void* data, zwlr_foreign_toplevel_handle_v1*, wl_output* output) {
        auto* entry = static_cast<Display::Toplevel*>(data);
        if (std::ranges::find(entry->outputs, output) == entry->outputs.end()) {
            entry->outputs.push_back(output);
        }
    }
    static void output_leave(void* data, zwlr_foreign_toplevel_handle_v1*, wl_output* output) {
        auto* entry = static_cast<Display::Toplevel*>(data);
        std::erase(entry->outputs, output);
    }
    static void state(void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* states) {
        auto* entry = static_cast<Display::Toplevel*>(data);
        entry->pending_fullscreen = false;
        entry->pending_minimized = false;
        entry->pending_activated = false;
        auto* begin = static_cast<const std::uint32_t*>(states->data);
        for (std::size_t i = 0; i < states->size / sizeof(std::uint32_t); ++i) {
            if (begin[i] == toplevel_state_fullscreen) entry->pending_fullscreen = true;
            if (begin[i] == toplevel_state_minimized) entry->pending_minimized = true;
            if (begin[i] == toplevel_state_activated) entry->pending_activated = true;
        }
    }
    static void done(void* data, zwlr_foreign_toplevel_handle_v1*);
    static void closed(void* data, zwlr_foreign_toplevel_handle_v1* handle);
    static void parent(void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) {}

    static constexpr zwlr_foreign_toplevel_handle_v1_listener toplevel_handle = {
        .title = title,
        .app_id = app_id,
        .output_enter = output_enter,
        .output_leave = output_leave,
        .state = state,
        .done = done,
        .closed = closed,
        .parent = parent,
    };

    // linux-dmabuf default feedback: only the main device is used, to pick the GPU.
    static void feedback_done(void* data, zwp_linux_dmabuf_feedback_v1* object) {
        auto* self = static_cast<Display*>(data);
        zwp_linux_dmabuf_feedback_v1_destroy(object);
        self->feedback_ = nullptr;
        zwp_linux_dmabuf_v1_destroy(self->dmabuf_);
        self->dmabuf_ = nullptr;
    }
    static void format_table(void*, zwp_linux_dmabuf_feedback_v1*, std::int32_t fd, std::uint32_t) {
        ::close(fd);
    }
    static void main_device(void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* device) {
        auto* self = static_cast<Display*>(data);
        if (device->size != sizeof(dev_t)) return;
        dev_t value;
        std::memcpy(&value, device->data, sizeof(dev_t));
        self->main_device_ = value;
    }
    static void tranche_done(void*, zwp_linux_dmabuf_feedback_v1*) {}
    static void tranche_target_device(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
    static void tranche_formats(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
    static void tranche_flags(void*, zwp_linux_dmabuf_feedback_v1*, std::uint32_t) {}

    static constexpr zwp_linux_dmabuf_feedback_v1_listener feedback = {
        .done = feedback_done,
        .format_table = format_table,
        .main_device = main_device,
        .tranche_done = tranche_done,
        .tranche_target_device = tranche_target_device,
        .tranche_formats = tranche_formats,
        .tranche_flags = tranche_flags,
    };

    // Seat and pointer.
    static void capabilities(void* data, wl_seat* seat_object, std::uint32_t caps) {
        auto* self = static_cast<Display*>(data);
        bool has_pointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
        if (has_pointer && self->pointer_ == nullptr) {
            self->pointer_ = wl_seat_get_pointer(seat_object);
            wl_pointer_add_listener(self->pointer_, &pointer, self);
        } else if (!has_pointer && self->pointer_ != nullptr) {
            wl_pointer_release(self->pointer_);
            self->pointer_ = nullptr;
            self->pointer_output_ = nullptr;
        }
    }
    static void seat_name(void*, wl_seat*, const char*) {}

    static constexpr wl_seat_listener seat = {
        .capabilities = capabilities,
        .name = seat_name,
    };

    static void report(Display* self) {
        if (self->pointer_output_ == nullptr) return;
        double scale = self->pointer_output_->scale();
        self->listener_.pointer(*self->pointer_output_, self->pointer_x_ * scale,
                                self->pointer_y_ * scale, self->buttons_);
    }
    static void enter(void* data, wl_pointer*, std::uint32_t, wl_surface* surface, wl_fixed_t x,
                      wl_fixed_t y) {
        auto* self = static_cast<Display*>(data);
        self->pointer_output_ = self->find_surface(surface);
        self->pointer_x_ = wl_fixed_to_double(x);
        self->pointer_y_ = wl_fixed_to_double(y);
        report(self);
    }
    static void leave(void* data, wl_pointer*, std::uint32_t, wl_surface*) {
        auto* self = static_cast<Display*>(data);
        self->pointer_output_ = nullptr;
        self->buttons_ = 0;
    }
    static void motion(void* data, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
        auto* self = static_cast<Display*>(data);
        self->pointer_x_ = wl_fixed_to_double(x);
        self->pointer_y_ = wl_fixed_to_double(y);
        report(self);
    }
    static void button(void* data, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t code,
                       std::uint32_t state) {
        auto* self = static_cast<Display*>(data);
        std::uint32_t bit = 0;
        if (code == BTN_LEFT) bit = 1;
        if (code == BTN_RIGHT) bit = 2;
        if (code == BTN_MIDDLE) bit = 4;
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            self->buttons_ |= bit;
        } else {
            self->buttons_ &= ~bit;
        }
        report(self);
    }
    static void axis(void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {}
    static void frame(void*, wl_pointer*) {}
    static void axis_source(void*, wl_pointer*, std::uint32_t) {}
    static void axis_stop(void*, wl_pointer*, std::uint32_t, std::uint32_t) {}
    static void axis_discrete(void*, wl_pointer*, std::uint32_t, std::int32_t) {}

    // Bound at version 5, so axis_value120 and later events never arrive.
    static constexpr wl_pointer_listener pointer = {
        .enter = enter,
        .leave = leave,
        .motion = motion,
        .button = button,
        .axis = axis,
        .frame = frame,
        .axis_source = axis_source,
        .axis_stop = axis_stop,
        .axis_discrete = axis_discrete,
        .axis_value120 = nullptr,
        .axis_relative_direction = nullptr,
        .warp = nullptr,
    };
};

void DisplayListeners::done(void* data, zwlr_foreign_toplevel_handle_v1*) {
    auto* entry = static_cast<Display::Toplevel*>(data);
    entry->fullscreen = entry->pending_fullscreen;
    entry->minimized = entry->pending_minimized;
    entry->activated = entry->pending_activated;
    entry->display->recompute_covered();
}

void DisplayListeners::closed(void* data, zwlr_foreign_toplevel_handle_v1* handle) {
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    Display* display = static_cast<Display::Toplevel*>(data)->display;
    std::erase_if(display->toplevels_, [&](const auto& t) { return t.get() == data; });
    display->recompute_covered();
}

Display::Display(Listener& listener) : listener_(listener) {}

Display::~Display() {
    track_toplevels(false);
    outputs_.clear();
    if (pointer_ != nullptr) wl_pointer_release(pointer_);
    if (seat_ != nullptr) wl_seat_release(seat_);
    if (feedback_ != nullptr) zwp_linux_dmabuf_feedback_v1_destroy(feedback_);
    if (dmabuf_ != nullptr) zwp_linux_dmabuf_v1_destroy(dmabuf_);
    track_toplevels(false);
    if (fractional_manager_ != nullptr) wp_fractional_scale_manager_v1_destroy(fractional_manager_);
    if (viewporter_ != nullptr) wp_viewporter_destroy(viewporter_);
    if (layer_shell_ != nullptr) zwlr_layer_shell_v1_destroy(layer_shell_);
    if (compositor_ != nullptr) wl_compositor_destroy(compositor_);
    if (registry_ != nullptr) wl_registry_destroy(registry_);
    if (display_ != nullptr) {
        wl_display_flush(display_);
        wl_display_disconnect(display_);
    }
}

std::string Display::connect() {
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) return "cannot connect to the Wayland display";
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &DisplayListeners::registry, this);
    // First roundtrip: globals. Second: output names, seat capabilities, dmabuf feedback.
    if (wl_display_roundtrip(display_) < 0) return "Wayland roundtrip failed";
    if (compositor_ == nullptr) return "the compositor has no wl_compositor";
    if (layer_shell_ == nullptr) return "the compositor has no zwlr_layer_shell_v1";
    if (wl_display_roundtrip(display_) < 0) return "Wayland roundtrip failed";
    log::info("wayland: viewporter {}, fractional-scale {}, foreign-toplevel {}, seat {}",
              viewporter_ != nullptr ? "yes" : "no", fractional_manager_ != nullptr ? "yes" : "no",
              toplevel_global_ != 0 ? "yes" : "no", seat_ != nullptr ? "yes" : "no");
    return {};
}

int Display::fd() const {
    return wl_display_get_fd(display_);
}

bool Display::prepare() {
    while (wl_display_prepare_read(display_) != 0) {
        if (wl_display_dispatch_pending(display_) < 0) return false;
    }
    flush_pending_ = false;
    if (wl_display_flush(display_) < 0) {
        if (errno != EAGAIN) {
            wl_display_cancel_read(display_);
            return false;
        }
        flush_pending_ = true;
    }
    return true;
}

void Display::read() {
    wl_display_read_events(display_);
}

void Display::cancel() {
    wl_display_cancel_read(display_);
}

bool Display::dispatch() {
    return wl_display_dispatch_pending(display_) >= 0;
}

Output* Display::find(std::string_view name) const {
    for (const auto& output : outputs_) {
        if (output->name() == name && output->configured()) return output.get();
    }
    return nullptr;
}

Output* Display::find_surface(wl_surface* surface) const {
    for (const auto& output : outputs_) {
        if (output->surface() == surface) return output.get();
    }
    return nullptr;
}

void Display::remove_output(std::uint32_t global) {
    auto it = std::ranges::find_if(outputs_, [&](const auto& o) { return o->global() == global; });
    if (it == outputs_.end()) return;
    if (pointer_output_ == it->get()) pointer_output_ = nullptr;
    wl_output* handle = (*it)->handle();
    for (auto& toplevel : toplevels_) std::erase(toplevel->outputs, handle);
    log::info("{}: output removed", (*it)->name());
    outputs_.erase(it);  // the destructor tells the listener
}

void Display::track_toplevels(bool on) {
    if (on == (toplevel_manager_ != nullptr)) return;
    if (on) {
        if (toplevel_global_ == 0) return;
        toplevel_manager_ = bind<zwlr_foreign_toplevel_manager_v1>(
            registry_, toplevel_global_, &zwlr_foreign_toplevel_manager_v1_interface,
            toplevel_offered_, toplevel_version);
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager_, &DisplayListeners::manager,
                                                      this);
        log::debug("foreign-toplevel: tracking fullscreen windows");
        return;
    }
    for (auto& toplevel : toplevels_) zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
    toplevels_.clear();
    // No "finished" is waited for; events to the destroyed proxy are dropped by libwayland.
    zwlr_foreign_toplevel_manager_v1_stop(toplevel_manager_);
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(toplevel_manager_));
    toplevel_manager_ = nullptr;
    recompute_covered();
    log::debug("foreign-toplevel: stopped");
}

void Display::recompute_covered() {
    for (const auto& output : outputs_) {
        // Only a fullscreen window with focus counts. The protocol does not say whether a
        // window's workspace is visible: on Hyprland a fullscreen window on a hidden or
        // special workspace is still "fullscreen on DP-2", and pausing for it would freeze a
        // wallpaper that is in plain view. Missing a covered output costs little, since the
        // compositor sends no frame callbacks to a surface it does not draw.
        bool covered = std::ranges::any_of(toplevels_, [&](const auto& t) {
            return t->fullscreen && t->activated && !t->minimized &&
                   std::ranges::find(t->outputs, output->handle()) != t->outputs.end();
        });
        if (covered == output->covered()) continue;
        output->set_covered(covered);
        if (output->configured()) listener_.output_covered(*output, covered);
    }
}

}  // namespace glint::wl
