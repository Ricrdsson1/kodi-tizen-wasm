set(SOURCES CPUInfoWasm.cpp
            GPUInfoWasm.cpp
            MemUtils.cpp
            PlatformWasm.cpp
            StaticAddons.cpp
            SystemInfoWasm.cpp
            TitanWebApis.cpp
            TizenWebApis.cpp
            WasmAudioWorkletManager.cpp
            WasmWebApis.cpp)

set(HEADERS CPUInfoWasm.h
            GPUInfoWasm.h
            PlatformWasm.h
            StaticAddons.h
            SystemInfoWasm.h
            TitanWebApis.h
            TizenWebApis.h
            WasmAudioWorkletManager.h
            WasmWebApis.h)

core_add_library(platform_wasm)
