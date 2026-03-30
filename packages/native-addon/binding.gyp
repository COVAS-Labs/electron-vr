{
  "variables": {
    "openvr_sdk_dir%": "<!(node -p \"process.env.OPENVR_SDK_DIR || require('node:path').resolve(process.cwd(), '..', '..', '.openvr-sdk')\")",
    "openxr_sdk_dir%": "<!(node -p \"process.env.OPENXR_SDK_DIR || require('node:path').resolve(process.cwd(), '..', '..', '.openxr-sdk')\")"
  },
  "targets": [
    {
      "target_name": "vr_bridge",
      "sources": [
        "native/src/addon.cc",
        "native/src/bridge.cc",
        "native/src/curved_geometry.cc",
        "native/src/runtime_probe.cc",
        "native/src/mock_backend.cc",
        "native/src/openxr_backend.cc",
        "native/src/openvr_backend.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "<(openvr_sdk_dir)/headers"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS"
      ],
      "cflags!": [
        "-fno-exceptions"
      ],
      "cflags_cc!": [
        "-fno-exceptions"
      ],
      "cflags_cc": [
        "-std=c++17"
      ],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "10.15"
      },
      "conditions": [
        [
          "OS==\"linux\"",
          {
            "defines": [
              "XR_USE_PLATFORM_EGL",
              "XR_USE_GRAPHICS_API_OPENGL_ES"
            ],
            "ldflags": [
              "-Wl,-z,origin",
              "-Wl,-rpath,\\$$ORIGIN"
            ],
            "libraries": [
              "-L<(openvr_sdk_dir)/lib/linux64",
              "-lopenxr_loader",
              "-lopenvr_api",
              "-lEGL",
              "-lGLESv2",
              "-ldl"
            ]
          }
        ],
        [
          "OS==\"win\"",
          {
            "include_dirs": [
              "<(openxr_sdk_dir)/include"
            ],
            "defines": [
              "NOMINMAX",
              "WIN32_LEAN_AND_MEAN"
            ],
            "libraries": [
              "<(openvr_sdk_dir)/lib/win64/openvr_api.lib",
              "d3d11.lib",
              "dxgi.lib"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": [
                  "/std:c++17"
                ]
              }
            }
          }
        ]
      ]
    }
  ]
}
