set(SOURCES CPUInfoWasm.cpp
            GPUInfoWasm.cpp
            MemUtils.cpp
            PlatformWasm.cpp
            StaticAddons.cpp
            SystemInfoWasm.cpp
            WasmAudioWorkletManager.cpp)

set(HEADERS CPUInfoWasm.h
            GPUInfoWasm.h
            PlatformWasm.h
            StaticAddons.h
            SystemInfoWasm.h
            WasmAudioWorkletManager.h)

core_add_library(platform_wasm)
