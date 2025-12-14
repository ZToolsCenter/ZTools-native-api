{
  "targets": [
    {
      "target_name": "ztools_event_hook",
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "conditions": [
        [
          "OS=='mac'",
          {
            "sources": ["src/binding_mac.cpp"],
            "xcode_settings": {
              "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
              "CLANG_CXX_LIBRARY": "libc++",
              "MACOSX_DEPLOYMENT_TARGET": "10.15",
              "OTHER_CFLAGS": ["-arch x86_64", "-arch arm64"]
            },
            "libraries": [
              "-framework Cocoa",
              "-framework ApplicationServices",
              "-framework CoreGraphics"
            ]
          }
        ],
        [
          "OS=='win'",
          {
            "sources": ["src/binding_windows.cpp"],
            "libraries": [
              "user32.lib",
              "kernel32.lib"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": ["/std:c++17", "/utf-8"]
              }
            }
          }
        ]
      ]
    }
  ]
}

