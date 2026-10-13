set(SOURCES CPUInfoWasm.cpp
            GPUInfoWasm.cpp
            MemUtils.cpp
            PlatformWasm.cpp
            StaticAddons.cpp
            WasmAudioWorkletManager.cpp)

set(HEADERS CPUInfoWasm.h
            GPUInfoWasm.h
            PlatformWasm.h
            StaticAddons.h
            WasmAudioWorkletManager.h)

core_add_library(platform_wasm)
