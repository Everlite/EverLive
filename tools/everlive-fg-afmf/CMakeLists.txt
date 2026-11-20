cmake_minimum_required(VERSION 3.28)
project(afmf-linux VERSION 1.3.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

option(AFMF_WERROR "Treat warnings as errors" ON)
option(AFMF_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

include(GNUInstallDirs)

# The layer only needs the headers: a Vulkan layer must never link the loader itself.
# The test host is an ordinary application and does link it.
if(NOT DEFINED Vulkan_INCLUDE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/vulkan/vulkan.h")
    set(Vulkan_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE PATH "Vulkan header dir" FORCE)
endif()
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
# VK_EXT_swapchain_maintenance1 and VK_KHR_present_id are the oldest structures the layer carries
# for the application; VK_EXT_present_timing is optional (#ifdef in src/swapchain.c) so the
# distribution packages build against the headers Ubuntu 24.04 and Debian trixie ship.
if(Vulkan_VERSION VERSION_LESS 1.3.250)
    message(FATAL_ERROR "Vulkan headers ${Vulkan_VERSION} are too old; 1.3.250 or newer is needed")
endif()

set(AFMF_WARNINGS
    -Wall -Wextra
    -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wformat=2 -Werror=format-security
    -Wcast-qual -Wcast-align -Wpointer-arith -Wwrite-strings
    -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wmissing-declarations
    -Wvla -Walloca -Wstack-protector
    -Wnull-dereference -Wimplicit-fallthrough
    -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion
    $<$<BOOL:${AFMF_WERROR}>:-Werror>
)
set(AFMF_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

# ---- shaders: GLSL -> SPIR-V -> one embedded header --------------------------------------------

find_program(AFMF_GLSLANG NAMES glslangValidator glslang)
if(NOT AFMF_GLSLANG)
    message(FATAL_ERROR "glslang not found; it compiles the layer's shaders (Fedora: dnf install glslang)")
endif()

# Optional: every compiled module is also checked with spirv-val when SPIRV-Tools is installed.
find_program(AFMF_SPIRV_VAL spirv-val)
if(AFMF_SPIRV_VAL)
    set(AFMF_VALIDATE "${AFMF_SPIRV_VAL}" --target-env vulkan1.1)
else()
    set(AFMF_VALIDATE "${CMAKE_COMMAND}" -E echo "spirv-val not installed, skipping")
endif()
set(AFMF_SPV_DIR "${CMAKE_BINARY_DIR}/shaders")
set(AFMF_FFX_DIR "${CMAKE_SOURCE_DIR}/shaders/fidelityfx")
file(GLOB_RECURSE AFMF_FFX_HEADERS "${AFMF_FFX_DIR}/*.h")
set(AFMF_SPV_FILES)

# afmf_glsl(<output name> <source> [defines...]): compiles one compute shader to ${AFMF_SPV_DIR}/<name>.spv
function(afmf_glsl name source)
    set(spv "${AFMF_SPV_DIR}/${name}.spv")
    add_custom_command(
        OUTPUT "${spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${AFMF_SPV_DIR}"
        # Vulkan 1.1 / SPIR-V 1.3: the lowest version with subgroup ops, so the layer works
        # inside any application that asked for 1.1 or newer (see api_version in framegen.c).
        COMMAND "${AFMF_GLSLANG}" -V --target-env vulkan1.1 -S comp ${ARGN}
                "-I${AFMF_FFX_DIR}" -o "${spv}" "${source}"
        COMMAND ${AFMF_VALIDATE} "${spv}"
        DEPENDS "${source}" ${AFMF_FFX_HEADERS}
        COMMENT "glslang ${name}"
        VERBATIM
    )
    set(AFMF_SPV_FILES ${AFMF_SPV_FILES} "${spv}" PARENT_SCOPE)
endfunction()

# FidelityFX Optical Flow passes, SDR permutation, exactly as the SDK's Vulkan backend built them.
set(AFMF_FFX_DEFINES -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0)
foreach(pass
        ffx_opticalflow_prepare_luma_pass
        ffx_opticalflow_compute_luminance_pyramid_pass
        ffx_opticalflow_generate_scd_histogram_pass
        ffx_opticalflow_compute_scd_divergence_pass
        ffx_opticalflow_compute_optical_flow_advanced_pass_v5
        ffx_opticalflow_filter_optical_flow_pass_v5
        ffx_opticalflow_scale_optical_flow_advanced_pass_v5)
    afmf_glsl(${pass} "${AFMF_FFX_DIR}/passes/${pass}.glsl" ${AFMF_FFX_DEFINES})
endforeach()
# The block search once more with the SAD on packed 16-bit pairs, for devices with shaderInt16.
afmf_glsl(ffx_opticalflow_compute_optical_flow_advanced_pass_v5_int16
    "${AFMF_FFX_DIR}/passes/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl"
    ${AFMF_FFX_DEFINES} -DAFMF_SAD_INT16=1)

# The interpolator, once per output format it can write (see src/framegen_format.c).
set(AFMF_INTERP "${CMAKE_SOURCE_DIR}/shaders/afmf_interpolate.comp")
afmf_glsl(afmf_interpolate_rgba8 "${AFMF_INTERP}" -DAFMF_OUT_FORMAT=rgba8 -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_interpolate_rgba8_bgra "${AFMF_INTERP}" -DAFMF_OUT_FORMAT=rgba8 -DAFMF_SWAP_RB=1)
afmf_glsl(afmf_interpolate_rgb10a2 "${AFMF_INTERP}" -DAFMF_OUT_FORMAT=rgb10_a2 -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_interpolate_rgba16f "${AFMF_INTERP}" -DAFMF_OUT_FORMAT=rgba16f -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_interpolate_noformat "${AFMF_INTERP}" -DAFMF_OUT_FORMAT_NONE=1 -DAFMF_SWAP_RB=0)

# The fused ingest (colour ring + luma in one read of the game's frame), once per ring format.
set(AFMF_INGEST "${CMAKE_SOURCE_DIR}/shaders/afmf_ingest.comp")
afmf_glsl(afmf_ingest_rgba8 "${AFMF_INGEST}" -DAFMF_OUT_FORMAT=rgba8)
afmf_glsl(afmf_ingest_rgb10a2 "${AFMF_INGEST}" -DAFMF_OUT_FORMAT=rgb10_a2)
afmf_glsl(afmf_ingest_rgba16f "${AFMF_INGEST}" -DAFMF_OUT_FORMAT=rgba16f)
afmf_glsl(afmf_ingest_noformat "${AFMF_INGEST}" -DAFMF_OUT_FORMAT_NONE=1)

# The half-resolution downsample for performance mode: 8-bit, or half floats for scRGB.
set(AFMF_DOWNSAMPLE "${CMAKE_SOURCE_DIR}/shaders/afmf_downsample.comp")
afmf_glsl(afmf_downsample_rgba8 "${AFMF_DOWNSAMPLE}" -DAFMF_OUT_FORMAT=rgba8)
afmf_glsl(afmf_downsample_rgba16f "${AFMF_DOWNSAMPLE}" -DAFMF_OUT_FORMAT=rgba16f)

set(AFMF_SPIRV_HEADER "${AFMF_SPV_DIR}/afmf_spirv.h")
list(JOIN AFMF_SPV_FILES "|" AFMF_SPV_JOINED)
add_custom_command(
    OUTPUT "${AFMF_SPIRV_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${AFMF_SPIRV_HEADER}" "-DINPUTS=${AFMF_SPV_JOINED}"
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake"
    DEPENDS ${AFMF_SPV_FILES} "${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake"
    COMMENT "Embedding SPIR-V"
    VERBATIM
)

# ---- the layer: libafmf-linux.so + afmf-linux.json ----------------------------------------------

add_library(afmf-linux SHARED
    src/config.c
    src/framegen.c
    src/framegen_descriptors.c
    src/framegen_dump.c
    src/framegen_format.c
    src/framegen_passes.c
    src/framegen_pipelines.c
    src/framegen_profiler.c
    src/framegen_record.c
    src/framegen_record_frame.c
    src/framegen_resources.c
    src/layer.c
    src/layer_device.c
    src/layer_device_fns.c
    src/layer_device_probe.c
    src/layer_device_queues.c
    src/layer_instance.c
    src/layer_queue.c
    src/layer_registry.c
    src/log.c
    src/swapchain.c
    src/swapchain_cadence.c
    src/swapchain_chain.c
    src/swapchain_create.c
    src/swapchain_destroy.c
    src/swapchain_present_chain.c
    src/swapchain_presenter.c
    src/swapchain_record.c
    src/swapchain_resources.c
    src/swapchain_threads.c
    src/swapchain_worker.c
    "${AFMF_SPIRV_HEADER}"
)
target_include_directories(afmf-linux PRIVATE src "${AFMF_SPV_DIR}")
target_link_libraries(afmf-linux PRIVATE Vulkan::Headers Threads::Threads)
target_compile_definitions(afmf-linux PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(afmf-linux PRIVATE ${AFMF_WARNINGS})
set_target_properties(afmf-linux PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/layer")

# Two manifests from one template: the build tree's points at the built .so by absolute path (usable
# straight away with VK_ADD_IMPLICIT_LAYER_PATH=<build>/layer), the installed one at the installed .so.
set(AFMF_LAYER_LIBRARY_PATH "$<TARGET_FILE:afmf-linux>")
configure_file(layer/afmf-linux.json.in "${CMAKE_BINARY_DIR}/layer/afmf-linux.json.genex" @ONLY)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/layer/afmf-linux.json"
     INPUT "${CMAKE_BINARY_DIR}/layer/afmf-linux.json.genex")

set(AFMF_LAYER_LIBRARY_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/libafmf-linux.so")
configure_file(layer/afmf-linux.json.in "${CMAKE_BINARY_DIR}/install/afmf-linux.json" @ONLY)

install(TARGETS afmf-linux LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CMAKE_BINARY_DIR}/install/afmf-linux.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d")

# ---- tests -----------------------------------------------------------------------------------

enable_testing()

# The loader dlcloses the driver at instance destruction; LeakSanitizer then reports the driver's
# once-initialised allocations (reachable from its static data) as leaks with no symbols. Keeping
# the libraries mapped makes the leak check mean something.
# An installed afmf-linux package (an implicit layer of the same name) would win over the build's
# manifest in VK_ADD_LAYER_PATH and the tests would exercise the package: implicit layers are
# disabled for the tests, except the device selector MESA_VK_DEVICE_SELECT needs.
set(AFMF_TEST_LOADER_ENV "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1;VK_LOADER_LAYERS_DISABLE=~implicit~;VK_LOADER_LAYERS_ENABLE=VK_LAYER_MESA_device_select")

add_executable(afmf_headless
    tests/headless.c
    tests/headless_checks.c
    tests/headless_device.c
    tests/headless_frame.c
    tests/headless_instance.c
    tests/headless_swapchain.c
)
target_link_libraries(afmf_headless PRIVATE Vulkan::Vulkan)
target_compile_definitions(afmf_headless PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(afmf_headless PRIVATE ${AFMF_WARNINGS})

# 120 presents: the first has no history, every other one must have got a generated companion,
# and the dumped companions must show the synthetic square halfway between the real frames.
# The acquire timeout is raised and the presents paced at 2 ms because the test checks generation,
# not the never-stall policy: the headless WSI signals a spare's release asynchronously, the
# presentation thread needs a moment to hand images back, and a game presents milliseconds apart,
# not microseconds (unpaced, 1 present in 120 finds no spare on a busy machine).
add_test(NAME headless COMMAND afmf_headless)
set_tests_properties(headless PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf;AFMF_PERFORMANCE_MODE=quality;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL"
    SKIP_RETURN_CODE 77
)
# Same check with the optical flow at half resolution (16-pixel blocks on screen).
add_test(NAME headless_performance COMMAND afmf_headless)
set_tests_properties(headless_performance PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf-performance;AFMF_PERFORMANCE_MODE=performance;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL"
    SKIP_RETURN_CODE 77
)

# The application holds every compute queue (vkd3d-proton does): the layer must share the last one,
# serialising the application's submits on it, and still generate every frame.
add_test(NAME headless_shared_queue COMMAND afmf_headless)
set_tests_properties(headless_shared_queue PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf-shared;AFMF_TEST_ALL_QUEUES=1;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|layer queue on family|no compute queue to use"
    SKIP_RETURN_CODE 77
)

# Under Gamescope (AFMF_GAMESCOPE=1) the layer asks for five extra images instead of two, and
# still generates every frame.
add_test(NAME headless_gamescope COMMAND afmf_headless)
set_tests_properties(headless_gamescope PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf-gamescope;AFMF_GAMESCOPE=1;AFMF_TEST_EXTRA_IMAGES=5;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|expected at least"
    SKIP_RETURN_CODE 77
)

# The same variable reaches gamescope itself, which must get no companions: the test binary runs
# under that name (the layer looks at /proc/self/exe, so a copy, not a symlink) and every
# swapchain of it passes through.
add_custom_command(TARGET afmf_headless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/tests-as
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:afmf_headless> ${CMAKE_BINARY_DIR}/tests-as/gamescope
    VERBATIM)
add_test(NAME headless_gamescope_passive COMMAND ${CMAKE_BINARY_DIR}/tests-as/gamescope)
set_tests_properties(headless_gamescope_passive PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_GAMESCOPE=1;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "pass-through \\(this process is Gamescope itself"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|generation on"
    SKIP_RETURN_CODE 77
)

# Below AFMF_MIN_FPS (30 by default) no companion is made: at 25 fps the doubling is not worth
# its latency. The governor's other steps need a starved GPU and are checked by hand.
add_test(NAME headless_min_fps COMMAND afmf_headless)
set_tests_properties(headless_min_fps PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_TEST_FRAME_MS=40;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 0 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL"
    SKIP_RETURN_CODE 77
)

# Present ids (VK_KHR_present_id and present_id2, what DXVK attaches) must be carried on the
# real frame by the presentation thread, never make the presents inline.
foreach(id 1 2)
    add_test(NAME headless_present_id${id} COMMAND afmf_headless)
    set_tests_properties(headless_present_id${id} PROPERTIES
        ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_TEST_PRESENT_ID=${id};AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
        PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
        FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|presenting inline"
        SKIP_RETURN_CODE 77
    )
endforeach()

# AFMF_PRESENT_MODE: the FIFO swapchain the test creates comes out in MAILBOX (headless offers
# it), also with VK_EXT_swapchain_maintenance1's FIFO-only mode list and per-present FIFO on
# every present (both must be rewritten, or validation rejects the presents); `keep` leaves FIFO.
add_test(NAME headless_present_mode_auto COMMAND afmf_headless)
set_tests_properties(headless_present_mode_auto PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_TEST_PRESENT_MODES=1;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|present mode 2|presenting inline"
    SKIP_RETURN_CODE 77
)
# AFMF_DIRECT_OUTPUT=1: the interpolator writes the swapchain image itself (the headless surface
# gives R8G8B8A8_UNORM, which takes storage writes); the golden check reads that image.
add_test(NAME headless_direct_output COMMAND afmf_headless)
set_tests_properties(headless_direct_output PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_DIRECT_OUTPUT=1;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf-direct;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|direct output unavailable"
    SKIP_RETURN_CODE 77
)
# HUD detection: a bar that never moves, drawn over the moving square, must come out whole in the
# generated frames; with the detection off the square's motion warps it (the negative control:
# the harness must report the bar broken, or the check is not looking at anything).
add_test(NAME headless_hud COMMAND afmf_headless)
set_tests_properties(headless_hud PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_TEST_HUD_BAR=1;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf-hud;AFMF_PERFORMANCE_MODE=quality;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|bar broken"
    SKIP_RETURN_CODE 77
)
add_test(NAME headless_hud_negative COMMAND afmf_headless)
set_tests_properties(headless_hud_negative PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_TEST_HUD_BAR=1;AFMF_HUD_DETECT=0;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf-hud-off;AFMF_PERFORMANCE_MODE=quality;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "bar broken"
    FAIL_REGULAR_EXPRESSION "validation error|bar intact"
    SKIP_RETURN_CODE 77
)
# A full-frame pan: every block moves, so the search's prediction early-out (a block whose vector
# from the coarser level already matches keeps it) must still leave (-8, 0) on nearly every block.
add_test(NAME headless_pan COMMAND afmf_headless)
set_tests_properties(headless_pan PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_TEST_PAN=1;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf-pan;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|MISMATCH"
    SKIP_RETURN_CODE 77
)
# The same pan with the SDK's byte-at-a-time SAD (AFMF_SAD_INT16=0): both SADs must agree on
# the field, since the packed one is the default wherever shaderInt16 exists.
add_test(NAME headless_pan_scalar_sad COMMAND afmf_headless)
set_tests_properties(headless_pan_scalar_sad PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_TEST_PAN=1;AFMF_SAD_INT16=0;AFMF_DUMP_DIR=${CMAKE_BINARY_DIR}/Testing/afmf-pan-scalar;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|MISMATCH"
    SKIP_RETURN_CODE 77
)
# The search's worst case for cost, and the scene none of the tests above has: a scroll with
# almost no contrast, where no block finds evidence to skip its search and nearly all of them
# run the full 256 candidates. Long enough (400 presents) for the layer's own profile report,
# which comes every 300 generated frames and is the per-stage breakdown to read here.
add_test(NAME headless_fog COMMAND afmf_headless)
set_tests_properties(headless_fog PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=2;AFMF_PROFILE=1;AFMF_TEST_FOG=1;AFMF_TEST_FRAMES=400;AFMF_GOVERNOR=0;AFMF_ACQUIRE_TIMEOUT_US=16000;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "presented 400 frames through"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL"
    SKIP_RETURN_CODE 77
    TIMEOUT 120
)
# The first cost gate: the full-frame pan at the resolution the register measures, with a ceiling
# on the wall time per present. The figure carries the test's own upload and a queue drain, so
# the ceiling sits at twice what a healthy build takes: it catches the change that doubles the
# layer's GPU work, not a few per cent. The per-stage GPU numbers are in the profile lines.
add_test(NAME headless_cost COMMAND afmf_headless)
set_tests_properties(headless_cost PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=2;AFMF_PROFILE=1;AFMF_TEST_PAN=1;AFMF_TEST_EXTENT=3440x1440;AFMF_TEST_FRAMES=400;AFMF_TEST_MAX_MS=32;AFMF_GOVERNOR=0;AFMF_ACQUIRE_TIMEOUT_US=16000;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "presented 400 frames through"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL"
    SKIP_RETURN_CODE 77
    TIMEOUT 120
)
# Smoothness: a game at 8 ms with a stutter of 40 ms every 25 frames, long enough for two of the
# layer's cadence lines. No present may lose its companion over a hitch, and the governor, which
# only reads how late the generated frame was, must be at step 0 in both lines.
add_test(NAME headless_hitch COMMAND afmf_headless)
set_tests_properties(headless_hitch PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=2;AFMF_PROFILE=1;AFMF_TEST_FRAME_MS=8;AFMF_TEST_HITCH=25;AFMF_TEST_FRAMES=600;AFMF_GOVERNOR=1;AFMF_ACQUIRE_TIMEOUT_US=16000;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "presented 600 frames through"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL| [1-9][0-9]* no free image|governor step [1-3]"
    SKIP_RETURN_CODE 77
    TIMEOUT 120
)
add_test(NAME headless_present_mode_keep COMMAND afmf_headless)
set_tests_properties(headless_present_mode_keep PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_PRESENT_MODE=keep;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "present mode 2 \\(app asked 2\\)"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|present mode 1"
    SKIP_RETURN_CODE 77
)
# Teardown with frames still in the layer's threads: unpaced presents keep the work and the
# presentation thread busy, and the swapchain is destroyed without the vkDeviceWaitIdle that
# would drain them first. Its destruction must drain both, without a deadlock or a leak (the
# sanitizer build runs this too).
add_test(NAME headless_async_drain COMMAND afmf_headless)
set_tests_properties(headless_async_drain PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3;AFMF_TEST_NO_IDLE=1;AFMF_ACQUIRE_TIMEOUT_US=16000;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "destroyed after 120 presents"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL|still alive at device destruction"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
)
# What a session looks like and the tests above never did: thousands of presents, and every 250
# of them the swapchain goes down and comes back at another resolution while the layer's threads
# are still working. Everything the layer allocated per swapchain is built and destroyed a dozen
# times over, under the validation layer, which is where a use-after-free or a stale descriptor
# shows up.
add_test(NAME headless_soak COMMAND afmf_headless)
set_tests_properties(headless_soak PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=2;AFMF_TEST_FRAMES=3000;AFMF_TEST_RECREATE=250;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=1;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "presented 3000 frames"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL"
    SKIP_RETURN_CODE 77
    TIMEOUT 300
)
# The validation layer instruments the shaders themselves: a read or a write of theirs that
# leaves its buffer is reported instead of corrupting whatever sits next to it, which is the
# shape a crash takes in a game and the shape no test above can see. Slow, so it runs short.
add_test(NAME headless_gpuav COMMAND afmf_headless)
set_tests_properties(headless_gpuav PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=2;AFMF_TEST_GPUAV=1;AFMF_TEST_PAN=1;AFMF_GOVERNOR=0;AFMF_ACQUIRE_TIMEOUT_US=16000;AFMF_TEST_FRAME_MS=2;${AFMF_TEST_LOADER_ENV}"
    PASS_REGULAR_EXPRESSION "presented 120 frames through"
    FAIL_REGULAR_EXPRESSION "validation error|headless: FAIL"
    SKIP_RETURN_CODE 77
    TIMEOUT 300
)

if(AFMF_SANITIZE)
    foreach(target afmf-linux afmf_headless)
        target_compile_options(${target} PRIVATE ${AFMF_SANITIZE_FLAGS})
        target_link_options(${target} PRIVATE ${AFMF_SANITIZE_FLAGS})
    endforeach()
endif()
