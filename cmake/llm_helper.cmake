# The language-model helper: llama.cpp's server, built from source in its own CMake run (so its
# options never touch the rest of VCSIDE) and shipped next to the IDE. VCSIDE starts it on demand
# on a loopback port and stops it when the assistant is closed; the user never sees a server.
# llama.cpp and ggml are MIT licensed (see licenses/llama.cpp-MIT.txt).
include(ExternalProject)

set(VCSIDE_LLAMA_TAG b12200 CACHE STRING "llama.cpp release tag used for the helper")
set(VCSIDE_LLM_PREFIX ${CMAKE_BINARY_DIR}/llm-helper)

set(VCSIDE_LLM_ARGS
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=${VCSIDE_LLM_PREFIX}
  -DLLAMA_BUILD_TESTS=OFF
  -DLLAMA_BUILD_EXAMPLES=OFF
  -DLLAMA_BUILD_SERVER=ON
  -DLLAMA_CURL=OFF
  -DLLAMA_OPENSSL=OFF
  -DGGML_CCACHE=OFF)

if(APPLE)
  # One self-contained executable: Metal for the GPU, the CPU kernels built in.
  list(APPEND VCSIDE_LLM_ARGS -DBUILD_SHARED_LIBS=OFF -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON
       -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})
else()
  # One install that works on any machine: each backend is its own library, loaded at run time,
  # and the best CPU variant is picked on start. Vulkan covers NVIDIA, AMD and Intel GPUs.
  list(APPEND VCSIDE_LLM_ARGS -DBUILD_SHARED_LIBS=ON -DGGML_BACKEND_DL=ON -DGGML_NATIVE=OFF
       -DGGML_CPU_ALL_VARIANTS=ON "-DCMAKE_INSTALL_RPATH=$ORIGIN")
  find_package(Vulkan QUIET COMPONENTS glslc)
  if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
    list(APPEND VCSIDE_LLM_ARGS -DGGML_VULKAN=ON)
  else()
    message(STATUS "Vulkan SDK (with glslc) not found: the language-model helper will run on the CPU only")
  endif()
endif()

ExternalProject_Add(vcside_llm_helper
  URL https://github.com/ggml-org/llama.cpp/archive/refs/tags/${VCSIDE_LLAMA_TAG}.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  CMAKE_ARGS ${VCSIDE_LLM_ARGS}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --target llama-server
  # The server and, outside macOS, the backend libraries it loads, all in bin/.
  INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory <BINARY_DIR>/bin ${VCSIDE_LLM_PREFIX}/bin
  INSTALL_BYPRODUCTS ${VCSIDE_LLM_PREFIX}/bin/llama-server)
