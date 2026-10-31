# Super Mario 64 DS 3DS port: build root.
#
# Builds the decomp's src/ and the PC port's port/ tree (a checkout of
# tangosdev/sm64ds-decomp at release/game-0.9.1, see PORT_ROOT) with
# devkitARM, plus the 3DS platform code in this folder. Nothing in PORT_ROOT is
# edited.
#
# Configure:
#   cmake -S port3ds -B build3ds -G Ninja
#         -DCMAKE_TOOLCHAIN_FILE=C:/devkitPro/cmake/3DS.cmake
cmake_minimum_required(VERSION 3.16)
project(sm64ds-3ds C CXX ASM)

set(PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../port-0.9.1" CACHE PATH
    "checkout of sm64ds-decomp at the PC port's release branch")
set(PORT_DIR "${PORT_ROOT}/port")
get_filename_component(PORT_ROOT_NAME "${PORT_ROOT}" ABSOLUTE)
get_filename_component(PORT_ROOT_NAME "${PORT_ROOT_NAME}" NAME)

# GAME3DS_WIDE=ON (build.sh: GAME3DS_WIDE=1, built in build3ds-wide): the
# 400x240 widescreen top screen with a 320x192 DS picture. OFF: the DS's 4:3
# picture, pillarboxed at 320x240.
option(GAME3DS_ALLOC_CENSUS "wrap the allocators and print who holds the heap (game/alloc_census.cpp)" OFF)
option(GAME3DS_WIDE "400x240 widescreen top screen (NTR_WIDE_3DS)" OFF)
if(GAME3DS_WIDE)
    add_compile_definitions(NTR_WIDE_3DS)
    set(GAME3DS_NAME_SUFFIX " (wide)")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# The decomp is written for mwccarm: type-punned pointers everywhere, signed
# overflow relied on, and C/C++ dialect corners GCC is stricter about.
set(SM64DS_OPT "-O2 -g1" CACHE STRING "optimisation and debug flags (build.sh passes SM64DS_OPT)")
separate_arguments(SM64DS_OPT_LIST UNIX_COMMAND "${SM64DS_OPT}")
add_compile_options(
    ${SM64DS_OPT_LIST}
    -fno-strict-aliasing
    -fwrapv
    -fno-delete-null-pointer-checks   # DS code may touch address 0 (ITCM there)
    -fno-isolate-erroneous-paths-dereference   # ...so no trap in its place either
    # Turn off the -O2 assumptions this port breaks on purpose: the HAL writes
    # functions into vtables at runtime, so virtual calls must go through the
    # table (devirtualization skipped those), and decomp code compares function
    # addresses, which identical-code folding would merge.
    $<$<COMPILE_LANGUAGE:CXX>:-fno-devirtualize>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-devirtualize-speculatively>
    -fno-ipa-icf
    -Wno-unknown-pragmas          # MSVC #pragma comment(linker, ...) in port/hal
    # No RTTI: the game's vtables keep their type-info slot either way, and the
    # HAL provides the ROM's own _ZTI data, which a GCC one would clash with.
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    # GCC 14 turned some old C diagnostics into errors (return with no value,
    # implicit declarations, int/pointer conversions). mwccarm accepts them and the
    # decomp relies on that, so -fpermissive makes them warnings again.
    -fpermissive
)

# Read a port/slice_*.txt manifest into a list of absolute source paths.
function(read_slice manifest outvar)
    file(STRINGS "${PORT_DIR}/${manifest}" lines)
    set(out "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line MATCHES "^#" OR line STREQUAL "")
            continue()
        endif()
        list(APPEND out "${PORT_ROOT}/${line}")
    endforeach()
    set(${outvar} "${out}" PARENT_SCOPE)
endfunction()

# Common settings for every target that compiles game code.
function(sm64ds_game_target target)
    # gccfix's transformed mirror of the PC-port headers, ahead of the originals.
    set(mirror "${CMAKE_CURRENT_BINARY_DIR}/gccfix/${PORT_ROOT_NAME}/port")
    target_include_directories(${target} BEFORE PRIVATE "${mirror}" "${mirror}/ntr/include")
    target_include_directories(${target} PRIVATE
        "${PORT_DIR}"
        "${PORT_DIR}/ntr/include"
        "${PORT_ROOT}/include")
    # The port's HAL keys host behaviour on SM64DS_PLATFORM_PC; the 3DS is a
    # host in that sense. SM64DS_HOST_3DS marks the places that differ.
    target_compile_definitions(${target} PRIVATE
        SM64DS_PLATFORM_PC
        SM64DS_HOST_3DS
        # hal/fs.cpp and friends find build/assets/ and extracted/ under this
        # (the PC build bakes the repo path in; SM64DS_ASSET_ROOT overrides).
        PORT_REPO_ROOT="sdmc:/sm64ds3ds")
    # MSVC keywords in the HAL, including hand-written Win32 declarations
    # (__declspec is in compat/include/msvc_declspec.h). msvc_compat.h is
    # force-included only into the PC port's own code (see sm64ds_msvc_compat).
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:-include;${CMAKE_CURRENT_SOURCE_DIR}/compat/include/msvc_declspec.h;-D__stdcall=;-D__cdecl=;-D__fastcall=;-D__thiscall=>")
    # Last, so the HAL's #include <windows.h> finds the compat shim.
    target_include_directories(${target} AFTER PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/compat/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simde")
endfunction()

# ROM data tables (sine table, identity matrices, ...) generated from the
# user's own decompressed arm9 (PORT_ROOT/extracted/arm9_dec.bin). That's
# Nintendo's data, so it only goes in the build tree, never the repo.
set(ROMDATA_C "${CMAKE_CURRENT_BINARY_DIR}/host-src/romdata.c")
add_custom_command(OUTPUT "${ROMDATA_C}"
    COMMAND "${PYTHON}" "${PORT_DIR}/tools/romdata.py" "${ROMDATA_C}"
    DEPENDS "${PORT_DIR}/tools/romdata.py" "${PORT_ROOT}/extracted/arm9_dec.bin"
    COMMENT "romdata from extracted/arm9_dec.bin")

# One .3dsx per PC smoke test:
#   sm64ds_smoke(<name> TEST <port/tests/x.cpp> [MEMPROBE] SOURCES <...>)
# The test's main() is renamed sm64ds_smoke_main and run by src/main_smoke.cpp.
# Output is build3ds/<name>.3dsx, logging to sdmc:/sm64ds3ds/<name>.log.
#
# The sources compile into an OBJECT library, tools/linkfix.py runs over the
# objects (weakening host copies of symbols the game defines and turning the
# PC HAL's /alternatename rows into PROVIDE lines), and the executable links
# the objects plus that generated linker script.
# STRING rather than FILEPATH/PATH: MSYS2's CMake treats "C:/..." as relative.
set(PYTHON "python" CACHE STRING "Windows Python 3 for the build tools")
set(DKA_BIN "C:/devkitPro/devkitARM/bin" CACHE STRING "devkitARM bin, Windows form")

# hostgen.py: copies of src/ files with MMIO stores rewritten into NTR_MMIO()
# proxies, so registers with write side effects (the geometry FIFO, the
# divider) still trigger them. Generated into the build tree.
function(sm64ds_hostgen outvar)
    cmake_parse_arguments(H "EXTERN_DATA" "" "SYMS" ${ARGN})
    set(out "")
    foreach(sym IN LISTS H_SYMS)
        if(EXISTS "${PORT_ROOT}/src/${sym}.c")
            set(src "${PORT_ROOT}/src/${sym}.c")
        else()
            set(src "${PORT_ROOT}/src/${sym}.cpp")
        endif()
        set(gen "${CMAKE_CURRENT_BINARY_DIR}/host-src/src/${sym}.cpp")
        list(APPEND out "${gen}")
        # One rule per output: several targets may ask for the same symbol.
        get_property(done GLOBAL PROPERTY SM64DS_HOSTGEN_DONE)
        if("${sym}" IN_LIST done)
            continue()
        endif()
        set_property(GLOBAL APPEND PROPERTY SM64DS_HOSTGEN_DONE "${sym}")
        set(flags "")
        if(H_EXTERN_DATA)
            set(flags --extern-data)
        endif()
        add_custom_command(OUTPUT "${gen}"
            COMMAND "${PYTHON}" "${PORT_DIR}/tools/hostgen.py"
                    --out "${CMAKE_CURRENT_BINARY_DIR}/host-src" ${flags} ${sym}
            DEPENDS "${src}" "${PORT_DIR}/tools/hostgen.py"
            COMMENT "hostgen ${sym}")
    endforeach()
    set(${outvar} "${out}" PARENT_SCOPE)
endfunction()

# The PICA vertex shader for src/gpu3ds.cpp, embedded as gx_shader_shbin[].
ctr_add_shader_library(gx_shader shaders/gx.v.pica)
dkp_add_embedded_binary_library(gx_shader_bin gx_shader)
# Step 5's: the DS transform itself, from gx.cpp's record (gx_rec_shader_shbin[]).
ctr_add_shader_library(gx_rec_shader shaders/gx_rec.v.pica)
dkp_add_embedded_binary_library(gx_rec_shader_bin gx_rec_shader)

# In the list named by <listvar>, swap every source tools/gccfix.py had to
# rewrite for its build-tree copy, and mark .c files with the decomp's //cpp
# marker as C++. Covers the upstream checkout and the PC probe tree. Per-file
# rules come from game/gccfix_rules.json. Runs at configure time, and the
# originals the rules touch are configure dependencies.
set(SM64DS_TOP "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(GCCFIX_RULES "${CMAKE_CURRENT_SOURCE_DIR}/game/gccfix_rules.json")
if(EXISTS "${GCCFIX_RULES}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GCCFIX_RULES}")
endif()
function(sm64ds_gccfix listvar)
    set(listfile "${CMAKE_CURRENT_BINARY_DIR}/gccfix_${listvar}_in.txt")
    set(srcs "")
    foreach(s IN LISTS ${listvar})
        if(s MATCHES "\\.(c|cpp)$" AND NOT s MATCHES "^${CMAKE_CURRENT_SOURCE_DIR}/(src|compat|game|tests)/")
            list(APPEND srcs "${s}")
        endif()
    endforeach()
    string(REPLACE ";" "\n" body "${srcs}")
    file(WRITE "${listfile}" "${body}\n")
    execute_process(
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gccfix.py"
                --root "${SM64DS_TOP}" --out "${CMAKE_CURRENT_BINARY_DIR}/gccfix"
                --list "${listfile}" --rules "${GCCFIX_RULES}"
                --headers "${PORT_DIR}/hal" --headers "${PORT_DIR}/ntr/include"
                --headers "${PORT_DIR}/unmatched" --headers "${PORT_DIR}/tests"
        OUTPUT_VARIABLE map RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "gccfix.py failed (${rc})")
    endif()
    set(out ${${listvar}})
    string(REPLACE "\n" ";" rows "${map}")
    foreach(row IN LISTS rows)
        if(row STREQUAL "")
            continue()
        endif()
        string(REPLACE "\t" ";" cols "${row}")
        list(GET cols 0 orig)
        list(GET cols 1 copy)
        list(GET cols 2 lang)
        set(target_file "${orig}")
        if(NOT copy STREQUAL "-")
            list(FIND out "${orig}" at)
            if(at GREATER -1)
                list(REMOVE_AT out ${at})
                list(INSERT out ${at} "${copy}")
            endif()
            get_filename_component(origdir "${orig}" DIRECTORY)
            set_source_files_properties("${copy}" PROPERTIES INCLUDE_DIRECTORIES "${origdir}")
            # The copy compiles exactly as the original would have.
            foreach(prop COMPILE_DEFINITIONS COMPILE_OPTIONS)
                get_source_file_property(v "${orig}" ${prop})
                if(v)
                    set_source_files_properties("${copy}" PROPERTIES ${prop} "${v}")
                endif()
            endforeach()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${orig}")
            set(target_file "${copy}")
        endif()
        if(lang STREQUAL "CXX")
            set_source_files_properties("${target_file}" PROPERTIES LANGUAGE CXX)
        endif()
    endforeach()
    set(${listvar} "${out}" PARENT_SCOPE)
endfunction()

#   [GPU]  the target draws with citro3d (src/gpu3ds.cpp): links citro3d and
#          the gx shader, and puts the text console on the bottom screen. TEST
#          can then be one of this repo's own tests (relative to port3ds/).
# Force-include compat/include/msvc_compat.h only where it's needed: the PC
# port's own code and the PC build's generated non-src files. The decomp's
# src/ never uses MSVC runtime names, and leaving its ~8,000 files out means a
# compat edit rebuilds ~600 files instead of everything.
function(sm64ds_msvc_compat)
    foreach(s IN LISTS ARGN)
        if(s MATCHES "/port/(hal|ntr|tests|unmatched)/" OR
           (s MATCHES "/host-src/" AND NOT s MATCHES "/host-src/src/"))
            # Keep MSVC's in-order layout: the HAL puts runs of DS globals under
            # `#pragma data_seg` (ignored by GCC) and expects them to stay in definition
            # order, zero-filled ones included.
            set_property(SOURCE "${s}" APPEND PROPERTY COMPILE_OPTIONS
                "-include;${CMAKE_CURRENT_SOURCE_DIR}/compat/include/msvc_compat.h"
                -fno-toplevel-reorder -fno-zero-initialized-in-bss)
        endif()
    endforeach()
endfunction()

function(sm64ds_smoke name)
    cmake_parse_arguments(S "MEMPROBE;GPU" "TEST" "SOURCES" ${ARGN})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${S_TEST}")
        set(test_src "${CMAKE_CURRENT_SOURCE_DIR}/${S_TEST}")
    else()
        set(test_src "${PORT_DIR}/${S_TEST}")
    endif()
    # src/ files tools/closure.py found this target's link needs.
    set(auto_list "${CMAKE_CURRENT_SOURCE_DIR}/slices/${name}.auto.txt")
    set(auto_sources "")
    set(auto_hostgen "")
    if(EXISTS "${auto_list}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${auto_list}")
        file(STRINGS "${auto_list}" auto_lines)
        foreach(line IN LISTS auto_lines)
            string(STRIP "${line}" line)
            if(line MATCHES "^#" OR line STREQUAL "")
                continue()
            endif()
            # "hostgen:<sym>": compile hostgen.py's rewrite of src/<sym>.
            if(line MATCHES "^hostgen:(.+)$")
                list(APPEND auto_hostgen "${CMAKE_MATCH_1}")
            else()
                list(APPEND auto_sources "${PORT_ROOT}/${line}")
            endif()
        endforeach()
    endif()
    if(auto_hostgen)
        sm64ds_hostgen(auto_gen SYMS ${auto_hostgen})
        list(APPEND auto_sources ${auto_gen})
    endif()
    # Swap in tools/gccfix.py copies for the src/ files GCC rejects as written.
    set(all_sources ${S_SOURCES} ${auto_sources})
    sm64ds_gccfix(all_sources)
    sm64ds_msvc_compat(${all_sources} "${test_src}")
    add_library(${name}_obj OBJECT
        src/main_smoke.cpp
        src/console_tee.cpp
        src/memprobe.cpp
        src/crash_handler.cpp
        src/host3ds_mem.cpp
        compat/win32_compat.cpp
        compat/fiber_switch.S
        "${test_src}"
        ${all_sources})
    sm64ds_game_target(${name}_obj)
    target_compile_definitions(${name}_obj PRIVATE SMOKE_NAME="${name}")
    # ppu_sub.cpp includes src/sub_scan_single.inl in every smoke, GPU or not
    target_include_directories(${name}_obj PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(S_MEMPROBE)
        target_compile_definitions(${name}_obj PRIVATE SMOKE_MEMPROBE)
    endif()
    set_source_files_properties("${test_src}" TARGET_DIRECTORY ${name}_obj
        PROPERTIES COMPILE_DEFINITIONS "main=sm64ds_smoke_main")
    if(S_GPU)
        target_sources(${name}_obj PRIVATE src/gpu3ds.cpp src/host3ds.cpp)
        target_compile_definitions(${name}_obj PRIVATE SMOKE_CONSOLE_BOTTOM)
        target_link_libraries(${name}_obj PRIVATE gx_shader_bin gx_rec_shader_bin)
    endif()

    set(ld "${CMAKE_CURRENT_BINARY_DIR}/${name}_aliases.ld")
    # The object list goes to linkfix in a response file (one path per line):
    # the game's ~9,000 objects exceed the Windows command-line limit.
    set(objlist "${CMAKE_CURRENT_BINARY_DIR}/${name}_objects.rsp")
    file(GENERATE OUTPUT "${objlist}"
        CONTENT "$<JOIN:$<TARGET_OBJECTS:${name}_obj>,\n>\n")
    # /alternatename rows from the HAL, the PC port's ntr/ and unmatched/ code,
    # and (for the game) the rewritten src/ copies.
    set(alias_src --alias-src "${PORT_DIR}/hal" --alias-src "${PORT_DIR}/ntr"
                  --alias-src "${PORT_DIR}/unmatched")
    if(DEFINED PC_PROBE AND EXISTS "${PC_PROBE}/host-src")
        list(APPEND alias_src --alias-src "${PC_PROBE}/host-src")
    endif()
    add_custom_command(OUTPUT "${ld}"
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/linkfix.py"
                --out "${ld}" --report "${CMAKE_CURRENT_BINARY_DIR}/${name}_linkfix.txt"
                --tools "${DKA_BIN}"
                --lib-cache "${CMAKE_CURRENT_BINARY_DIR}/libsyms.txt"
                --port-root-name "${PORT_ROOT_NAME}"
                ${alias_src}
                --objects-out "${CMAKE_CURRENT_BINARY_DIR}/${name}_objects.native.rsp"
                "@${objlist}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${name}_objects.native.rsp"
        DEPENDS ${name}_obj $<TARGET_OBJECTS:${name}_obj> "${objlist}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/linkfix.py"
        COMMENT "linkfix ${name}"
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(${name}_linkfix DEPENDS "${ld}")

    # The objects go to g++ as a response file of native paths (written by
    # linkfix) because the game's command line would be past the Windows limit.
    # The executable's own source is empty. Relative @path since MSYS doesn't
    # convert an @/c/... argument, and Ninja links from this directory.
    add_executable(${name} src/link_stub.cpp)
    add_dependencies(${name} ${name}_linkfix)
    # --sort-section=name: MSVC's order for grouped sections (see
    # compat/include/msvc_declspec.h, allocate).
    target_link_options(${name} PRIVATE "${ld}" -Wl,--sort-section=name
        "@${name}_objects.native.rsp")
    set_target_properties(${name} PROPERTIES LINK_DEPENDS
        "${ld};${CMAKE_CURRENT_BINARY_DIR}/${name}_objects.native.rsp")
    if(S_GPU)
        target_link_libraries(${name} PRIVATE gx_shader_bin gx_rec_shader_bin citro3d)
        # Hook citro3d's waits without changing its SYNCDRAW path:
        # gspSetEventCallback so we count vblanks too (frame_sync), and
        # gxCmdQueueStop/Clear and gspWaitForEvent so frames built ahead
        # (ahead_flush) can begin without waiting for the queue.
        target_link_options(${name} PRIVATE -Wl,--wrap=gxCmdQueueWait
            -Wl,--wrap=gspWaitForAnyEvent -Wl,--wrap=gspSetEventCallback
            -Wl,--wrap=gxCmdQueueStop -Wl,--wrap=gxCmdQueueClear -Wl,--wrap=gspWaitForEvent)
    endif()
    target_link_libraries(${name} PRIVATE ctru m)
    ctr_generate_smdh(${name}.smdh
        NAME "SM64DS 3DS: ${name}${GAME3DS_NAME_SUFFIX}"
        DESCRIPTION "sm64ds-3ds port smoke test"
        AUTHOR "sm64ds-3ds")
    ctr_create_3dsx(${name} SMDH ${name}.smdh)
endfunction()

read_slice(slice_gate1.txt  GATE1_SOURCES)
read_slice(slice_gate2.txt  GATE2_SOURCES)
read_slice(slice_gate3a.txt GATE3A_SOURCES)
read_slice(slice_gate3b.txt GATE3B_SOURCES)

# Gate 1: fixed-point math, Timer, Fader. Also carries the memory-map probe.
sm64ds_smoke(smoke3ds TEST tests/smoke.cpp MEMPROBE SOURCES
    src/shims_gate1.cpp
    "${PORT_DIR}/hal/cstd_div.c"
    "${PORT_DIR}/hal/os_time.cpp"
    ${GATE1_SOURCES})

# Gate 2: the ExpandingHeapAllocator family, 5,000-op torture test. The PC
# target also links generated MSVC wrappers and /alternatename aliases; GCC
# uses the ROM's own names and ARM calling convention, so neither is needed.
sm64ds_smoke(smoke_heap TEST tests/smoke_heap.cpp SOURCES
    "${PORT_DIR}/hal/heap_globals.cpp"
    ${GATE2_SOURCES})

# Gate 3a: SetupRootHeap and the Memory:: layer on top of the gate 2 allocator.
#
# 3DS-only extras: the destructors (under the Itanium ABI a vtable is only
# emitted where its key function is defined, while MSVC emits one wherever it's
# needed), and the four ExpandingHeap methods the PC build leaves to HAL
# slots. The PC HAL's _ZTV13ExpandingHeap is an array of x86 __fastcall shims
# that would read every argument one register off on ARM. With the class's own
# methods compiled, GCC emits the real table and linkfix weakens the HAL's.
set(GATE3A_3DS_SOURCES
    "${PORT_ROOT}/src/_ZN4HeapD1Ev.cpp"
    "${PORT_ROOT}/src/_ZN13ExpandingHeapD1Ev.cpp"
    "${PORT_ROOT}/src/_ZN13ExpandingHeap8VDestroyEv.cpp"
    "${PORT_ROOT}/src/_ZN13ExpandingHeap14VDeallocateAllEv.cpp"
    "${PORT_ROOT}/src/_ZN13ExpandingHeap19VMaxAllocatableSizeEv.cpp"
    "${PORT_ROOT}/src/_ZN13ExpandingHeap22VMaxAllocationUnitSizeEv.cpp")
sm64ds_smoke(smoke_roots TEST tests/smoke_roots.cpp SOURCES
    "${PORT_DIR}/hal/heap_globals.cpp"
    "${PORT_DIR}/hal/os_arena.cpp"
    "${PORT_DIR}/hal/heap_vtable.cpp"
    ${GATE3A_3DS_SOURCES}
    ${GATE2_SOURCES} ${GATE3A_SOURCES})

# Gate 3b: SharedFilePtr over the host card code (port/hal/fs.cpp): real game
# files from the asset catalog, LZ77 decoded by the HAL and cross-checked.
# Needs the assets on the SD card (tools/stage_assets.ps1).
sm64ds_smoke(smoke_fs TEST tests/smoke_fs.cpp SOURCES
    "${PORT_DIR}/hal/heap_globals.cpp"
    "${PORT_DIR}/hal/os_arena.cpp"
    "${PORT_DIR}/hal/heap_vtable.cpp"
    "${PORT_DIR}/hal/fs.cpp"
    "${ROMDATA_C}"
    ${GATE3A_3DS_SOURCES}
    ${GATE2_SOURCES} ${GATE3A_SOURCES} ${GATE3B_SOURCES})

# ---- port/ntr: the DS hardware layer (I/O, GX, 2D engines, runtime) -------
set(NTR_SOURCES "")
foreach(f io gx texture ppu ppu_sub ppu_audit lz77 rt runtime fs bmd ipc hdtex smooth)
    list(APPEND NTR_SOURCES "${PORT_DIR}/ntr/${f}.cpp")
endforeach()

sm64ds_hostgen(GATE4A_GEN SYMS
    func_0205532c func_02055388 func_020553a4 func_02055780
    func_0205a358 func_0205a290 func_0205a21c func_02056e98 func_02059fa8)

# Gate 4a: the game's interrupt-driven display-list pump over the ntr GX,
# stream checked byte-equal against the harness.
sm64ds_smoke(smoke_gx TEST tests/smoke_gx.cpp SOURCES
    ${GATE4A_GEN} ${NTR_SOURCES})

sm64ds_hostgen(GATE4B_GEN SYMS
    func_02044b30 Geometry_MatrixMultiply3x3 func_02055998
    func_02044534 func_0204488c NormalizeVec3)
read_slice(slice_gate4b.txt GATE4B_SOURCES)

# The common stack of gates 4b, 4c and 5: heap + fs + the Model pipeline.
set(MODEL_STACK_SOURCES
    "${PORT_DIR}/hal/heap_globals.cpp"
    "${PORT_DIR}/hal/os_arena.cpp"
    "${PORT_DIR}/hal/heap_vtable.cpp"
    "${PORT_DIR}/hal/fs.cpp"
    "${PORT_DIR}/hal/model_host.cpp"
    "${PORT_DIR}/hal/gx_upload_bridge.cpp"
    "${ROMDATA_C}"
    ${GATE3A_3DS_SOURCES}
    ${GATE2_SOURCES} ${GATE3A_SOURCES} ${GATE3B_SOURCES} ${GATE4B_SOURCES}
    ${GATE4A_GEN} ${GATE4B_GEN} ${NTR_SOURCES})

# Gate 4b: the whole Model pipeline: load, rebase, VRAM upload, materials,
# render (Mario, textured).
sm64ds_smoke(smoke_model TEST tests/smoke_model.cpp SOURCES ${MODEL_STACK_SOURCES})

# Gate 4c: Animation / UpdateBones recursion (the Mad Piano, posed).
sm64ds_smoke(smoke_anim TEST tests/smoke_anim.cpp SOURCES ${MODEL_STACK_SOURCES})

# Gate 5: the fiber frame loop: game-shaped frames, the piano attack in motion.
sm64ds_smoke(smoke_frames TEST tests/smoke_frames.cpp SOURCES ${MODEL_STACK_SOURCES})

# ---- Milestone 3: the 3DS GPU renderer ------------------------------------
# view_model: the gate-4b model pipeline drawn by citro3d on the top screen.
sm64ds_smoke(view_model TEST tests/view_model.cpp GPU SOURCES ${MODEL_STACK_SOURCES})
# readback: gpu3ds::readback (the card's settle) on a title-shaped frame of
# known triangles: positions, colours, coverage and the depth order.
sm64ds_smoke(readback TEST tests/readback.cpp GPU SOURCES ${MODEL_STACK_SOURCES})
# card_model: the same pipeline through the ntr/gx.h card protocol and the
# 3DS card (src/card3ds.cpp), with a synthetic 2D overlay and the fade.
sm64ds_smoke(card_model TEST tests/card_model.cpp GPU SOURCES
    src/card3ds.cpp src/interp3ds.cpp ${MODEL_STACK_SOURCES})

# ---- the game ----
# The PC port's game executable (walk_window plus its shared slice and ntr)
# as exported by tools/pcmanifest.py, minus game/exclude.txt, plus the 3DS card
# and host code. PC_PROBE is the PC CMake project configured with this
# toolchain and its generators run (tools/pcprobe.sh); the generated files the
# manifest names live there.
set(PC_PROBE "${CMAKE_CURRENT_SOURCE_DIR}/../build-pcprobe" CACHE STRING
    "the PC port's CMake configured by tools/pcprobe.sh")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game/manifest.cmake")
    # the manifest's per-file defines/options apply to this target only
    set(GAME_OBJ_TARGET game3ds_obj)
    include("${CMAKE_CURRENT_SOURCE_DIR}/game/manifest.cmake")
    # The host's per-pixel code (the 2D engines, menus, resolve) is most of a title
    # frame at -O1. It's host code, not decomp code, so -O2 is safe. gccfix copies
    # keep the option.
    foreach(f port/ntr/ppu.cpp port/ntr/ppu_sub.cpp port/ntr/gx.cpp port/hal/message_compositor.cpp
            port/hal/sub_screen.cpp port/hal/menu_compose.cpp)
        set_property(SOURCE "${PORT_ROOT}/${f}" APPEND PROPERTY COMPILE_OPTIONS -O2)
    endforeach()
    set_property(SOURCE src/card3ds.cpp src/gpu3ds.cpp src/interp3ds.cpp APPEND PROPERTY COMPILE_OPTIONS -O2)
    # The deleting-destructor forwarders define symbols an inline virtual
    # destructor also emits when not inlined (as -Os does), which causes a section
    # conflict, so they stay at -O2 whatever SM64DS_OPT says.
    file(GLOB DTOR_FWD "${PORT_ROOT}/port/hal/dtor_forwarders_gen*.cpp")
    set_property(SOURCE ${DTOR_FWD} APPEND PROPERTY COMPILE_OPTIONS -O2)
    # and the generated DS data: -Os stops word-aligning small arrays, and
    # check_ds_align refuses a DS global off its word (the 3DS faults on ldm/strd)
    file(GLOB HOST_DATA "${PC_PROBE}/host-src/*.c" "${PC_PROBE}/host-src/*.cpp")
    set_property(SOURCE ${HOST_DATA} APPEND PROPERTY COMPILE_OPTIONS -O2)
    foreach(f port/hal/actor_vtables.cpp port/hal/cxx_aliases.cpp port/hal/sub_actors.cpp)
        set_property(SOURCE "${PORT_ROOT}/${f}" APPEND PROPERTY COMPILE_OPTIONS -O2)
    endforeach()
    set_source_files_properties(game/emit_inline.cpp PROPERTIES
        COMPILE_OPTIONS -fkeep-inline-functions)
    sm64ds_smoke(game3ds TEST game/main_game.cpp GPU SOURCES
        src/card3ds.cpp src/interp3ds.cpp game/early_io.cpp game/files_pack.cpp game/card_fast.cpp game/host_stubs.cpp game/emit_inline.cpp
        game/virtual_thunks.cpp game/input3ds.cpp game/options3ds.cpp game/out_3ds.cpp game/phase_timers.cpp game/watchdog3ds.cpp
        game/save_replace.cpp game/save_async.cpp game/sub_async.cpp game/lid3ds.cpp ${GAME_SOURCES})
    target_link_options(game3ds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/game/game_aliases.ld")
    # game/ld/3dsx.ld (writable .rodata: the HAL writes GCC's vtables) shadows
    # devkitARM's; 3dsx.specs looks the script up with %s, which searches -B first.
    target_link_options(game3ds PRIVATE "-B${CMAKE_CURRENT_SOURCE_DIR}/game/ld/")
    # game/phase_timers.cpp times the scene tick's phases through these
    foreach(sym hal_sub_screen_frame_begin port_frame_phase2 port_actor_frame port_actor_tick
            _ZN3ntr16rt_scanout_frameEv port_actor_render port_frame_phase5
            _ZN3ntr13gx_clear_fillERNS_11FramebufferE _ZN3ntr9gx_renderERNS_11FramebufferE
            port_message_composite_engine_a hal_sub_screen_present port_actor_scene_pass
            port_actor_frame_end sdat_host_tick
            func_02043fdc func_020441cc _ZN3ntr15gx_write_fifo_nEPKmi _ZN3ntr8io_writeEmyj
            _ZN3ntr16ppu_vram_publishEv _ZN3ntr19ppu_display_captureEPKmii
            _ZN3ntr15ppu_scanout_subERNS_14SubFramebufferE _ZN3OAM4LoadEv
            _ZN3ntr22gx_card_settle_becauseEPKc hal_menu_present
            _ZN3ntr15ppu_compose_subERKNS_14SubFramebufferEPmiiiiii port_engine_a_fade)
        target_link_options(game3ds PRIVATE "-Wl,--wrap=${sym}")
    endforeach()
    # game/main_game.cpp: the core 2 present must be idle when gx.cpp frees
    # the decoded textures (src/card3ds.cpp, "on core 2")
    target_link_options(game3ds PRIVATE "-Wl,--wrap=_ZN3ntr22gx_invalidate_texturesEv")
    # game/main_game.cpp: the New 3DS C-stick presses the bottom screen's
    # camera arrows inside Stage::CheckCameraInput
    target_link_options(game3ds PRIVATE "-Wl,--wrap=_ZN5Stage16CheckCameraInputEv")
    # no minimap touch circle for the Circle Pad's touch-stick record
    target_link_options(game3ds PRIVATE "-Wl,--wrap=_ZN7Minimap8BehaviorEv")
    # game/main_game.cpp counts and times the game's file opens (the level
    # boot's wait is the SD card's open, not its reads)
    target_link_options(game3ds PRIVATE "-Wl,--wrap=fopen" "-Wl,--wrap=fclose")
    # game/main_game.cpp names each display list whole for gx.cpp's list memo
    # (the ROM's two list senders)
    target_link_options(game3ds PRIVATE "-Wl,--wrap=func_0205a358" "-Wl,--wrap=func_01ffde98")
    # game/card_fast.cpp: the ROM's card page reader, a block in one call
    target_link_options(game3ds PRIVATE "-Wl,--wrap=func_02060a64")
    # the widescreen build widens the ROM's object cull after each Camera::Render
    # (game/main_game.cpp)
    if(GAME3DS_WIDE)
        target_link_options(game3ds PRIVATE "-Wl,--wrap=_ZN6Camera6RenderEv")
    endif()
    # GAME3DS_ALLOC_CENSUS=ON (build.sh: GAME3DS_ALLOC_CENSUS=1): who holds the
    # heap and the linear heap, in the [mem] lines (game/alloc_census.cpp)
    if(GAME3DS_ALLOC_CENSUS)
        target_sources(game3ds PRIVATE game/alloc_census.cpp)
        foreach(sym malloc calloc realloc memalign free _Znwj _Znaj linearAlloc linearMemAlign linearFree)
            target_link_options(game3ds PRIVATE "-Wl,--wrap=${sym}")
        endforeach()
    endif()
    set_property(TARGET game3ds APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/game/game_aliases.ld;${CMAKE_CURRENT_SOURCE_DIR}/game/ld/3dsx.ld")
    # A DS global left less aligned than on the DS faults on hardware only
    # (tools/check_ds_align.py says why); refuse such a link.
    add_custom_command(TARGET game3ds POST_BUILD
        COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ds_align.py"
                "${CMAKE_NM}" "$<TARGET_FILE:game3ds>"
        VERBATIM)
endif()
