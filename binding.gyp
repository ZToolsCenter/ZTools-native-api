{
  "targets": [
    {
      "target_name": "ztools_native",
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
              "OTHER_CFLAGS": ["-arch x86_64", "-arch arm64"],
              "OTHER_CPLUSPLUSFLAGS": ["-arch x86_64", "-arch arm64"],
              "OTHER_LDFLAGS": ["-arch x86_64", "-arch arm64"]
            },
            "libraries": ["-framework Cocoa"]
          }
        ],
        [
          "OS=='win'",
          {
            "include_dirs": ["src/screenshot/algo"],
            "sources": [
              "src/binding_windows.cpp",
              "src/screenshot/windows/capture_windows.cpp",
              "src/screenshot/windows/icons_windows.cpp",
              "src/screenshot/windows/overlay_ui_windows.cpp",
              "src/screenshot/windows/overlay_paint_windows.cpp",
              "src/screenshot/windows/overlay_input_windows.cpp",
              "src/screenshot/windows/annotations_windows.cpp",
              "src/screenshot/windows/mosaic_windows.cpp",
              "src/screenshot/windows/output_windows.cpp",
              "src/screenshot/algo/lc_match_core.cpp",
              "src/screenshot/algo/lc_stitch_state.cpp",
              "src/screenshot/windows/lc_frame_io_windows.cpp",
              "src/screenshot/windows/lc_panel_ui_windows.cpp",
              "src/screenshot/windows/lc_toolbar_ui_windows.cpp",
              "src/screenshot/windows/lc_session_windows.cpp",
              "src/screenshot/windows/wndproc_windows.cpp",
              "src/screenshot/windows/session_windows.cpp"
            ],
            "defines": ["_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS"],
            "libraries": [
              "user32.lib",
              "kernel32.lib",
              "psapi.lib",
              "shell32.lib",
              "shlwapi.lib",
              "ole32.lib",
              "oleaut32.lib",
              "uiautomationcore.lib",
              "gdiplus.lib",
              "dwmapi.lib",
              "gdi32.lib",
              "imm32.lib",
              "windowsapp.lib"
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
