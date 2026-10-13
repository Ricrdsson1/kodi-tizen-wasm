#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Derives the static add-on build options (see cmake/scripts/wasm/ExtraTargets.cmake)
for binary add-ons, from what their own CMakeLists.txt asks for.

Usage: static_addons.py <depends-prefix> <add-on source dir>...

Prints one CMake argument per line, ending with -DWASM_STATIC_ADDONS=<the directories>.
"""

import os
import re
import sys

EXTRA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'addons')

# Libraries of the depends prefix by the name an add-on finds them with.
LIBRARIES = {
    'jsoncpp': ['jsoncpp'],
    'tinyxml2': ['tinyxml2'],
    'tinyxml': ['tinyxml'],
    'pugixml': ['pugixml'],
    'zlib': ['z'],
    'lzma': ['lzma'],
    'openssl': ['ssl', 'crypto'],
    'libxml2': ['xml2', 'lzma', 'z', 'iconv'],
    'sqlite3': ['sqlite3'],
    'hdhomerun': ['hdhomerun'],
}

# What cannot be told from an add-on's CMakeLists.txt.
OVERRIDES = {
    'pvr.hdhomerun': {'includes': ['@DEPENDS@/include/hdhomerun']},
    'pvr.hts': {'exclude': r'net_winsock\.c'},
    'pvr.mediaportal.tvserver': {'definitions': ['SOCKLEN_T=socklen_t']},
    # lib/cppmyth generates a configuration header.
    'pvr.mythtv': {'includes': ['@EXTRA@/pvr.mythtv'], 'libraries': ['z']},
    # A source file its own build leaves out.
    'pvr.vbox': {'exclude': r'timeshift/DummyBuffer\.cpp'},
    'pvr.vdr.vnsi': {'definitions': ['HAS_GLES=3']},
}

SKIPPED_DIRECTORIES = re.compile(
    r'/(test|tests|windows|win32|depends|debian|doc|docs|example|examples)(/|$)', re.I)
WINDOWS_DEFINITION = re.compile(r'WIN32|_WINSOCK|_CRT_|UNICODE|TARGET_WINDOWS|_USE_32BIT|NOMINMAX|_WIN')


def options(directory, depends):
    addon = os.path.basename(directory)
    cmake = open(os.path.join(directory, 'CMakeLists.txt')).read()
    manifest = open(os.path.join(directory, addon, 'addon.xml.in')).read()
    version = re.search(r'<addon[^>]*?\sversion="([^"]+)"', manifest, re.S).group(1)
    override = OVERRIDES.get(addon, {})

    libraries = []
    packages = [name.lower() for name in re.findall(r'find_package\(([A-Za-z0-9_+-]+)', cmake)]
    for package in packages:
        libraries += [lib for lib in LIBRARIES.get(package, []) if lib not in libraries]
    libraries += [lib for lib in override.get('libraries', []) if lib not in libraries]

    # Every directory with headers, as add-ons include theirs by bare name.
    includes = set()
    for base in ('lib', 'src'):
        for current, children, files in os.walk(os.path.join(directory, base)):
            relative = os.path.relpath(current, directory)
            if SKIPPED_DIRECTORIES.search('/' + relative + '/'):
                children[:] = []
                continue
            if relative != 'src' and any(name.endswith(('.h', '.hpp', '.hh')) for name in files):
                includes.add(relative)
    if os.path.isdir(os.path.join(directory, 'lib')):
        includes.add('lib')
    if 'libxml2' in packages:
        includes.add(os.path.join(depends, 'include/libxml2'))
    for include in override.get('includes', []):
        includes.add(include.replace('@EXTRA@', EXTRA).replace('@DEPENDS@', depends))

    definitions = []
    for arguments in re.findall(r'add_definitions\(([^)]*)\)', cmake):
        for argument in arguments.split():
            argument = argument.strip('"')
            if not argument.startswith('-D') or WINDOWS_DEFINITION.search(argument):
                continue
            definition = re.sub(r'\$\{[A-Za-z0-9_]*VERSION[A-Za-z0-9_]*\}', version, argument[2:])
            if '${' not in definition and definition not in definitions:
                definitions.append(definition)
    definitions += override.get('definitions', [])

    exclude = '/(test|tests|windows|win32|depends|example|examples)/'
    if 'exclude' in override:
        exclude += '|' + override['exclude']

    return [
        f'-DWASM_STATIC_ADDON_SOURCES_{addon}=src/*.cpp;src/*.c;lib/*.cpp;lib/*.c;lib/*.cc',
        f'-DWASM_STATIC_ADDON_EXCLUDE_{addon}={exclude}',
        f'-DWASM_STATIC_ADDON_INCLUDES_{addon}=' + ';'.join(sorted(includes)),
        f'-DWASM_STATIC_ADDON_LIBRARIES_{addon}=' + ';'.join(libraries),
        f'-DWASM_STATIC_ADDON_DEFINITIONS_{addon}=' + ';'.join(definitions),
    ]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)

    depends = os.path.abspath(sys.argv[1])
    directories = [os.path.abspath(directory) for directory in sys.argv[2:]]
    for directory in directories:
        print('\n'.join(options(directory, depends)))
    print('-DWASM_STATIC_ADDONS=' + ';'.join(directories))


if __name__ == '__main__':
    main()
