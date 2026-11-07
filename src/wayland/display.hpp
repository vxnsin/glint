#pragma once

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_output;
struct wl_surface;
struct wl_callback;
struct wl_seat;
struct wl_pointer;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct zwlr_foreign_toplevel_manager_v1;
struct zwlr_foreign_toplevel_handle_v1;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;
struct zwp_linux_dmabuf_v1;
struct zwp_linux_dmabuf_feedback_v1;

// The Wayland side of glint: globals, one Output per wl_output with its layer surface on the
// background layer, fullscreen toplevels per output, the pointer. Everything here runs on the
// main thread; the daemon gets told about changes through Display::Listener.
namespace glint::wl {

class Display;

class Output {
public:
    Output(Display& display, std::uint32_t global, wl_output* output);
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    // From wl_output.
    const std::string& name() const { return name_; }
    const std::string& description() const { return description_; }
    const std::string& make() const { return make_; }
    const std::string& model() const { return model_; }
    std::int32_t x() const { return x_; }
    std::int32_t y() const { return y_; }
    std::int32_t mode_width() const { return mode_width_; }
    std::int32_t mode_height() const { return mode_height_; }
    double refresh_hz() const { return refresh_mhz_ / 1000.0; }

    // The layer surface. Sizes are known once configured() is true.
    wl_surface* surface() const { return surface_; }
    bool configured() const { return configured_; }
    std::uint32_t logical_width() const { return logical_width_; }
    std::uint32_t logical_height() const { return logical_height_; }
    double scale() const { return static_cast<double>(scale120_) / 120.0; }
    std::uint32_t pixel_width() const;
    std::uint32_t pixel_height() const;

    // Asks for a wl_surface.frame callback with the next commit (the present does it).
    void request_frame();
    bool frame_pending() const { return frame_ != nullptr; }
    // Gives up on a frame callback the compositor may never send (one asked for before a
    // pause, which Hyprland can hold back for good): the next draw goes ahead without it.
    void abandon_frame();
    // Reassert the background layer after Hyprland leaves a newly mapped layer transparent.
    void restore_background_layer();
    // When the last frame callback came (CLOCK_MONOTONIC ns): the compositor had shown the
    // frame before it by then. 0 before the first.
    std::int64_t frame_done_ns() const { return frame_done_ns_; }

    // Viewport destination and opaque region for the current size. Pending state: it takes
    // effect with the next commit, which is the next present.
    void apply_surface_state();

    bool covered() const { return covered_; }

    // Whether the surface takes pointer input. Off by default: an empty input region, so
    // pointer motion over the desktop does not wake glint. On when a wallpaper wants the
    // cursor. Pending state, like apply_surface_state().
    void set_pointer_input(bool on);
    bool pointer_input() const { return pointer_input_; }

    // Internal, called by Display and the listeners.
    std::uint32_t global() const { return global_; }
    wl_output* handle() const { return output_; }
    void create_surface();
    void destroy_surface();
    void set_covered(bool covered) { covered_ = covered; }
    // Recomputes the scale in use; true if it changed.
    bool update_scale();

private:
    friend struct OutputListeners;

    Display& display_;
    std::uint32_t global_;
    wl_output* output_;

    std::string name_, description_, make_, model_;
    std::int32_t x_ = 0, y_ = 0;
    std::int32_t mode_width_ = 0, mode_height_ = 0, refresh_mhz_ = 0;
    std::int32_t integer_scale_ = 1;
    bool announced_ = false;

    wl_surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layer_ = nullptr;
    wp_viewport* viewport_ = nullptr;
    wp_fractional_scale_v1* fractional_ = nullptr;
    wl_callback* frame_ = nullptr;
    std::int64_t frame_done_ns_ = 0;
    bool configured_ = false;
    std::uint32_t logical_width_ = 0, logical_height_ = 0;
    std::uint32_t scale120_ = 120;    // in use
    std::uint32_t preferred120_ = 0;  // from wp_fractional_scale_v1, 0 until it says

    bool covered_ = false;
    bool pointer_input_ = false;
};

class Display {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        // The output has a name and a configured layer surface; it can be drawn to.
        virtual void output_added(Output& output) = 0;
        // Called before the Output is destroyed, or before its surface is recreated.
        virtual void output_removed(Output& output) = 0;
        // Pixel size or scale changed.
        virtual void output_resized(Output& output) = 0;
        // A wl_surface.frame callback arrived; time in milliseconds.
        virtual void output_frame(Output& output, std::uint32_t time_ms) = 0;
        virtual void output_covered(Output& output, bool covered) = 0;
        // Pointer position in output pixels.
        virtual void pointer(Output& output, double x, double y, std::uint32_t buttons) = 0;
    };

    explicit Display(Listener& listener);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // Connects and runs the initial roundtrips. Returns an error message or empty.
    std::string connect();

    wl_display* handle() const { return display_; }
    int fd() const;

    // The poll protocol of libwayland: prepare() before poll, then read() if the fd was
    // readable or cancel() if not, then dispatch(). prepare() returns false when the
    // connection broke. flush_pending() says whether poll should wait for POLLOUT.
    // Events already read but not yet handled (none while a read is prepared); how many.
    int dispatch_pending();
    // Lets go of the outputs and the toplevel tracking while the listener is still whole
    // (the destructor does it too).
    void shutdown();
    bool prepare();
    void read();
    void cancel();
    bool dispatch();
    bool flush_pending() const { return flush_pending_; }

    const std::vector<std::unique_ptr<Output>>& outputs() const { return outputs_; }
    Output* find(std::string_view name) const;

    // Follow toplevels to see fullscreen windows per output. Off by default: the compositor
    // sends an event for every title change of every window, which would wake glint for
    // nothing while it shows still pictures (covered stills get no frame callbacks anyway).
    void track_toplevels(bool on);
    bool tracking_toplevels() const { return toplevel_manager_ != nullptr; }

    // The DRM device the compositor renders with, from linux-dmabuf feedback.
    std::optional<dev_t> main_device() const { return main_device_; }

    // Internal, for the listeners.
    Listener& listener() { return listener_; }
    wl_compositor* compositor() const { return compositor_; }
    zwlr_layer_shell_v1* layer_shell() const { return layer_shell_; }
    wp_viewporter* viewporter() const { return viewporter_; }
    wp_fractional_scale_manager_v1* fractional_manager() const { return fractional_manager_; }
    void recompute_covered();

private:
    friend struct DisplayListeners;

    struct Toplevel {
        Display* display = nullptr;
        zwlr_foreign_toplevel_handle_v1* handle = nullptr;
        std::vector<wl_output*> outputs;
        bool fullscreen = false, minimized = false, activated = false;
        bool pending_fullscreen = false, pending_minimized = false, pending_activated = false;
        std::uint64_t activated_serial = 0;  // when it last had focus, by order
    };
    std::uint64_t activation_serial_ = 0;

    Output* find_surface(wl_surface* surface) const;
    void remove_output(std::uint32_t global);

    Listener& listener_;
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    wp_viewporter* viewporter_ = nullptr;
    wp_fractional_scale_manager_v1* fractional_manager_ = nullptr;
    zwlr_foreign_toplevel_manager_v1* toplevel_manager_ = nullptr;
    std::uint32_t toplevel_global_ = 0;
    std::uint32_t toplevel_offered_ = 0;
    zwp_linux_dmabuf_v1* dmabuf_ = nullptr;
    zwp_linux_dmabuf_feedback_v1* feedback_ = nullptr;
    wl_seat* seat_ = nullptr;
    std::uint32_t seat_global_ = 0;
    wl_pointer* pointer_ = nullptr;

    std::vector<std::unique_ptr<Output>> outputs_;
    std::vector<std::unique_ptr<Toplevel>> toplevels_;
    std::vector<zwlr_foreign_toplevel_manager_v1*> stopping_managers_;  // told to stop
    std::optional<dev_t> main_device_;
    bool flush_pending_ = false;

    Output* pointer_output_ = nullptr;
    double pointer_x_ = 0, pointer_y_ = 0;
    std::uint32_t buttons_ = 0;
};

}  // namespace glint::wl
