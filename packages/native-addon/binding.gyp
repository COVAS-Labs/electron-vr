{
  "targets": [
    {
      "target_name": "vr_bridge",
      "sources": [
        "native/src/addon.cc",
        "native/src/bridge.cc",
        "native/src/runtime_probe.cc",
        "native/src/mock_backend.cc",
        "native/src/openxr_backend.cc",
        "native/src/openvr_backend.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
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
            "libraries": [
              "-lX11",
              "-lEGL",
              "-lGLESv2",
              "-ldl"
            ]
          }
        ],
        [
          "OS==\"win\"",
          {
            "defines": [
              "NOMINMAX",
              "WIN32_LEAN_AND_MEAN"
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
