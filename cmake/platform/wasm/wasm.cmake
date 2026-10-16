# Emscripten WebAssembly platform — GLES maps to WebGL2
set(PLATFORM_REQUIRED_DEPS OpenGLES EGL)
set(PLATFORM_OPTIONAL_DEPS_EXCLUDE CEC)
# smb:// through libsmb2, as libsmbclient (Samba) does not build for WebAssembly
set(PLATFORM_OPTIONAL_DEPS SMB2)
set(APP_RENDER_SYSTEM gles)