# Samsung Tizen TV packaging
#
# A TV only runs .NET and web apps, so the package is a small .NET app (tools/tizen/packaging)
# that copies Kodi out of its read-only package into its data directory and runs it there:
#   lib/kodi.bin          Kodi
#   lib/python3.x/        Python's standard library (PYTHONHOME is the package)
#   lib/kodi/addons/      binary add-ons (KODI_BIN_HOME)
#   res/kodi/             addons, media, system, userdata (KODI_HOME)
#
# make bundle  - assembles the package directory
# make tpk     - builds and signs the .tpk with the Tizen Studio security profile
#                TIZEN_SECURITY_PROFILE (default: the active one)
#
# With TIZEN_ADDON_REPOSITORY_URL, Kodi comes with the add-on repository there
# (tools/tizen/repository), from which its binary add-ons are installed.
#
# Development builds can add, through the environment of "make bundle":
#   KODI_LOG_URL         address the launcher posts Kodi's log to
#   KODI_ARGS            arguments Kodi is started with, one per line
#   KODI_DEV_WEBSERVER   password of Kodi's web server, enabled on port 8095 of a new profile

find_program(DOTNET_EXECUTABLE dotnet HINTS ENV DOTNET_ROOT)
find_program(TIZEN_CLI tizen HINTS $ENV{HOME}/tizen-studio/tools/ide/bin)
set(TIZEN_SECURITY_PROFILE "" CACHE STRING "Tizen Studio security profile the .tpk is signed with")
set(TIZEN_ADDON_REPOSITORY_URL "" CACHE STRING "URL of the add-on repository for Tizen that Kodi comes with")
set(TIZEN_BUNDLED_ADDONS "inputstream.adaptive;inputstream.ffmpegdirect" CACHE STRING
    "Binary add-ons of the depends that Kodi comes with")

set(APP_PACKAGE_DIR ${CMAKE_BINARY_DIR}/tools/tizen/packaging)
set(TIZEN_MANIFEST ${CMAKE_BINARY_DIR}/tools/tizen/tizen-manifest.xml)
configure_file(${CMAKE_SOURCE_DIR}/tools/tizen/packaging/tizen-manifest.xml.in ${TIZEN_MANIFEST} @ONLY)

set(APP_INSTALL_DIRS ${CMAKE_BINARY_DIR}/addons
                     ${CMAKE_BINARY_DIR}/media
                     ${CMAKE_BINARY_DIR}/system
                     ${CMAKE_BINARY_DIR}/userdata)
# Libraries Kodi links that a TV does not have
set(APP_EXTRA_LIBS ${DEPENDS_PATH}/lib/libshairplay.so.0
                   ${TOOLCHAIN}/${HOST}/lib/libatomic.so.1)
string(REPLACE ";" "|" APP_INSTALL_DIRS "${APP_INSTALL_DIRS}")
string(REPLACE ";" "|" APP_EXTRA_LIBS "${APP_EXTRA_LIBS}")
string(REPLACE ";" "|" TIZEN_BUNDLED_ADDONS_ARG "${TIZEN_BUNDLED_ADDONS}")

add_custom_target(bundle
  DEPENDS ${APP_NAME_LC}
  COMMAND ${CMAKE_COMMAND} -DAPP_PACKAGE_DIR=${APP_PACKAGE_DIR}
                           -DPACKAGING_SOURCE=${CMAKE_SOURCE_DIR}/tools/tizen/packaging
                           -DMANIFEST=${TIZEN_MANIFEST}
                           -DBINARY=${CMAKE_BINARY_DIR}/${APP_BINARY}
                           -DSTRIP=${CMAKE_STRIP}
                           -DDEPENDS_PATH=${DEPENDS_PATH}
                           -DPYTHON_VERSION=${PYTHON_VERSION}
                           -DHOST_PYTHON=${NATIVEPREFIX}/bin/python3
                           -DADDON_REPOSITORY_URL=${TIZEN_ADDON_REPOSITORY_URL}
                           -DADDON_REPOSITORY_TEMPLATE=${CMAKE_SOURCE_DIR}/tools/tizen/repository/repository.kodi-tizen
                           "-DBUNDLED_ADDONS=${TIZEN_BUNDLED_ADDONS_ARG}"
                           "-DINSTALL_DIRS=${APP_INSTALL_DIRS}"
                           "-DEXTRA_LIBS=${APP_EXTRA_LIBS}"
                           -P ${CMAKE_SOURCE_DIR}/cmake/scripts/tizen/Bundle.cmake
  VERBATIM
)

if(TIZEN_SECURITY_PROFILE)
  set(TIZEN_SIGN_ARGS -s ${TIZEN_SECURITY_PROFILE})
endif()
set(APP_TPK ${CMAKE_BINARY_DIR}/${APP_NAME_LC}-${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}.tpk)
add_custom_target(tpk
  DEPENDS bundle
  COMMAND ${DOTNET_EXECUTABLE} build -c Release -nologo -v q
  COMMAND ${CMAKE_COMMAND} -DPATTERN=${APP_PACKAGE_DIR}/bin/Release/tizen40/*.tpk -DTARGET=${APP_TPK}
                           -P ${CMAKE_SOURCE_DIR}/cmake/scripts/tizen/CopyTpk.cmake
  COMMAND ${TIZEN_CLI} package -t tpk ${TIZEN_SIGN_ARGS} -- ${APP_TPK}
  WORKING_DIRECTORY ${APP_PACKAGE_DIR}
  VERBATIM
)
