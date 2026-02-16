{
  "targets": [
    {
      "target_name": "dtpipe",
      "sources": ["src/addon.cc"],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "../libdtpipe/include"
      ],
      "libraries": [
        "<(module_root_dir)/../libdtpipe/build-release/src/libdtpipe.dylib"
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "11.0",
        "OTHER_LDFLAGS": ["-Wl,-rpath,@loader_path/../../../libdtpipe/build-release/src"]
      },
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
      "conditions": [
        ["OS=='mac'", {
          "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"]
        }],
        ["OS=='linux'", {
          "ldflags": ["-Wl,-rpath,'$$ORIGIN/../../libdtpipe/build-release/src'"]
        }]
      ]
    }
  ]
}
