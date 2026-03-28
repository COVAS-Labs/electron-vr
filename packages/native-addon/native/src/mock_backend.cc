#include "mock_backend.h"

#include <array>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <drm/drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

namespace vrbridge {

namespace {

#if defined(__linux__)

std::string FormatHex(unsigned long value) {
  std::ostringstream stream;
  stream << "0x" << std::hex << value;
  return stream.str();
}

std::string EglErrorString(EGLint error) {
  return FormatHex(static_cast<unsigned long>(error));
}

GLuint CompileShader(GLenum shader_type, const char* source) {
  const GLuint shader = glCreateShader(shader_type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }

  GLint log_length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
  std::string log(static_cast<size_t>(log_length > 1 ? log_length : 1), '\0');
  if (log_length > 1) {
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
  }

  glDeleteShader(shader);
  throw std::runtime_error("Shader compilation failed: " + log);
}

GLuint LinkProgram(const char* vertex_source, const char* fragment_source) {
  const GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, vertex_source);
  const GLuint fragment_shader = CompileShader(GL_FRAGMENT_SHADER, fragment_source);

  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex_shader);
  glAttachShader(program, fragment_shader);
  glBindAttribLocation(program, 0, "a_position");
  glBindAttribLocation(program, 1, "a_tex_coord");
  glLinkProgram(program);

  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);

  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) {
    return program;
  }

  GLint log_length = 0;
  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
  std::string log(static_cast<size_t>(log_length > 1 ? log_length : 1), '\0');
  if (log_length > 1) {
    glGetProgramInfoLog(program, log_length, nullptr, log.data());
  }

  glDeleteProgram(program);
  throw std::runtime_error("Program link failed: " + log);
}

EGLint ToDrmFormat(const std::string& pixel_format) {
  if (pixel_format == "bgra") {
    return DRM_FORMAT_ARGB8888;
  }

  return DRM_FORMAT_ABGR8888;
}

bool ParseModifier(const std::string& modifier_string, uint64_t* modifier_out) {
  if (modifier_string.empty() || modifier_out == nullptr) {
    return false;
  }

  try {
    *modifier_out = std::stoull(modifier_string, nullptr, 0);
    return true;
  } catch (...) {
    return false;
  }
}

class LinuxMockPreviewWindow {
 public:
  bool Initialize(const InitializeOptions& options, std::string* error_message) {
    if (initialized_) {
      return true;
    }

    x_display_ = XOpenDisplay(nullptr);
    if (x_display_ == nullptr) {
      SetError(error_message, "Failed to open X11 display for mock preview.");
      return false;
    }

    egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(x_display_));
    if (egl_display_ == EGL_NO_DISPLAY) {
      SetError(error_message, "Failed to acquire EGL display for mock preview.");
      Cleanup();
      return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(egl_display_, &major, &minor) != EGL_TRUE) {
      SetError(error_message, "Failed to initialize EGL: " + EglErrorString(eglGetError()));
      Cleanup();
      return false;
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
      SetError(error_message, "Failed to bind OpenGL ES API: " + EglErrorString(eglGetError()));
      Cleanup();
      return false;
    }

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };

    EGLint num_configs = 0;
    if (eglChooseConfig(egl_display_, config_attributes, nullptr, 0, &num_configs) != EGL_TRUE || num_configs <= 0) {
      SetError(error_message, "Failed to choose EGL config: " + EglErrorString(eglGetError()));
      Cleanup();
      return false;
    }

    std::vector<EGLConfig> configs(static_cast<size_t>(num_configs));
    if (eglChooseConfig(egl_display_, config_attributes, configs.data(), num_configs, &num_configs) != EGL_TRUE || num_configs <= 0) {
      SetError(error_message, "Failed to enumerate EGL configs: " + EglErrorString(eglGetError()));
      Cleanup();
      return false;
    }

    XVisualInfo* visual_info = nullptr;
    for (EGLint config_index = 0; config_index < num_configs; ++config_index) {
      EGLint visual_id = 0;
      if (eglGetConfigAttrib(egl_display_, configs[static_cast<size_t>(config_index)], EGL_NATIVE_VISUAL_ID, &visual_id) != EGL_TRUE || visual_id == 0) {
        continue;
      }

      XVisualInfo visual_template{};
      visual_template.visualid = static_cast<VisualID>(visual_id);
      int matched_visuals = 0;
      XVisualInfo* candidate_visual_info = XGetVisualInfo(x_display_, VisualIDMask, &visual_template, &matched_visuals);
      if (candidate_visual_info == nullptr || matched_visuals == 0) {
        if (candidate_visual_info != nullptr) {
          XFree(candidate_visual_info);
        }
        continue;
      }

      if (candidate_visual_info->depth >= 32) {
        egl_config_ = configs[static_cast<size_t>(config_index)];
        visual_info = candidate_visual_info;
        break;
      }

      XFree(candidate_visual_info);
    }

    if (visual_info == nullptr) {
      SetError(error_message, "Failed to find an EGL/X11 visual with an alpha channel for mock preview transparency.");
      Cleanup();
      return false;
    }

    const int screen = DefaultScreen(x_display_);
    x_colormap_ = XCreateColormap(x_display_, RootWindow(x_display_, screen), visual_info->visual, AllocNone);

    XSetWindowAttributes window_attributes{};
    window_attributes.colormap = x_colormap_;
    window_attributes.background_pixmap = None;
    window_attributes.border_pixel = 0;
    window_attributes.event_mask = ExposureMask | StructureNotifyMask;

    x_window_ = XCreateWindow(
        x_display_,
        RootWindow(x_display_, screen),
        64,
        64,
        options.width,
        options.height,
        0,
        visual_info->depth,
        InputOutput,
        visual_info->visual,
        CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask,
        &window_attributes);

    if (x_window_ == 0) {
      XFree(visual_info);
      SetError(error_message, "Failed to create X11 mock preview window.");
      Cleanup();
      return false;
    }

    XFree(visual_info);

    XStoreName(x_display_, x_window_, "VR Mock Preview");
    delete_window_atom_ = XInternAtom(x_display_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(x_display_, x_window_, &delete_window_atom_, 1);
    XMapWindow(x_display_, x_window_);
    XFlush(x_display_);

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, reinterpret_cast<EGLNativeWindowType>(x_window_), nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
      SetError(error_message, "Failed to create EGL window surface: " + EglErrorString(eglGetError()));
      Cleanup();
      return false;
    }

    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, context_attributes);
    if (egl_context_ == EGL_NO_CONTEXT) {
      SetError(error_message, "Failed to create EGL context: " + EglErrorString(eglGetError()));
      Cleanup();
      return false;
    }

    if (eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_) != EGL_TRUE) {
      SetError(error_message, "Failed to make EGL context current: " + EglErrorString(eglGetError()));
      Cleanup();
      return false;
    }

    egl_create_image_khr_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    egl_destroy_image_khr_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    gl_egl_image_target_texture_2d_oes_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (egl_create_image_khr_ == nullptr || egl_destroy_image_khr_ == nullptr || gl_egl_image_target_texture_2d_oes_ == nullptr) {
      SetError(error_message, "Required EGL image extensions are unavailable for mock preview.");
      Cleanup();
      return false;
    }

    try {
      static constexpr char kVertexShaderSource[] = R"(
        attribute vec2 a_position;
        attribute vec2 a_tex_coord;
        varying vec2 v_tex_coord;
        void main() {
          v_tex_coord = a_tex_coord;
          gl_Position = vec4(a_position, 0.0, 1.0);
        }
      )";
      static constexpr char kFragmentShaderSource[] = R"(
        precision mediump float;
        varying vec2 v_tex_coord;
        uniform sampler2D u_texture;
        void main() {
          gl_FragColor = texture2D(u_texture, v_tex_coord);
        }
      )";

      program_ = LinkProgram(kVertexShaderSource, kFragmentShaderSource);
    } catch (const std::exception& error) {
      SetError(error_message, error.what());
      Cleanup();
      return false;
    }

    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_texture"), 0);

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    initialized_ = true;
    window_width_ = options.width;
    window_height_ = options.height;
    if (error_message != nullptr) {
      error_message->clear();
    }

    return true;
  }

  bool Submit(const LinuxTextureInfo& texture_info, std::string* error_message) {
    if (!initialized_) {
      SetError(error_message, "Mock preview window is not initialized.");
      return false;
    }

    PumpEvents();
    if (closed_) {
      SetError(error_message, "Mock preview window was closed.");
      return false;
    }

    if (texture_info.planes.empty()) {
      SetError(error_message, "Mock preview requires at least one DMA-BUF plane.");
      return false;
    }

    if (texture_info.width == 0 || texture_info.height == 0) {
      SetError(error_message, "Mock preview requires codedSize width and height.");
      return false;
    }

    const LinuxPlaneInfo& plane = texture_info.planes.front();
    std::array<EGLint, 32> attributes{};
    size_t attribute_index = 0;
    auto push_attribute = [&](EGLint key, EGLint value) {
      attributes[attribute_index++] = key;
      attributes[attribute_index++] = value;
    };

    push_attribute(EGL_WIDTH, static_cast<EGLint>(texture_info.width));
    push_attribute(EGL_HEIGHT, static_cast<EGLint>(texture_info.height));
    push_attribute(EGL_LINUX_DRM_FOURCC_EXT, ToDrmFormat(texture_info.pixel_format));
    push_attribute(EGL_DMA_BUF_PLANE0_FD_EXT, plane.fd);
    push_attribute(EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(plane.offset));
    push_attribute(EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(plane.stride));

    uint64_t modifier = 0;
    if (ParseModifier(texture_info.modifier, &modifier)) {
      push_attribute(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(modifier & 0xffffffffULL));
      push_attribute(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>((modifier >> 32U) & 0xffffffffULL));
    }

    attributes[attribute_index] = EGL_NONE;

    EGLImageKHR image = egl_create_image_khr_(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
    if (image == EGL_NO_IMAGE_KHR) {
      SetError(error_message, "Failed to import DMA-BUF into EGLImage: " + EglErrorString(eglGetError()));
      return false;
    }

    glViewport(0, 0, static_cast<GLsizei>(window_width_), static_cast<GLsizei>(window_height_));
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    gl_egl_image_target_texture_2d_oes_(GL_TEXTURE_2D, image);

    static constexpr GLfloat kVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kVertices);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kVertices + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glFinish();

    const EGLBoolean swap_result = eglSwapBuffers(egl_display_, egl_surface_);
    const EGLBoolean destroy_result = egl_destroy_image_khr_(egl_display_, image);
    if (swap_result != EGL_TRUE) {
      SetError(error_message, "Failed to swap mock preview buffers: " + EglErrorString(eglGetError()));
      return false;
    }

    if (destroy_result != EGL_TRUE) {
      SetError(error_message, "Failed to destroy imported EGLImage: " + EglErrorString(eglGetError()));
      return false;
    }

    if (error_message != nullptr) {
      error_message->clear();
    }

    return true;
  }

  bool SubmitSoftwareFrame(const SoftwareFrameInfo& frame_info, std::string* error_message) {
    if (!initialized_) {
      SetError(error_message, "Mock preview window is not initialized.");
      return false;
    }

    PumpEvents();
    if (closed_) {
      SetError(error_message, "Mock preview window was closed.");
      return false;
    }

    const size_t expected_size = static_cast<size_t>(frame_info.width) * static_cast<size_t>(frame_info.height) * 4U;
    if (frame_info.width == 0 || frame_info.height == 0 || frame_info.rgba_pixels.size() < expected_size) {
      SetError(error_message, "Software frame dimensions or RGBA data are invalid.");
      return false;
    }

    glViewport(0, 0, static_cast<GLsizei>(window_width_), static_cast<GLsizei>(window_height_));
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        static_cast<GLsizei>(frame_info.width),
        static_cast<GLsizei>(frame_info.height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        frame_info.rgba_pixels.data());

    static constexpr GLfloat kVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kVertices);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kVertices + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glFinish();

    if (eglSwapBuffers(egl_display_, egl_surface_) != EGL_TRUE) {
      SetError(error_message, "Failed to swap mock preview buffers: " + EglErrorString(eglGetError()));
      return false;
    }

    if (error_message != nullptr) {
      error_message->clear();
    }

    return true;
  }

  void Shutdown() {
    Cleanup();
  }

 private:
  void PumpEvents() {
    if (x_display_ == nullptr) {
      return;
    }

    while (XPending(x_display_) > 0) {
      XEvent event{};
      XNextEvent(x_display_, &event);
      if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == delete_window_atom_) {
        closed_ = true;
      }
    }
  }

  void Cleanup() {
    initialized_ = false;

    if (egl_display_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (texture_ != 0) {
      glDeleteTextures(1, &texture_);
      texture_ = 0;
    }

    if (program_ != 0) {
      glDeleteProgram(program_);
      program_ = 0;
    }

    if (egl_context_ != EGL_NO_CONTEXT && egl_display_ != EGL_NO_DISPLAY) {
      eglDestroyContext(egl_display_, egl_context_);
      egl_context_ = EGL_NO_CONTEXT;
    }

    if (egl_surface_ != EGL_NO_SURFACE && egl_display_ != EGL_NO_DISPLAY) {
      eglDestroySurface(egl_display_, egl_surface_);
      egl_surface_ = EGL_NO_SURFACE;
    }

    if (egl_display_ != EGL_NO_DISPLAY) {
      eglTerminate(egl_display_);
      egl_display_ = EGL_NO_DISPLAY;
    }

    if (x_window_ != 0 && x_display_ != nullptr) {
      XDestroyWindow(x_display_, x_window_);
      x_window_ = 0;
    }

    if (x_colormap_ != 0 && x_display_ != nullptr) {
      XFreeColormap(x_display_, x_colormap_);
      x_colormap_ = 0;
    }

    if (x_display_ != nullptr) {
      XCloseDisplay(x_display_);
      x_display_ = nullptr;
    }

    egl_create_image_khr_ = nullptr;
    egl_destroy_image_khr_ = nullptr;
    gl_egl_image_target_texture_2d_oes_ = nullptr;
    closed_ = false;
    window_width_ = 0;
    window_height_ = 0;
  }

  static void SetError(std::string* error_message, const std::string& message) {
    if (error_message != nullptr) {
      *error_message = message;
    }
  }

  Display* x_display_ = nullptr;
  Window x_window_ = 0;
  Colormap x_colormap_ = 0;
  Atom delete_window_atom_ = None;
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLConfig egl_config_ = nullptr;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  PFNEGLCREATEIMAGEKHRPROC egl_create_image_khr_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image_khr_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture_2d_oes_ = nullptr;
  GLuint program_ = 0;
  GLuint texture_ = 0;
  uint32_t window_width_ = 0;
  uint32_t window_height_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

LinuxMockPreviewWindow g_linux_mock_preview;

#endif

bool g_initialized = false;

}  // namespace

bool InitializeMockBackend(const InitializeOptions& options, std::string* error_message) {
  if (options.name.empty()) {
    if (error_message != nullptr) {
      *error_message = "Mock backend requires a non-empty overlay name.";
    }
    return false;
  }

#if defined(__linux__)
  if (!g_linux_mock_preview.Initialize(options, error_message)) {
    return false;
  }
#endif

  g_initialized = true;
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SubmitMockFrameWindows(uint64_t shared_handle, std::string* error_message) {
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "Mock backend is not initialized.";
    }
    return false;
  }

  if (shared_handle == 0) {
    if (error_message != nullptr) {
      *error_message = "Mock backend received an invalid Windows shared handle.";
    }
    return false;
  }

  if (error_message != nullptr) {
    *error_message = "Mock preview is not implemented on Windows yet.";
  }
  return false;
}

bool SubmitMockFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message) {
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "Mock backend is not initialized.";
    }
    return false;
  }

#if defined(__linux__)
  return g_linux_mock_preview.Submit(texture_info, error_message);
#else
  (void)texture_info;
  if (error_message != nullptr) {
    *error_message = "Mock preview is only implemented on Linux.";
  }
  return false;
#endif
}

bool SubmitMockSoftwareFrame(const SoftwareFrameInfo& frame_info, std::string* error_message) {
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "Mock backend is not initialized.";
    }
    return false;
  }

#if defined(__linux__)
  return g_linux_mock_preview.SubmitSoftwareFrame(frame_info, error_message);
#else
  (void)frame_info;
  if (error_message != nullptr) {
    *error_message = "Mock software preview is only implemented on Linux.";
  }
  return false;
#endif
}

bool SetMockPlacement(const OverlayPlacement& placement, std::string* error_message) {
  (void)placement;
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "Mock backend is not initialized.";
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SetMockVisible(bool visible, std::string* error_message) {
  (void)visible;
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "Mock backend is not initialized.";
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SetMockSizeMeters(float size_meters, std::string* error_message) {
  (void)size_meters;
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "Mock backend is not initialized.";
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

void ShutdownMockBackend() {
#if defined(__linux__)
  g_linux_mock_preview.Shutdown();
#endif
  g_initialized = false;
}

}  // namespace vrbridge
