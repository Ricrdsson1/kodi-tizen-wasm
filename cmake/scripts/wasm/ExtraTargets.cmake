# Emscripten reads these at link time; without this a change to them does not relink.
set_property(TARGET ${APP_NAME_LC} APPEND PROPERTY LINK_DEPENDS
  ${CMAKE_SOURCE_DIR}/xbmc/platform/wasm/kodi_pre.js
  ${CMAKE_SOURCE_DIR}/xbmc/windowing/wasm/webgl_commit.js
  ${CMAKE_SOURCE_DIR}/xbmc/platform/wasm/tizen/tizen_emss.js
  ${CMAKE_SOURCE_DIR}/xbmc/platform/wasm/tizen/tizen_drm.js
  ${CMAKE_SOURCE_DIR}/xbmc/platform/wasm/player/web_player.js
  ${CMAKE_SOURCE_DIR}/xbmc/platform/wasm/network/tizen_sockets.js)
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
#
# Per add-on:
#   WASM_STATIC_ADDON_LIBRARIES_<id>    libraries it needs from the depends prefix
#   WASM_STATIC_ADDON_DEFINITIONS_<id>  compile definitions its own build would set
#   WASM_STATIC_ADDON_SOURCES_<id>      source globs, relative to its directory or
#                                       absolute (default: everything below src)
#   WASM_STATIC_ADDON_EXCLUDE_<id>      regular expression of sources to leave out
#   WASM_STATIC_ADDON_INCLUDES_<id>     include directories, relative to its
#                                       directory or absolute, in addition to src
#   WASM_STATIC_ADDON_TEMPLATE_<id>     NAME=value pairs for the @NAME@ variables
#                                       of the *.in files among its resources
set(WASM_STATIC_ADDONS "" CACHE STRING "Source directories of binary add-ons to link statically")
# All of the data is loaded into memory at start, and translations are most of an
# add-on's. English is always kept, as it is what Kodi falls back to.
set(WASM_STATIC_ADDON_LANGUAGES "" CACHE STRING
    "Languages of static add-ons to ship, e.g. cs_cz;de_de (default: all)")
find_program(WASM_PYTHON3 python3 HINTS "${NATIVEPREFIX}/bin" REQUIRED)

set(_static_addon_declarations "")
set(_static_addon_rows "")
foreach(_dir ${WASM_STATIC_ADDONS})
  get_filename_component(_dir "${_dir}" ABSOLUTE)
  get_filename_component(_id "${_dir}" NAME)
  string(MAKE_C_IDENTIFIER "${_id}" _prefix)

  file(READ "${_dir}/${_id}/addon.xml.in" _addon_xml)

  set(_globs ${WASM_STATIC_ADDON_SOURCES_${_id}})
  if(NOT _globs)
    set(_globs "src/*.cpp" "src/*.c")
  endif()
  # Relative to the add-on, unless they point elsewhere.
  list(TRANSFORM _globs PREPEND "${_dir}/" REGEX "^[^/]")
  file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ${_globs})
  if(WASM_STATIC_ADDON_EXCLUDE_${_id})
    list(FILTER _sources EXCLUDE REGEX "${WASM_STATIC_ADDON_EXCLUDE_${_id}}")
  endif()
  set(_includes ${WASM_STATIC_ADDON_INCLUDES_${_id}})
  list(TRANSFORM _includes PREPEND "${_dir}/" REGEX "^[^/]")
  add_library(${_id} STATIC ${_sources})
  # The add-on is not Kodi code: it is not held to Kodi's warning set, and must
  # find its own headers where Kodi has some of the same name.
  set_target_properties(${_id} PROPERTIES COMPILE_OPTIONS "$<$<BOOL:${WASM_OPTIMIZE_SIZE}>:-Os>" INCLUDE_DIRECTORIES ""
                                          FOLDER "Static Addons")
  target_include_directories(${_id} PRIVATE "${_dir}/src" ${_includes}
                                            "${CMAKE_SOURCE_DIR}/tools/wasm/addon-compat"
                                            "${CMAKE_SOURCE_DIR}/xbmc/addons/kodi-dev-kit/include"
                                            "${DEPENDS_PATH}/include")
  target_compile_definitions(${_id} PRIVATE ADDON_Create=${_prefix}_ADDON_Create
                                            ADDON_GetTypeVersion=${_prefix}_ADDON_GetTypeVersion
                                            ADDON_GetTypeMinVersion=${_prefix}_ADDON_GetTypeMinVersion
                                            # What add-ons take Emscripten's POSIX for.
                                            TARGET_POSIX TARGET_LINUX
                                            ${WASM_STATIC_ADDON_DEFINITIONS_${_id}})

  # Every add-on carries its own copy of the add-on development kit and of the
  # libraries it uses. Merged into one object in which only the entry points stay
  # global, the copies of two add-ons cannot collide.
  set(_libraries "")
  foreach(_library ${WASM_STATIC_ADDON_LIBRARIES_${_id}})
    list(APPEND _libraries "${DEPENDS_PATH}/lib/lib${_library}.a")
  endforeach()
  set(_merged "${CMAKE_BINARY_DIR}/static-addons/${_id}.merged.o")
  set(_isolated "${CMAKE_BINARY_DIR}/static-addons/${_id}.o")
  add_custom_command(OUTPUT "${_isolated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/static-addons"
    COMMAND ${CMAKE_LINKER} -r --whole-archive $<TARGET_FILE:${_id}> --no-whole-archive
            ${_libraries} -o "${_merged}"
    COMMAND ${WASM_PYTHON3} "${CMAKE_SOURCE_DIR}/tools/wasm/localize_symbols.py"
            "${_merged}" "${_isolated}" ${_prefix}_ADDON_Create
            ${_prefix}_ADDON_GetTypeVersion ${_prefix}_ADDON_GetTypeMinVersion
    DEPENDS ${_id} ${_libraries} "${CMAKE_SOURCE_DIR}/tools/wasm/localize_symbols.py"
    COMMENT "Isolating the symbols of ${_id}"
    VERBATIM)
  set_source_files_properties("${_isolated}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  target_sources(${APP_NAME_LC} PRIVATE "${_isolated}")

  # Kodi only starts an add-on whose library file exists.
  set(_addon_data "${CMAKE_BINARY_DIR}/addons/${_id}")
  file(REMOVE_RECURSE "${_addon_data}")
  file(COPY "${_dir}/${_id}/" DESTINATION "${_addon_data}" PATTERN "addon.xml.in" EXCLUDE)
  if(WASM_STATIC_ADDON_LANGUAGES)
    file(GLOB _languages "${_addon_data}/resources/language/resource.language.*")
    foreach(_language ${_languages})
      string(REGEX REPLACE ".*resource\\.language\\." "" _code "${_language}")
      if(NOT _code STREQUAL "en_gb" AND NOT _code IN_LIST WASM_STATIC_ADDON_LANGUAGES)
        file(REMOVE_RECURSE "${_language}")
      endif()
    endforeach()
  endif()
  file(WRITE "${_addon_data}/${_id}.wasm" "")
  # Templates the add-on's own build would fill in. Variables that are not given
  # a value in WASM_STATIC_ADDON_TEMPLATE_<id> become empty.
  file(GLOB_RECURSE _templates "${_addon_data}/*.in")
  foreach(_template ${_templates})
    file(READ "${_template}" _content)
    foreach(_variable ${WASM_STATIC_ADDON_TEMPLATE_${_id}})
      string(REGEX MATCH "^([^=]+)=(.*)$" _unused "${_variable}")
      string(REPLACE "@${CMAKE_MATCH_1}@" "${CMAKE_MATCH_2}" _content "${_content}")
    endforeach()
    string(REGEX REPLACE "@[A-Za-z0-9_]+@" "" _content "${_content}")
    string(REGEX REPLACE "\\.in$" "" _output "${_template}")
    file(WRITE "${_output}" "${_content}")
    file(REMOVE "${_template}")
    # The data is packed when linking, which does not see it change by itself.
    set_property(TARGET ${APP_NAME_LC} APPEND PROPERTY LINK_DEPENDS "${_output}")
  endforeach()
  string(REPLACE "@ADDON_DEPENDS@" "" _addon_xml "${_addon_xml}")
  string(REPLACE "@PLATFORM@" "wasm" _addon_xml "${_addon_xml}")
  string(REPLACE "@LIBRARY_FILENAME@" "${_id}.wasm" _addon_xml "${_addon_xml}")
  # Other binary add-ons are not part of this build.
  string(REGEX REPLACE "[ \t]*<import addon=\"inputstream\\.[^>]*>[ \t]*\r?\n" "" _addon_xml "${_addon_xml}")
  file(WRITE "${_addon_data}/addon.xml" "${_addon_xml}")
  set_property(TARGET ${APP_NAME_LC} APPEND PROPERTY LINK_DEPENDS "${_addon_data}/addon.xml")

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
