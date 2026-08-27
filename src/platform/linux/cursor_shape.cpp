/**
 * @file src/platform/linux/cursor_shape.cpp
 * @brief Definitions for the X11 cursor shape watcher.
 *
 * Watches the X server's cursor via XFixes and publishes a platf::cursor_shape_t whenever it
 * changes. Well-known cursor names are published as names only; anything else is published as
 * the largest raster the current Xcursor theme ships for that name (or the server's rendered
 * bitmap for unnamed cursors), so the client only ever has to downscale.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

// platform includes
#include <sys/select.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xlib.h>

// local includes
#include "cursor_shape.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::x11 {
  namespace {
    // Largest raster we will put on the wire. The control stream packet length is 16 bits and
    // encryption adds a few dozen bytes, so stay comfortably below 64 KiB.
    constexpr std::size_t max_raster_bytes = 60 * 1024;
    // Minimum spacing between publishes of the same named-but-unlisted cursor (animated cursors
    // deliver a CursorNotify per frame).
    constexpr auto same_name_min_interval = 250ms;

    // Names the client is expected to resolve to a native cursor. This list is part of the
    // protocol contract; see cursor-shape-sync-brief.md.
    const std::unordered_set<std::string_view> well_known_names {
      "default"sv, "left_ptr"sv, "arrow"sv, "top_left_arrow"sv,
      "text"sv, "xterm"sv, "ibeam"sv,
      "vertical-text"sv,
      "pointer"sv, "hand"sv, "hand1"sv, "hand2"sv, "pointing_hand"sv,
      "grab"sv, "openhand"sv, "fleur"sv,
      "grabbing"sv, "closedhand"sv, "dnd-move"sv,
      "crosshair"sv, "cross"sv, "tcross"sv,
      "not-allowed"sv, "no-drop"sv, "crossed_circle"sv, "forbidden"sv, "dnd-no-drop"sv,
      "col-resize"sv, "ew-resize"sv, "sb_h_double_arrow"sv, "h_double_arrow"sv, "size_hor"sv, "split_h"sv,
      "row-resize"sv, "ns-resize"sv, "sb_v_double_arrow"sv, "v_double_arrow"sv, "size_ver"sv, "split_v"sv,
      "e-resize"sv, "right_side"sv, "right_arrow"sv,
      "w-resize"sv, "left_side"sv, "left_arrow"sv,
      "n-resize"sv, "top_side"sv, "up_arrow"sv,
      "s-resize"sv, "bottom_side"sv, "down_arrow"sv,
      "nwse-resize"sv, "nw-resize"sv, "se-resize"sv, "size_fdiag"sv, "top_left_corner"sv, "bottom_right_corner"sv,
      "nesw-resize"sv, "ne-resize"sv, "sw-resize"sv, "size_bdiag"sv, "top_right_corner"sv, "bottom_left_corner"sv,
      "all-scroll"sv, "move"sv, "size_all"sv,
      "context-menu"sv,
      "copy"sv, "dnd-copy"sv,
      "alias"sv, "dnd-link"sv,
      "help"sv, "question_arrow"sv, "whats_this"sv,
      "wait"sv, "watch"sv,
      "progress"sv, "left_ptr_watch"sv, "half-busy"sv,
      "zoom-in"sv, "zoom-out"sv,
      "cell"sv, "plus"sv,
      "none"sv,
    };

    // Dynamically loaded Xlib / XFixes entry points. Sunshine avoids a hard link dependency on
    // X11 so the same binary runs on Wayland-only systems.
#define _FN(x, ret, args) \
  typedef ret(*x##_fn) args; \
  x##_fn x
    _FN(OpenDisplay, Display *, (_Xconst char *display_name));
    _FN(CloseDisplay, int, (Display * display));
    _FN(Free, int, (void *data));
    _FN(InitThreads, Status, (void) );
    _FN(DefaultScreen, int, (Display * display));
    _FN(RootWindow, Window, (Display * display, int screen));
    _FN(ConnectionNumber, int, (Display * display));
    _FN(Pending, int, (Display * display));
    _FN(NextEvent, int, (Display * display, XEvent *event));
    _FN(InternAtom, Atom, (Display * display, _Xconst char *atom_name, Bool only_if_exists));
    _FN(GetSelectionOwner, Window, (Display * display, Atom selection));
    _FN(GetWindowProperty, int, (Display * display, Window w, Atom property, long long_offset, long long_length, Bool del, Atom req_type, Atom *actual_type, int *actual_format, unsigned long *nitems, unsigned long *bytes_after, unsigned char **prop));
    _FN(GetDefault, char *, (Display * display, _Xconst char *program, _Xconst char *option));
    _FN(FixesQueryExtension, Bool, (Display * dpy, int *event_base, int *error_base));
    _FN(FixesSelectCursorInput, void, (Display * dpy, Window win, unsigned long event_mask));
    _FN(FixesGetCursorImage, XFixesCursorImage *, (Display * dpy));
#undef _FN

    int load_syms() {
      static void *x11_handle {nullptr};
      static void *fixes_handle {nullptr};
      static bool loaded = false;
      if (loaded) {
        return 0;
      }
      if (!x11_handle) {
        x11_handle = dyn::handle({"libX11.so.6", "libX11.so"});
        if (!x11_handle) {
          return -1;
        }
      }
      if (!fixes_handle) {
        fixes_handle = dyn::handle({"libXfixes.so.3", "libXfixes.so"});
        if (!fixes_handle) {
          return -1;
        }
      }
      std::vector<std::tuple<dyn::apiproc *, const char *>> x11_funcs {
        {(dyn::apiproc *) &OpenDisplay, "XOpenDisplay"},
        {(dyn::apiproc *) &CloseDisplay, "XCloseDisplay"},
        {(dyn::apiproc *) &Free, "XFree"},
        {(dyn::apiproc *) &InitThreads, "XInitThreads"},
        {(dyn::apiproc *) &DefaultScreen, "XDefaultScreen"},
        {(dyn::apiproc *) &RootWindow, "XRootWindow"},
        {(dyn::apiproc *) &ConnectionNumber, "XConnectionNumber"},
        {(dyn::apiproc *) &Pending, "XPending"},
        {(dyn::apiproc *) &NextEvent, "XNextEvent"},
        {(dyn::apiproc *) &InternAtom, "XInternAtom"},
        {(dyn::apiproc *) &GetSelectionOwner, "XGetSelectionOwner"},
        {(dyn::apiproc *) &GetWindowProperty, "XGetWindowProperty"},
        {(dyn::apiproc *) &GetDefault, "XGetDefault"},
      };
      std::vector<std::tuple<dyn::apiproc *, const char *>> fixes_funcs {
        {(dyn::apiproc *) &FixesQueryExtension, "XFixesQueryExtension"},
        {(dyn::apiproc *) &FixesSelectCursorInput, "XFixesSelectCursorInput"},
        {(dyn::apiproc *) &FixesGetCursorImage, "XFixesGetCursorImage"},
      };
      if (dyn::load(x11_handle, x11_funcs) || dyn::load(fixes_handle, fixes_funcs)) {
        return -1;
      }
      loaded = true;
      return 0;
    }

    struct display_deleter {
      void operator()(Display *d) const {
        CloseDisplay(d);
      }
    };

    using display_ptr = std::unique_ptr<Display, display_deleter>;

    // ---- theme discovery -------------------------------------------------------------------

    struct theme_settings_t {
      std::string theme;
      int size = 0;
      std::string source;  // for logging
    };

    /**
     * @brief Read Gtk/CursorThemeName and Gtk/CursorThemeSize from the XSETTINGS manager.
     *
     * This is what GTK applications actually use on X11 (gsd-xsettings exports the gsettings
     * values), so it beats the Xcursor.theme resource when the two disagree.
     */
    std::optional<theme_settings_t> read_xsettings(Display *dpy) {
      auto screen = DefaultScreen(dpy);
      auto sel_name = "_XSETTINGS_S" + std::to_string(screen);
      auto sel = InternAtom(dpy, sel_name.c_str(), True);
      if (sel == None) {
        return std::nullopt;
      }
      auto owner = GetSelectionOwner(dpy, sel);
      if (owner == None) {
        return std::nullopt;
      }
      auto prop = InternAtom(dpy, "_XSETTINGS_SETTINGS", True);
      if (prop == None) {
        return std::nullopt;
      }

      Atom actual_type;
      int actual_format;
      unsigned long nitems, bytes_after;
      unsigned char *raw = nullptr;
      if (GetWindowProperty(dpy, owner, prop, 0, 0x7fffffff, False, prop, &actual_type, &actual_format, &nitems, &bytes_after, &raw) != Success || !raw) {
        return std::nullopt;
      }
      auto free_raw = util::fail_guard([raw]() {
        Free(raw);
      });
      if (actual_format != 8 || nitems < 12) {
        return std::nullopt;
      }

      const std::uint8_t *p = raw;
      const std::uint8_t *end = raw + nitems;
      bool big_endian = p[0] != 0;
      auto rd32 = [&](const std::uint8_t *at) -> std::uint32_t {
        return big_endian ?
                 (at[0] << 24) | (at[1] << 16) | (at[2] << 8) | at[3] :
                 (at[3] << 24) | (at[2] << 16) | (at[1] << 8) | at[0];
      };
      auto rd16 = [&](const std::uint8_t *at) -> std::uint16_t {
        return big_endian ? (at[0] << 8) | at[1] : (at[1] << 8) | at[0];
      };
      auto pad4 = [](std::size_t n) {
        return (n + 3) & ~std::size_t(3);
      };

      auto n_settings = rd32(p + 8);
      p += 12;
      theme_settings_t out;
      bool found = false;
      for (std::uint32_t i = 0; i < n_settings && p + 8 <= end; ++i) {
        auto type = p[0];
        auto name_len = rd16(p + 2);
        p += 4;
        if (p + pad4(name_len) + 4 > end) {
          break;
        }
        std::string_view name {(const char *) p, name_len};
        p += pad4(name_len) + 4;  // name + last-change-serial
        if (type == 0) {  // integer
          if (p + 4 > end) {
            break;
          }
          auto v = (std::int32_t) rd32(p);
          p += 4;
          if (name == "Gtk/CursorThemeSize"sv) {
            out.size = v;
            found = true;
          }
        } else if (type == 1) {  // string
          if (p + 4 > end) {
            break;
          }
          auto len = rd32(p);
          p += 4;
          if (p + pad4(len) > end) {
            break;
          }
          if (name == "Gtk/CursorThemeName"sv) {
            out.theme.assign((const char *) p, len);
            found = true;
          }
          p += pad4(len);
        } else if (type == 2) {  // color
          p += 8;
        } else {
          break;
        }
      }
      if (!found) {
        return std::nullopt;
      }
      out.source = "XSETTINGS";
      return out;
    }

    theme_settings_t resolve_theme(Display *dpy) {
      theme_settings_t out;
      if (!config::video.cursor_theme.empty()) {
        out.theme = config::video.cursor_theme;
        out.source = "config (cursor_theme)";
      }
      auto xs = read_xsettings(dpy);
      if (out.theme.empty() && xs && !xs->theme.empty()) {
        out.theme = xs->theme;
        out.source = xs->source;
      }
      if (xs) {
        out.size = xs->size;
      }
      if (out.theme.empty()) {
        if (auto env = std::getenv("XCURSOR_THEME"); env && *env) {
          out.theme = env;
          out.source = "XCURSOR_THEME";
        }
      }
      if (out.theme.empty()) {
        if (auto res = GetDefault(dpy, "Xcursor", "theme"); res && *res) {
          out.theme = res;
          out.source = "Xcursor.theme resource";
        }
      }
      if (out.theme.empty()) {
        out.theme = "default";
        out.source = "fallback";
      }
      if (out.size <= 0) {
        if (auto env = std::getenv("XCURSOR_SIZE"); env && *env) {
          out.size = std::atoi(env);
        }
      }
      if (out.size <= 0) {
        if (auto res = GetDefault(dpy, "Xcursor", "size"); res && *res) {
          out.size = std::atoi(res);
        }
      }
      return out;
    }

    std::vector<std::filesystem::path> xcursor_search_path() {
      std::vector<std::filesystem::path> out;
      std::string path;
      if (auto env = std::getenv("XCURSOR_PATH"); env && *env) {
        path = env;
      } else {
        std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
        path = home + "/.local/share/icons:" + home + "/.icons:/usr/share/icons:/usr/share/pixmaps:/usr/X11R6/lib/X11/icons";
      }
      std::size_t start = 0;
      while (start <= path.size()) {
        auto colon = path.find(':', start);
        auto part = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!part.empty()) {
          out.emplace_back(part);
        }
        if (colon == std::string::npos) {
          break;
        }
        start = colon + 1;
      }
      return out;
    }

    /**
     * @brief Read the `Inherits=` line of a theme's index.theme / cursor.theme.
     */
    std::vector<std::string> theme_parents(const std::filesystem::path &theme_dir) {
      std::vector<std::string> out;
      for (auto file : {"index.theme", "cursor.theme"}) {
        std::ifstream in(theme_dir / file);
        std::string line;
        while (std::getline(in, line)) {
          if (line.rfind("Inherits", 0) != 0) {
            continue;
          }
          auto eq = line.find('=');
          if (eq == std::string::npos) {
            continue;
          }
          auto value = line.substr(eq + 1);
          std::size_t s = 0;
          while (s <= value.size()) {
            auto e = value.find_first_of(",;", s);
            auto name = value.substr(s, e == std::string::npos ? std::string::npos : e - s);
            // trim
            auto b = name.find_first_not_of(" \t\r");
            auto f = name.find_last_not_of(" \t\r");
            if (b != std::string::npos) {
              out.push_back(name.substr(b, f - b + 1));
            }
            if (e == std::string::npos) {
              break;
            }
            s = e + 1;
          }
        }
      }
      return out;
    }

    struct theme_file_t {
      std::filesystem::path path;
      bool svg;
    };

    /**
     * @brief Locate a cursor file by name in a theme or its inheritance chain.
     */
    std::optional<theme_file_t> find_theme_file(const std::string &theme, const std::string &name, std::unordered_set<std::string> &visited, int depth = 0) {
      if (depth > 8 || !visited.insert(theme).second) {
        return std::nullopt;
      }
      // Reject anything that could escape the theme directory.
      if (name.find('/') != std::string::npos || name.find("..") != std::string::npos || theme.find('/') != std::string::npos || theme.find("..") != std::string::npos) {
        return std::nullopt;
      }
      std::vector<std::filesystem::path> theme_dirs;
      for (auto &dir : xcursor_search_path()) {
        std::error_code ec;
        auto theme_dir = dir / theme;
        if (!std::filesystem::is_directory(theme_dir, ec)) {
          continue;
        }
        theme_dirs.push_back(theme_dir);
        auto svg = theme_dir / "cursors_scalable" / (name + ".svg");
        if (std::filesystem::is_regular_file(svg, ec)) {
          return theme_file_t {svg, true};
        }
        auto raster = theme_dir / "cursors" / name;
        if (std::filesystem::is_regular_file(raster, ec)) {
          return theme_file_t {raster, false};
        }
      }
      for (auto &theme_dir : theme_dirs) {
        for (auto &parent : theme_parents(theme_dir)) {
          if (auto found = find_theme_file(parent, name, visited, depth + 1)) {
            return found;
          }
        }
      }
      return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path &path, std::size_t max_size) {
      std::ifstream in(path, std::ios::binary | std::ios::ate);
      if (!in) {
        return std::nullopt;
      }
      auto size = (std::size_t) in.tellg();
      if (size == 0 || size > max_size) {
        return std::nullopt;
      }
      std::vector<std::uint8_t> buf(size);
      in.seekg(0);
      in.read((char *) buf.data(), size);
      if (!in) {
        return std::nullopt;
      }
      return buf;
    }

    struct raster_t {
      std::uint16_t nominal, width, height, hot_x, hot_y;
      std::vector<std::uint8_t> pixels;  // BGRA in memory == uint32 0xAARRGGBB little-endian
    };

    /**
     * @brief Parse an Xcursor file and return the first frame of the largest size that fits on the wire.
     *
     * Format: "Xcur" magic, header size, version, ntoc; then ntoc entries of (type, subtype, position);
     * image chunks (type 0xfffd0002) at position: header size, type, subtype (nominal size), version,
     * width, height, xhot, yhot, delay, then width*height premultiplied ARGB32 little-endian pixels.
     */
    std::optional<raster_t> parse_xcursor(const std::vector<std::uint8_t> &f) {
      auto rd32 = [&](std::size_t at) -> std::uint32_t {
        return at + 4 <= f.size() ? (std::uint32_t) f[at] | (f[at + 1] << 8) | (f[at + 2] << 16) | ((std::uint32_t) f[at + 3] << 24) : 0;
      };
      if (f.size() < 16 || std::memcmp(f.data(), "Xcur", 4) != 0) {
        return std::nullopt;
      }
      auto ntoc = rd32(12);
      std::optional<raster_t> best;
      std::uint32_t best_nominal = 0;
      for (std::uint32_t i = 0; i < ntoc && i < 4096; ++i) {
        auto entry = 16 + 12 * (std::size_t) i;
        if (entry + 12 > f.size()) {
          break;
        }
        auto type = rd32(entry);
        auto nominal = rd32(entry + 4);
        auto pos = rd32(entry + 8);
        if (type != 0xfffd0002 || nominal <= best_nominal) {
          continue;  // not an image, or not larger than what we already have (first frame wins)
        }
        if (pos + 36 > f.size()) {
          continue;
        }
        auto width = rd32(pos + 16);
        auto height = rd32(pos + 20);
        auto xhot = rd32(pos + 24);
        auto yhot = rd32(pos + 28);
        if (width == 0 || height == 0 || width > 0x7fff || height > 0x7fff) {
          continue;
        }
        auto bytes = (std::size_t) width * height * 4;
        if (bytes > max_raster_bytes || pos + 36 + bytes > f.size()) {
          continue;
        }
        raster_t r;
        r.nominal = nominal;
        r.width = width;
        r.height = height;
        r.hot_x = xhot;
        r.hot_y = yhot;
        r.pixels.assign(f.begin() + pos + 36, f.begin() + pos + 36 + bytes);
        best = std::move(r);
        best_nominal = nominal;
      }
      return best;
    }

    // ---- watcher -----------------------------------------------------------------------------

    std::mutex state_mutex;
    std::shared_ptr<const cursor_shape_t> current;
    std::uint64_t generation = 0;
    std::thread watcher;
    std::atomic<bool> stop_flag {false};
    std::atomic<bool> running {false};

    void publish(cursor_shape_t shape) {
      auto sp = std::make_shared<cursor_shape_t>(std::move(shape));
      {
        std::lock_guard lg(state_mutex);
        sp->generation = ++generation;
        current = sp;
      }
      BOOST_LOG(debug) << "Cursor shape: "sv
                       << (sp->format == cursor_shape_format::named ? "named"sv : sp->format == cursor_shape_format::argb ? "argb"sv : sp->format == cursor_shape_format::svg ? "svg"sv : "hidden"sv)
                       << " \""sv << sp->name << "\" "sv << sp->width << 'x' << sp->height
                       << " hot("sv << sp->hot_x << ',' << sp->hot_y << ") nominal "sv << sp->nominal_size
                       << ", "sv << sp->data.size() << " bytes"sv;
    }

    struct watcher_ctx_t {
      Display *dpy;
      theme_settings_t theme;
      unsigned long last_serial = 0;
      std::string last_name;
      std::uint8_t last_format = 0xff;
      std::chrono::steady_clock::time_point last_publish {};

      // Animated cursors: the server first reports a placeholder (serial 0, 1x1) carrying the
      // name, then one unnamed image per frame with the real geometry.
      bool in_animation = false;  // a placeholder was seen and no unrelated cursor since
      bool anim_geometry_known = false;  // the first frame has been seen
      bool anim_frames_needed = false;  // nothing usable was published for the placeholder, so frames must be
      std::uint16_t anim_w = 0, anim_h = 0, anim_hx = 0, anim_hy = 0;
    };

    /**
     * @brief Try to build a shape from the theme's own file for a named cursor.
     * @return `true` if `s` was filled in.
     */
    bool shape_from_theme(const watcher_ctx_t &ctx, const std::string &name, cursor_shape_t &s) {
      std::unordered_set<std::string> visited;
      auto file = find_theme_file(ctx.theme.theme, name, visited);
      if (!file) {
        return false;
      }
      auto bytes = read_file(file->path, file->svg ? max_raster_bytes : 16 * 1024 * 1024);
      if (!bytes) {
        return false;
      }
      if (file->svg) {
        s.format = cursor_shape_format::svg;
        s.hot_x = 0;  // hotspot metadata for scalable cursors lives in the theme's index; not supported yet
        s.hot_y = 0;
        s.data = std::move(*bytes);
        return true;
      }
      auto raster = parse_xcursor(*bytes);
      if (!raster) {
        return false;
      }
      s.format = cursor_shape_format::argb;
      s.width = raster->width;
      s.height = raster->height;
      s.hot_x = raster->hot_x;
      s.hot_y = raster->hot_y;
      s.data = std::move(raster->pixels);
      return true;
    }

    /**
     * @brief Build a shape from the image the server rendered.
     * @return `false` if the image is unusable (too large / empty); `s` is then the default arrow.
     */
    bool shape_from_server(const XFixesCursorImage *img, cursor_shape_t &s) {
      auto bytes = (std::size_t) img->width * img->height * 4;
      if (img->width == 0 || img->height == 0 || bytes > max_raster_bytes) {
        BOOST_LOG(warning) << "Cursor shape: cursor \""sv << s.name << "\" ("sv << img->width << 'x' << img->height << ") is too large to send; using the default arrow instead"sv;
        s.format = cursor_shape_format::named;
        s.name = "default";
        s.data.clear();
        return false;
      }
      s.format = cursor_shape_format::argb;
      s.width = img->width;
      s.height = img->height;
      s.hot_x = img->xhot;
      s.hot_y = img->yhot;
      s.data.resize(bytes);
      // XFixes hands back one unsigned long per pixel holding a 32-bit premultiplied ARGB value.
      for (std::size_t i = 0; i < (std::size_t) img->width * img->height; ++i) {
        auto px = (std::uint32_t) img->pixels[i];
        s.data[i * 4 + 0] = px & 0xff;
        s.data[i * 4 + 1] = (px >> 8) & 0xff;
        s.data[i * 4 + 2] = (px >> 16) & 0xff;
        s.data[i * 4 + 3] = (px >> 24) & 0xff;
      }
      return true;
    }

    void read_and_publish(watcher_ctx_t &ctx) {
      auto img = FixesGetCursorImage(ctx.dpy);
      if (!img) {
        return;
      }
      auto free_img = util::fail_guard([img]() {
        Free(img);
      });
      if (img->cursor_serial == ctx.last_serial) {
        return;
      }
      ctx.last_serial = img->cursor_serial;

      std::string name = img->name ? img->name : "";
      if (name.size() > 63) {
        name.clear();
      }
      auto now = std::chrono::steady_clock::now();
      auto nominal = (std::uint16_t) (ctx.theme.size > 0 ? ctx.theme.size : std::max(img->width, img->height));
      bool placeholder = img->cursor_serial == 0 || (img->width <= 1 && img->height <= 1);

      BOOST_LOG(verbose) << "Cursor shape: server image serial "sv << img->cursor_serial << " name \""sv << name << "\" "sv << img->width << 'x' << img->height << " hot("sv << img->xhot << ',' << img->yhot << ')' << (placeholder ? " (animation placeholder)"sv : ""sv);

      // ---- animation frames ------------------------------------------------------------------
      if (placeholder) {
        ctx.in_animation = true;
        ctx.anim_geometry_known = false;
        ctx.anim_frames_needed = false;
      } else if (!name.empty()) {
        ctx.in_animation = false;  // a real named cursor ends any animation
      } else if (ctx.in_animation) {
        if (!ctx.anim_geometry_known) {
          ctx.anim_geometry_known = true;
          ctx.anim_w = img->width;
          ctx.anim_h = img->height;
          ctx.anim_hx = img->xhot;
          ctx.anim_hy = img->yhot;
        } else if (img->width != ctx.anim_w || img->height != ctx.anim_h || img->xhot != ctx.anim_hx || img->yhot != ctx.anim_hy) {
          ctx.in_animation = false;  // different geometry: this is some other cursor
        }
        if (ctx.in_animation) {
          if (!ctx.anim_frames_needed) {
            return;  // the placeholder was published as a name or theme image; frames add nothing
          }
          if (now - ctx.last_publish < same_name_min_interval && ctx.last_format == cursor_shape_format::argb) {
            return;  // crude animation from server frames, rate limited
          }
        }
      }

      cursor_shape_t s {};
      s.name = name;
      s.nominal_size = nominal;

      // ---- well-known: name only ---------------------------------------------------------------
      if (!name.empty() && well_known_names.count(name)) {
        if (ctx.last_format == cursor_shape_format::named && ctx.last_name == name) {
          return;
        }
        s.format = cursor_shape_format::named;
      }
      // ---- named but unlisted: theme file, else server image -----------------------------------
      else if (!name.empty()) {
        if (!shape_from_theme(ctx, name, s)) {
          if (placeholder) {
            // Nothing usable in the theme and the placeholder has no pixels; send the frames instead
            ctx.anim_frames_needed = true;
            return;
          }
          shape_from_server(img, s);
        }
      }
      // ---- unnamed: server image --------------------------------------------------------------
      else {
        if (placeholder) {
          ctx.anim_frames_needed = true;
          return;
        }
        shape_from_server(img, s);
      }

      ctx.last_name = name;
      ctx.last_format = s.format;
      ctx.last_publish = now;
      publish(std::move(s));
    }

    void watcher_main() {
      display_ptr dpy {OpenDisplay(nullptr)};
      if (!dpy) {
        BOOST_LOG(warning) << "Cursor shape: couldn't open X display; no cursor shapes will be sent"sv;
        return;
      }
      int event_base, error_base;
      if (!FixesQueryExtension(dpy.get(), &event_base, &error_base)) {
        BOOST_LOG(warning) << "Cursor shape: XFixes extension not available; no cursor shapes will be sent"sv;
        return;
      }

      watcher_ctx_t ctx {dpy.get()};
      ctx.theme = resolve_theme(dpy.get());
      {
        std::unordered_set<std::string> visited;
        auto probe = find_theme_file(ctx.theme.theme, "left_ptr", visited);
        BOOST_LOG(info) << "Cursor shape: watching X cursor; theme \""sv << ctx.theme.theme << "\" (from "sv << ctx.theme.source << "), size "sv << ctx.theme.size
                        << (probe ? ", files at " + probe->path.parent_path().string() : ", theme files not found (unlisted cursors will use the server-rendered bitmap)"s);
      }

      FixesSelectCursorInput(dpy.get(), RootWindow(dpy.get(), DefaultScreen(dpy.get())), XFixesDisplayCursorNotifyMask);
      read_and_publish(ctx);

      auto fd = ConnectionNumber(dpy.get());
      while (!stop_flag.load(std::memory_order_acquire)) {
        bool changed = false;
        while (Pending(dpy.get())) {
          XEvent ev;
          NextEvent(dpy.get(), &ev);
          if (ev.type == event_base + XFixesCursorNotify) {
            changed = true;
          }
        }
        if (changed) {
          read_and_publish(ctx);
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval tv {0, 250 * 1000};
        select(fd + 1, &fds, nullptr, nullptr, &tv);
      }
      BOOST_LOG(debug) << "Cursor shape: watcher stopped"sv;
    }
  }  // namespace

  bool cursor_shape_supported() {
    static int cached = -1;
    if (cached >= 0) {
      return cached;
    }
    cached = 0;
    if (load_syms()) {
      return false;
    }
    display_ptr dpy {OpenDisplay(nullptr)};
    if (!dpy) {
      return false;
    }
    int eb, erb;
    cached = FixesQueryExtension(dpy.get(), &eb, &erb) ? 1 : 0;
    return cached;
  }

  void cursor_shape_start() {
    std::lock_guard lg(state_mutex);
    if (running) {
      return;
    }
    if (load_syms()) {
      return;
    }
    InitThreads();
    stop_flag = false;
    running = true;
    watcher = std::thread(watcher_main);
  }

  void cursor_shape_stop() {
    std::thread t;
    {
      std::lock_guard lg(state_mutex);
      if (!running) {
        return;
      }
      stop_flag = true;
      t = std::move(watcher);
      running = false;
    }
    if (t.joinable()) {
      t.join();
    }
    std::lock_guard lg(state_mutex);
    current.reset();
  }

  std::shared_ptr<const cursor_shape_t> current_cursor_shape() {
    std::lock_guard lg(state_mutex);
    return current;
  }
}  // namespace platf::x11
