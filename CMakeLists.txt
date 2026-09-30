cmake_minimum_required(VERSION 3.21)
project(spore VERSION 0.1.0 LANGUAGES C)

# ---- options ---------------------------------------------------------------
# The core (HTTP/1.1, access policy, static files, JSON) is always built.
option(SPORE_WS  "WebSocket module (spore_ws.h)" ON)
option(SPORE_LLM "OpenAI-compatible LLM endpoints (spore_llm.h)" ON)
include(CMakeDependentOption)
cmake_dependent_option(SPORE_REALTIME
  "OpenAI Realtime API over WebSocket (spore_realtime.h); needs SPORE_WS" ON SPORE_WS OFF)
option(SPORE_BUILD_EXAMPLES "Build spored" ${PROJECT_IS_TOP_LEVEL})
option(SPORE_BUILD_TESTS "Build tests (needs examples for integration)" ${PROJECT_IS_TOP_LEVEL})
option(SPORE_FUZZ "Build libFuzzer targets (clang only)" OFF)
set(SPORE_SANITIZE "" CACHE STRING
    "Sanitizers for every target: address,undefined or thread")
set(SPORE_LLAMA_DIR "" CACHE PATH
    "llama.cpp tree with include/ and lib/ for the llama backend; empty disables it")
set(SPORE_WHISPER_DIR "" CACHE PATH
    "whisper.cpp tree with include/ and lib/ for the whisper backend; empty disables it")
set(SPORE_TEST_MODELS "" CACHE PATH
    "Directory of models for the engine tests (make test-engines)")
set(SPORE_ASR_SAMPLE "" CACHE FILEPATH
    "16 kHz mono WAV of known speech for the ASR test (whisper.cpp's samples/jfk.wav)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
include(GNUInstallDirs)

if(SPORE_SANITIZE)
  add_compile_options(-fsanitize=${SPORE_SANITIZE} -fno-omit-frame-pointer -O1)
  add_link_options(-fsanitize=${SPORE_SANITIZE})
endif()

set(SPORE_WARN)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(SPORE_WARN -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)
endif()

function(spore_c_target target)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE ${SPORE_WARN})
endfunction()

# ---- library ---------------------------------------------------------------

set(SPORE_CORE_SRC src/buf.c src/http.c src/json.c src/server.c src/static.c)
set(SPORE_WS_SRC   src/sha1.c src/ws.c)
set(SPORE_LLM_SRC  src/llm.c)
set(SPORE_RT_SRC   src/realtime.c src/rt_audio.c)


set(SPORE_SRC ${SPORE_CORE_SRC})
set(SPORE_HEADERS include/spore.h include/spore_json.h)
if(SPORE_WS)
  list(APPEND SPORE_SRC ${SPORE_WS_SRC})
  list(APPEND SPORE_HEADERS include/spore_ws.h)
endif()
if(SPORE_LLM)
  list(APPEND SPORE_SRC ${SPORE_LLM_SRC})
endif()
if(SPORE_REALTIME)
  list(APPEND SPORE_SRC ${SPORE_RT_SRC})
  list(APPEND SPORE_HEADERS include/spore_realtime.h)
endif()
if(SPORE_LLM OR SPORE_REALTIME) # realtime uses the backend types only
  list(APPEND SPORE_HEADERS include/spore_llm.h)
endif()

add_library(spore ${SPORE_SRC})
add_library(spore::spore ALIAS spore)
spore_c_target(spore)
target_include_directories(spore
  PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src)
target_link_libraries(spore PUBLIC Threads::Threads m)
# Consumers can test for modules with #ifdef SPORE_WITH_WS / SPORE_WITH_LLM.
target_compile_definitions(spore PUBLIC
  $<$<BOOL:${SPORE_WS}>:SPORE_WITH_WS>
  $<$<BOOL:${SPORE_LLM}>:SPORE_WITH_LLM>
  $<$<BOOL:${SPORE_REALTIME}>:SPORE_WITH_REALTIME>)

include(CMakePackageConfigHelpers)
install(TARGETS spore EXPORT spore-targets)
install(FILES ${SPORE_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT spore-targets NAMESPACE spore::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spore)
file(WRITE ${PROJECT_BINARY_DIR}/spore-config.cmake
  "include(CMakeFindDependencyMacro)\n"
  "find_dependency(Threads)\n"
  "include(\${CMAKE_CURRENT_LIST_DIR}/spore-targets.cmake)\n")
write_basic_package_version_file(${PROJECT_BINARY_DIR}/spore-config-version.cmake
  COMPATIBILITY SameMinorVersion)
install(FILES ${PROJECT_BINARY_DIR}/spore-config.cmake
              ${PROJECT_BINARY_DIR}/spore-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spore)

# ---- spored ------------------------------------------------------------------

if(SPORE_BUILD_EXAMPLES)
  set(SPORED_SRC examples/spored.c)
  if(SPORE_LLM OR SPORE_REALTIME)
    list(APPEND SPORED_SRC examples/echo_backend.c)
  endif()
  if(SPORE_REALTIME)
    list(APPEND SPORED_SRC examples/rt_mock_backend.c)
  endif()
  add_executable(spored ${SPORED_SRC})
  spore_c_target(spored)
  target_include_directories(spored PRIVATE examples)
  target_link_libraries(spored PRIVATE spore)

  # ---- engine adapters (backends/) -------------------------------------
  # Each adapter links one engine; spored-engines combines those configured.
  # ggml is linked once: llama.cpp's copy when present, else whisper.cpp's.
  # whisper.cpp built against ggml 0.23 links against llama.cpp's 0.25:
  # the ggml_tensor layout and the precision API it uses are unchanged.
  set(engine_defs)
  set(engine_libs)
  if(SPORE_LLAMA_DIR OR SPORE_WHISPER_DIR)
    enable_language(CXX)
    find_package(OpenMP REQUIRED COMPONENTS CXX)
    if(SPORE_LLAMA_DIR)
      set(ggml_dir ${SPORE_LLAMA_DIR})
    else()
      set(ggml_dir ${SPORE_WHISPER_DIR})
    endif()
    function(spore_require_lib path)
      if(NOT EXISTS ${path})
        message(FATAL_ERROR "missing ${path}")
      endif()
    endfunction()
    add_library(spore_ggml INTERFACE)
    foreach(lib ggml ggml-cpu ggml-base)
      spore_require_lib(${ggml_dir}/lib/lib${lib}.a)
      target_link_libraries(spore_ggml INTERFACE ${ggml_dir}/lib/lib${lib}.a)
    endforeach()
    # GPU backends, when the ggml build has them (same detection as chimera).
    if(EXISTS ${ggml_dir}/lib/libggml-cuda.a)
      find_package(CUDAToolkit REQUIRED)
      target_link_libraries(spore_ggml INTERFACE ${ggml_dir}/lib/libggml-cuda.a
        CUDA::cudart CUDA::cublas CUDA::cuda_driver)
      message(STATUS "spore engines: CUDA backend")
      set(SPORE_ENGINE_GPU 1)
    endif()
    if(EXISTS ${ggml_dir}/lib/libggml-vulkan.a)
      find_package(Vulkan REQUIRED)
      target_link_libraries(spore_ggml INTERFACE ${ggml_dir}/lib/libggml-vulkan.a Vulkan::Vulkan)
      message(STATUS "spore engines: Vulkan backend")
      set(SPORE_ENGINE_GPU 1)
    endif()
    target_link_libraries(spore_ggml INTERFACE OpenMP::OpenMP_CXX ${CMAKE_DL_LIBS})
  endif()

  if(SPORE_LLAMA_DIR)
    if(NOT SPORE_LLM AND NOT SPORE_REALTIME)
      message(FATAL_ERROR "SPORE_LLAMA_DIR needs SPORE_LLM or SPORE_REALTIME")
    endif()
    spore_require_lib(${SPORE_LLAMA_DIR}/lib/libllama.a)
    add_library(spore_llama STATIC backends/llama/spore_llama.cpp)
    set_target_properties(spore_llama PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_options(spore_llama PRIVATE -Wall -Wextra)
    target_include_directories(spore_llama
      PUBLIC backends/llama PRIVATE ${SPORE_LLAMA_DIR}/include)
    target_link_libraries(spore_llama PUBLIC spore
      ${SPORE_LLAMA_DIR}/lib/libllama.a spore_ggml)
    list(APPEND engine_defs SPORE_WITH_LLAMA)
    list(APPEND engine_libs spore_llama)
  endif()

  # OuteTTS runs on llama.cpp too, so it comes with SPORE_LLAMA_DIR.
  if(SPORE_LLAMA_DIR AND SPORE_REALTIME)
    add_library(spore_outetts STATIC backends/outetts/spore_outetts.cpp)
    set_target_properties(spore_outetts PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_options(spore_outetts PRIVATE -Wall -Wextra)
    target_include_directories(spore_outetts
      PUBLIC backends/outetts PRIVATE ${SPORE_LLAMA_DIR}/include)
    target_link_libraries(spore_outetts PUBLIC spore
      ${SPORE_LLAMA_DIR}/lib/libllama.a spore_ggml Threads::Threads)
    list(APPEND engine_defs SPORE_WITH_OUTETTS)
    list(APPEND engine_libs spore_outetts)
  endif()

  if(SPORE_WHISPER_DIR)
    if(NOT SPORE_REALTIME)
      message(FATAL_ERROR "SPORE_WHISPER_DIR needs SPORE_REALTIME")
    endif()
    spore_require_lib(${SPORE_WHISPER_DIR}/lib/libwhisper.a)
    add_library(spore_whisper STATIC backends/whisper/spore_whisper.c)
    spore_c_target(spore_whisper)
    # whisper.cpp's headers are not our code: include them as system headers.
    target_include_directories(spore_whisper PUBLIC backends/whisper)
    target_include_directories(spore_whisper SYSTEM PRIVATE ${SPORE_WHISPER_DIR}/include)
    target_link_libraries(spore_whisper PUBLIC spore
      ${SPORE_WHISPER_DIR}/lib/libwhisper.a spore_ggml)
    list(APPEND engine_defs SPORE_WITH_WHISPER)
    list(APPEND engine_libs spore_whisper)
  endif()

  if(engine_libs)
    add_executable(spored-engines ${SPORED_SRC})
    spore_c_target(spored-engines)
    target_compile_definitions(spored-engines PRIVATE ${engine_defs})
    target_include_directories(spored-engines PRIVATE examples)
    target_link_libraries(spored-engines PRIVATE ${engine_libs})
    set_target_properties(spored-engines PROPERTIES LINKER_LANGUAGE CXX)
  endif()
endif()

# ---- tests -------------------------------------------------------------------

if(SPORE_BUILD_TESTS)
  enable_testing()

  add_executable(test_unit tests/test_unit.c)
  spore_c_target(test_unit)
  target_include_directories(test_unit PRIVATE src)
  target_link_libraries(test_unit PRIVATE spore)
  add_test(NAME unit COMMAND test_unit)

  # Replay the fuzz seed corpus without libFuzzer, under any compiler.
  set(replay_targets http json)
  if(SPORE_WS)
    list(APPEND replay_targets ws)
  endif()
  foreach(t ${replay_targets})
    add_executable(replay_${t} tests/fuzz/fuzz_${t}.c tests/fuzz/replay.c)
    set_target_properties(replay_${t} PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(replay_${t} PRIVATE src)
    target_link_libraries(replay_${t} PRIVATE spore)
    file(GLOB seeds ${PROJECT_SOURCE_DIR}/tests/fuzz/seeds/${t}/*)
    add_test(NAME replay_${t} COMMAND replay_${t} ${seeds})
  endforeach()

  find_program(UV uv)
  if(SPORE_BUILD_EXAMPLES AND UV)
    add_test(NAME integration
      COMMAND ${UV} run --no-project --with pytest --with "openai[realtime]" --with websockets
              pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tests)
    set_tests_properties(integration PROPERTIES
      ENVIRONMENT "SPORED=$<TARGET_FILE:spored>")

    # Real engines and models: excluded from `make test`, run by `make test-engines`.
    if(TARGET spored-engines AND SPORE_TEST_MODELS)
      set(m ${SPORE_TEST_MODELS})
      add_test(NAME engines
        COMMAND ${UV} run --no-project --with pytest --with "openai[realtime]" --with websockets
                pytest -q -p no:cacheprovider
                ${PROJECT_SOURCE_DIR}/tests/test_llama.py ${PROJECT_SOURCE_DIR}/tests/test_engines.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tests)
      set_tests_properties(engines PROPERTIES LABELS engines ENVIRONMENT
        "SPORED=$<TARGET_FILE:spored>;SPORED_ENGINES=$<TARGET_FILE:spored-engines>;SPORE_CHAT_MODEL=${m}/Qwen3-0.6B-Q8_0.gguf;SPORE_EMBED_MODEL=${m}/bge-small-en-v1.5-q8_0.gguf;SPORE_VOICE_LLM=${m}/Llama-3.2-1B-Instruct-Q8_0.gguf;SPORE_ASR_MODEL=${m}/ggml-base.en.bin;SPORE_ASR_SAMPLE=${SPORE_ASR_SAMPLE};SPORE_TTS_MODEL=${m}/OuteTTS-0.3-1B-Q6_K.gguf;SPORE_VOCODER=${m}/WavTokenizer-Large-75-F16.gguf;SPORE_ENGINE_GPU=${SPORE_ENGINE_GPU}")
    endif()
  elseif(SPORE_BUILD_EXAMPLES)
    message(STATUS "uv not found: integration tests disabled")
  endif()
endif()

# ---- libFuzzer ---------------------------------------------------------------
# Every source is instrumented, so the targets compile the sources directly.

if(SPORE_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SPORE_FUZZ needs clang (e.g. -DCMAKE_C_COMPILER=clang)")
  endif()
  foreach(t http json ws)
    add_executable(fuzz_${t} tests/fuzz/fuzz_${t}.c ${SPORE_CORE_SRC} ${SPORE_WS_SRC})
    set_target_properties(fuzz_${t} PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(fuzz_${t} PRIVATE include src)
    target_compile_options(fuzz_${t} PRIVATE -g -O1
      -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
    target_link_options(fuzz_${t} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz_${t} PRIVATE Threads::Threads m)
  endforeach()
endif()
