set(SOURCES AddonPythonInvoker.cpp
            CallbackHandler.cpp
            ContextItemAddonInvoker.cpp
            LanguageHook.cpp
            PythonInvoker.cpp
            PythonToCppException.cpp
            XBPython.cpp
            PyContext.cpp)

set(HEADERS AddonPythonInvoker.h
            CallbackHandler.h
            ContextItemAddonInvoker.h
            LanguageHook.h
            PyContext.h
            PythonInvoker.h
            PythonToCppException.h
            pythreadstate.h
            XBPython.h)

core_add_library(python_interface)

# The Python bindings convert arguments by trying types and catching what fails, so they
# need C++ exceptions, which the WASM build otherwise leaves out.
if(CORE_SYSTEM_NAME STREQUAL wasm)
  target_compile_options(python_interface PRIVATE "SHELL:-sDISABLE_EXCEPTION_CATCHING=0")
endif()
