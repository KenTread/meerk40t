# SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
# Copyright (c) 2026 KenTread. All Rights Reserved.
# Proprietary and confidential -- see LICENSE for terms.
#
# GenerateNotices.cmake -- regenerates THIRD-PARTY-NOTICES.md from
# cmake/third-party.json for the components actually enabled in this
# build.
#
# Why generate rather than hand-maintain: attribution for the MIT, BSD,
# and Apache-2.0 components is a binding license obligation. A
# hand-written file drifts the moment a dependency is added or a version
# bumped, and the drift is invisible until someone audits it. Generating
# it makes the build fail loudly if the inventory is malformed instead.
#
# Run standalone with:
#   cmake -DINPUT_JSON=cmake/third-party.json \
#         -DOUTPUT_FILE=THIRD-PARTY-NOTICES.md -P cmake/GenerateNotices.cmake

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED INPUT_JSON OR NOT EXISTS "${INPUT_JSON}")
    message(FATAL_ERROR "GenerateNotices: INPUT_JSON not set or missing (${INPUT_JSON})")
endif()
if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "GenerateNotices: OUTPUT_FILE not set")
endif()

file(READ "${INPUT_JSON}" _json)

string(JSON _component_count ERROR_VARIABLE _err LENGTH "${_json}" components)
if(_err)
    message(FATAL_ERROR "GenerateNotices: malformed inventory: ${_err}")
endif()

# Maps a component's "requires" key onto the -DWITH_* flag passed in.
function(_component_enabled requires out_var)
    if(requires STREQUAL "always")
        set(${out_var} TRUE PARENT_SCOPE)
        return()
    endif()
    string(TOUPPER "${requires}" _upper)
    if(DEFINED WITH_${_upper} AND WITH_${_upper})
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(_summary_rows "")
set(_lgpl_components "")
set(_licenses_used "")

math(EXPR _last "${_component_count} - 1")
foreach(_i RANGE 0 ${_last})
    string(JSON _c GET "${_json}" components ${_i})

    string(JSON _name     GET "${_c}" name)
    string(JSON _version  GET "${_c}" version)
    string(JSON _license  GET "${_c}" license)
    string(JSON _linkage  GET "${_c}" linkage)
    string(JSON _url      GET "${_c}" url)
    string(JSON _purpose  GET "${_c}" purpose)
    string(JSON _requires GET "${_c}" requires)

    _component_enabled("${_requires}" _enabled)
    if(NOT _enabled)
        continue()
    endif()

    # A static LGPL component is a license violation, not a build
    # preference. Fail here as well as in CMakeLists so a standalone
    # regeneration also catches it.
    if(_license MATCHES "LGPL" AND NOT _linkage STREQUAL "dynamic")
        message(FATAL_ERROR
            "GenerateNotices: ${_name} is ${_license} but declares linkage '${_linkage}'. "
            "LGPL components must be dynamically linked so users retain their relink "
            "rights. See LICENSE section 9.2.")
    endif()

    list(APPEND _summary_rows
         "| [${_name}](${_url}) | ${_version} | ${_license} | ${_linkage} | ${_purpose} |")

    if(_license MATCHES "LGPL")
        list(APPEND _lgpl_components "${_name}")
    endif()
    list(APPEND _licenses_used "${_license}")
endforeach()

list(REMOVE_DUPLICATES _licenses_used)
list(JOIN _summary_rows "\n" _summary_table)
list(JOIN _licenses_used ", " _license_list)

string(TIMESTAMP _generated "%Y-%m-%d" UTC)

set(_content "# Third-Party Notices

<!-- GENERATED FILE. Edit cmake/third-party.json, not this file. -->
<!-- Regenerated ${_generated} by cmake/GenerateNotices.cmake -->

OmniFractal incorporates or links against the components listed below. Each is
licensed by its own authors under its own terms, which govern that component and
are **not** restricted by the OmniFractal EULA (see `LICENSE` section 9).

Reproducing these notices is a binding obligation under the MIT, BSD-3-Clause,
and Apache-2.0 licenses, not a courtesy. Any permitted redistribution of
OmniFractal must carry this file.

This listing reflects the components enabled in **this** build configuration.

## Components

| Component | Version | License | Linkage | Purpose |
|---|---|---|---|---|
${_summary_table}

Licenses present in this build: ${_license_list}.
")

if(_lgpl_components)
    list(JOIN _lgpl_components ", " _lgpl_list)
    string(APPEND _content "
## LGPL components: ${_lgpl_list}

These are **dynamically linked**, which is a licensing requirement rather than a
build preference. LGPL-2.1 section 6 permits combining the library with
proprietary software only if you retain the ability to relink against a modified
version of it.

`LICENSE` section 9.2 carries an explicit carve-out preserving your rights to
obtain, inspect, modify, relink, reverse engineer for that purpose, and
redistribute these components under LGPL-2.1. Those rights override sections 5
and 8 of the EULA and are not terminated by it.

Source corresponding to the linked binaries is available from each project's
listed URL. On request the Licensor will identify the exact version and build
configuration used.
")
endif()

string(APPEND _content "
## Relationship to Mandelbulb 3D

OmniFractal reads the `.m3p`, `.m3i`, `.m3a`, `.m3l`, `.m3c`, `.m3v`, `.m3f`,
`.d3f`, and `.dSO` formats for interoperability. It is **not** derived from
Mandelbulb 3D, is not a version or successor of it, and contains no code from
it.

* File formats are not copyrightable subject matter. Implementing a reader for
  interoperability is protected -- *Sega v. Accolade* (9th Cir. 1992), *Sony v.
  Connectix* (9th Cir. 2000), and Article 6 of EU Directive 2009/24/EC.
* The mathematics is not copyrightable. Mandelbox, Amazing Box, Tglad folds,
  spherical inversion, and the Hubbard-Douady distance estimate are published
  results, reimplemented from their public descriptions.
* No third-party creative assets ship with OmniFractal. Parameter presets, formula
  files, gradients, light maps, and background images belong to their authors and
  are not redistributed here. Point OmniFractal at your own installation to use them.

Full license texts for each component are reproduced in `docs/licenses/`.
")

file(WRITE "${OUTPUT_FILE}" "${_content}")
message(STATUS "GenerateNotices: wrote ${OUTPUT_FILE}")
