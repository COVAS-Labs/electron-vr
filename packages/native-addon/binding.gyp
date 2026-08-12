{
  "variables": {
    "openvr_sdk_dir%": "<!(node -p \"process.env.OPENVR_SDK_DIR || require('node:path').resolve(process.cwd(), '..', '..', '.openvr-sdk')\")",
    "openxr_sdk_dir%": "<!(node -p \"process.env.OPENXR_SDK_DIR || require('node:path').resolve(process.cwd(), '..', '..', '.openxr-sdk')\")",
    "openxr_loader_dir%": "<!(node -p \"process.env.OPENXR_LOADER_DIR || require('node:path').resolve(process.cwd(), '..', '..', '.openxr-loader-build', 'src', 'loader')\")"
  },
  "targets": [
    {
      "target_name": "vr_bridge",
      "sources": [
        "native/src/addon.cc",
        "native/src/bridge.cc",
        "native/src/runtime_probe.cc",
        "native/src/mock_backend.cc",
        "native/src/openxr_backend.cc",
        "native/src/openxr_companion.cc",
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
        ],
        [
          "OS==\"mac\"",
          {
            "include_dirs": [
              "<(openxr_sdk_dir)/include"
            ],
            "sources!": [
              "native/src/openxr_backend.cc"
            ],
            "sources": [
              "native/src/openxr_backend_mac.mm"
            ],
            "defines": [
              "XR_USE_GRAPHICS_API_METAL"
            ],
            "libraries": [
              "<(openxr_loader_dir)/libopenxr_loader.dylib",
              "-framework Metal",
              "-framework IOSurface",
              "-framework Foundation"
            ],
            "xcode_settings": {
              "CLANG_ENABLE_OBJC_ARC": "YES",
              "MACOSX_DEPLOYMENT_TARGET": "12.4",
              "LD_RUNPATH_SEARCH_PATHS": [
                "@loader_path"
              ]
            }
          }
        ]
      ]
    }
  ],
  "conditions": [
    [
      "OS==\"win\"",
      {
        "targets": [
          {
            "target_name": "electron_vr_openxr_layer",
            "type": "shared_library",
            "product_prefix": "",
            "sources": [
              "native/openxr-api-layer/layer.cc",
              "native/openxr-api-layer/layer.def"
            ],
            "include_dirs": [
              "<(openxr_sdk_dir)/include"
            ],
            "defines": [
              "NOMINMAX",
              "WIN32_LEAN_AND_MEAN"
            ],
            "libraries": [
              "d3d11.lib",
              "d3d12.lib",
              "dxgi.lib"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": ["/std:c++17", "/W4", "/permissive-"]
              }
            },
            "copies": [
              {
                "destination": "<(PRODUCT_DIR)",
                "files": [
                  "native/openxr-api-layer/electron_vr_openxr_layer.json",
                  "native/openxr-api-layer/protocol.json"
                ]
              }
            ]
          },
          {
            "target_name": "electron_vr_openxr_layer_cli",
            "type": "executable",
            "sources": ["native/openxr-api-layer/registration_cli.cc"],
            "defines": ["NOMINMAX", "WIN32_LEAN_AND_MEAN"],
            "libraries": ["advapi32.lib"],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": ["/std:c++17", "/W4", "/permissive-"]
              }
            }
          },
          {
            "target_name": "electron_vr_openxr_layer_test",
            "type": "executable",
            "dependencies": ["electron_vr_openxr_layer"],
            "sources": ["native/openxr-api-layer/layer_test.cc"],
            "include_dirs": ["<(openxr_sdk_dir)/include"],
            "defines": ["NOMINMAX", "WIN32_LEAN_AND_MEAN"],
            "libraries": ["d3d12.lib", "dxgi.lib"],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": ["/std:c++17", "/W4", "/permissive-"]
              }
            }
          }
        ]
      }
    ]
  ]
}
