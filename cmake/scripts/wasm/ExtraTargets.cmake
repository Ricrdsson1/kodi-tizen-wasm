# Emscripten reads these at link time; without this a change to them does not relink.
set_property(TARGET ${APP_NAME_LC} APPEND PROPERTY LINK_DEPENDS
  ${CMAKE_SOURCE_DIR}/xbmc/platform/wasm/kodi_pre.js
  ${CMAKE_SOURCE_DIR}/xbmc/windowing/wasm/webgl_commit.js
  ${CMAKE_SOURCE_DIR}/xbmc/platform/wasm/tizen/tizen_emss.js)
if(ENABLE_WASM_DEV_PROXY)
  set_property(TARGET ${APP_NAME_LC} APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_SOURCE_DIR}/tools/wasm/dev_proxy_pre.js)
endif()

# CPython is linked statically, so it has no install prefix to find on a real
# filesystem. Ship its standard library inside the Emscripten VFS as the zip
# landmark getpath() looks for at <PYTHONHOME>/lib/pythonXY.zip; XBPython points
# PYTHONHOME at special://xbmc, which is WASM_VFS_PREFIX here.
if(TARGET ${APP_NAME_LC}::Python)
  string(REPLACE "." "" PYTHON_ZIP_VERSION "${PYTHON_VERSION}")
  set(PYTHON_STDLIB_DIR "${DEPENDS_PATH}/lib/python${PYTHON_VERSION}")
  set(PYTHON_STDLIB_VFS_DIR "${CMAKE_BINARY_DIR}/python-stdlib/lib")
  set(PYTHON_STDLIB_ZIP "${PYTHON_STDLIB_VFS_DIR}/python${PYTHON_ZIP_VERSION}.zip")

  file(GLOB_RECURSE PYTHON_STDLIB_SOURCES CONFIGURE_DEPENDS "${PYTHON_STDLIB_DIR}/*.py")

  add_custom_command(OUTPUT "${PYTHON_STDLIB_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYTHON_STDLIB_VFS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${PYTHON_STDLIB_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${PYTHON_STDLIB_DIR}"
            ${CMAKE_COMMAND} -E tar cf "${PYTHON_STDLIB_ZIP}" --format=zip .
    DEPENDS ${PYTHON_STDLIB_SOURCES}
    COMMENT "Packing the Python standard library for the WASM virtual filesystem"
    VERBATIM)

  add_custom_target(python-stdlib-zip DEPENDS "${PYTHON_STDLIB_ZIP}")
  set_target_properties(python-stdlib-zip PROPERTIES FOLDER "Build Utilities")
  add_dependencies(${APP_NAME_LC} python-stdlib-zip)

  target_link_options(${APP_NAME_LC} PRIVATE
    "SHELL:--preload-file ${PYTHON_STDLIB_VFS_DIR}@${WASM_VFS_PREFIX}/lib")
  set_property(TARGET ${APP_NAME_LC} APPEND PROPERTY LINK_DEPENDS "${PYTHON_STDLIB_ZIP}")
endif()

# A browser cannot load a shared library at run time, so the binary add-ons whose
# source directories are listed in WASM_STATIC_ADDONS are linked into the
# executable. xbmc/platform/wasm/StaticAddons.h describes the table generated here.
set(WASM_STATIC_ADDONS "" CACHE STRING "Source directories of binary add-ons to link statically")
set(WASM_STATIC_ADDON_LIBRARIES "" CACHE STRING "Libraries from the depends prefix those add-ons need")
set(WASM_STATIC_ADDON_DEFINITIONS "" CACHE STRING "Compile definitions those add-ons need")

set(_static_addon_declarations "")
set(_static_addon_rows "")
foreach(_dir ${WASM_STATIC_ADDONS})
  get_filename_component(_dir "${_dir}" ABSOLUTE)
  get_filename_component(_id "${_dir}" NAME)
  string(MAKE_C_IDENTIFIER "${_id}" _prefix)

  file(READ "${_dir}/${_id}/addon.xml.in" _addon_xml)

  file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${_dir}/src/*.cpp" "${_dir}/src/*.c")
  add_library(${_id} STATIC ${_sources})
  # The add-on is not Kodi code and is not held to its warning set.
  set_target_properties(${_id} PROPERTIES COMPILE_OPTIONS "" FOLDER "Static Addons")
  target_include_directories(${_id} PRIVATE "${_dir}/src"
                                            "${CMAKE_SOURCE_DIR}/xbmc/addons/kodi-dev-kit/include"
                                            "${DEPENDS_PATH}/include")
  target_compile_definitions(${_id} PRIVATE ADDON_Create=${_prefix}_ADDON_Create
                                            ADDON_GetTypeVersion=${_prefix}_ADDON_GetTypeVersion
                                            ADDON_GetTypeMinVersion=${_prefix}_ADDON_GetTypeMinVersion
                                            ${WASM_STATIC_ADDON_DEFINITIONS})
  target_link_libraries(${APP_NAME_LC} ${_id})

  # Kodi only starts an add-on whose library file exists.
  set(_addon_data "${CMAKE_BINARY_DIR}/addons/${_id}")
  file(COPY "${_dir}/${_id}/" DESTINATION "${_addon_data}" PATTERN "addon.xml.in" EXCLUDE)
  file(WRITE "${_addon_data}/${_id}.wasm" "")
  string(REPLACE "@ADDON_DEPENDS@" "" _addon_xml "${_addon_xml}")
  string(REPLACE "@PLATFORM@" "wasm" _addon_xml "${_addon_xml}")
  string(REPLACE "@LIBRARY_FILENAME@" "${_id}.wasm" _addon_xml "${_addon_xml}")
  # Other binary add-ons are not part of this build.
  string(REGEX REPLACE "[ \t]*<import addon=\"inputstream\\.[^>]*>[ \t]*\r?\n" "" _addon_xml "${_addon_xml}")
  file(WRITE "${_addon_data}/addon.xml" "${_addon_xml}")

  string(APPEND _static_addon_declarations
         "int ${_prefix}_ADDON_Create(void*);\n"
         "const char* ${_prefix}_ADDON_GetTypeVersion(int);\n"
         "const char* ${_prefix}_ADDON_GetTypeMinVersion(int);\n")
  string(APPEND _static_addon_rows
         "    {\"${_id}.wasm\", reinterpret_cast<void*>(${_prefix}_ADDON_Create),\n"
         "     reinterpret_cast<void*>(${_prefix}_ADDON_GetTypeVersion),\n"
         "     reinterpret_cast<void*>(${_prefix}_ADDON_GetTypeMinVersion)},\n")
  message(STATUS "WASM: linking binary add-on ${_id} statically")
endforeach()

foreach(_library ${WASM_STATIC_ADDON_LIBRARIES})
  target_link_libraries(${APP_NAME_LC} "${DEPENDS_PATH}/lib/lib${_library}.a")
endforeach()

set(_static_addon_table "${CMAKE_BINARY_DIR}/StaticAddonsTable.cpp")
file(CONFIGURE OUTPUT "${_static_addon_table}" CONTENT
"#include \"platform/wasm/StaticAddons.h\"

extern \"C\"
{
${_static_addon_declarations}}

const StaticAddonLibrary g_staticAddonLibraries[] = {
${_static_addon_rows}    {nullptr, nullptr, nullptr, nullptr},
};
" @ONLY)
target_sources(${APP_NAME_LC} PRIVATE "${_static_addon_table}")
