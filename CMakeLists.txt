cmake_minimum_required(VERSION 3.16)
project(rs_rtsp
        VERSION 0.1.0
        DESCRIPTION "RTSP server for newosp: H.264/H.265 over RTP"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(NEWOSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../newosp" CACHE PATH "newosp root")
if(NOT EXISTS "${NEWOSP_DIR}/include/osp/event_loop.hpp")
  message(FATAL_ERROR "NEWOSP_DIR does not point at a newosp checkout: ${NEWOSP_DIR}")
endif()

set(RS_MAX_CONNECTIONS 16 CACHE STRING "Compile-time connection capacity (4..16)")
if(NOT RS_MAX_CONNECTIONS MATCHES "^([4-9]|1[0-6])$")
  message(FATAL_ERROR "RS_MAX_CONNECTIONS must be an integer from 4 to 16")
endif()

set(OSP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OSP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OSP_WITH_NETWORK ON CACHE BOOL "" FORCE)
set(OSP_CONFIG_INI OFF CACHE BOOL "" FORCE)
add_subdirectory("${NEWOSP_DIR}" "${CMAKE_BINARY_DIR}/newosp")

# Optional newosp extensions used when the checked-out baseline provides them.
# The upstream 0.9.6 tag lacks these, so the server keeps a portable fallback.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${NEWOSP_DIR}/include")
check_cxx_source_compiles("#include \"osp/io_poller.hpp\"
int main() { auto v = osp::IoEvent::kRdhup; (void)v; return 0; }" RS_HAVE_NEWOSP_RDHUP)
check_cxx_source_compiles("#include \"osp/socket.hpp\"
int main() { auto fn = &osp::TcpListener::Accept4; (void)fn; return 0; }" RS_HAVE_NEWOSP_ACCEPT4)
check_cxx_source_compiles("#include \"osp/event_loop.hpp\"
struct D : osp::EventLoop<D> {};
int main() { auto fn = &D::HasFailed; (void)fn; return 0; }" RS_HAVE_NEWOSP_HASFAILED)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  unset(RS_HAVE_NEWOSP_CHECKED_THREAD CACHE)
  check_cxx_source_compiles("#include \"osp/thread.hpp\"
#if !defined(OSP_THREAD_POSIX_CHECKED_START) || OSP_THREAD_POSIX_CHECKED_START != 1
#error newosp POSIX checked thread startup is required
#endif
int main() { return 0; }" RS_HAVE_NEWOSP_CHECKED_THREAD)
  if(NOT RS_HAVE_NEWOSP_CHECKED_THREAD)
    message(FATAL_ERROR "newosp must provide POSIX checked thread startup (OSP_THREAD_POSIX_CHECKED_START)")
  endif()
endif()
unset(CMAKE_REQUIRED_INCLUDES)

option(RS_NO_EXCEPTIONS "Build with -fno-exceptions -fno-rtti" ON)
option(RS_SANITIZE "Build with ASan+UBSan" OFF)
option(RS_BUILD_E2E "Build end-to-end tests that spawn the demo server" ON)
option(RS_ENABLE_DEBUG_SHELL "Build the telnet debug shell (P0/P1 commands)" OFF)
option(RS_ENABLE_FAULT_INJECTION "Build test-only fault injection points" OFF)

set(RS_WARN_FLAGS -Wall -Wextra -Werror -Wpedantic)
if(RS_NO_EXCEPTIONS)
  list(APPEND RS_WARN_FLAGS -fno-exceptions -fno-rtti)
endif()
if(RS_SANITIZE)
  list(APPEND RS_WARN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(rs_core STATIC
  src/rtsp_parser.cpp
  src/sdp.cpp
  src/codec.cpp
  src/file_source.cpp
  src/media_sender.cpp
  src/h264_packetizer.cpp
  src/h265_packetizer.cpp
  src/rtcp.cpp
  src/frame_ingress.cpp
  src/rtp_transport.cpp
  src/session_hsm.cpp
  src/rtsp_session.cpp
  src/rtsp_server.cpp
)
target_include_directories(rs_core PUBLIC include)
target_link_libraries(rs_core PUBLIC osp)
target_compile_options(rs_core PRIVATE ${RS_WARN_FLAGS})
# The version comes from project(VERSION) so that CMake, the git tag and the
# runtime string reported by the debug shell can never drift apart.
target_compile_definitions(rs_core PUBLIC OSP_HAS_NETWORK=1 RS_VERSION="${PROJECT_VERSION}"
  RS_MAX_CONNECTIONS=${RS_MAX_CONNECTIONS})
if(RS_HAVE_NEWOSP_RDHUP)
  target_compile_definitions(rs_core PUBLIC RS_HAVE_NEWOSP_RDHUP=1)
endif()
if(RS_HAVE_NEWOSP_ACCEPT4)
  target_compile_definitions(rs_core PUBLIC RS_HAVE_NEWOSP_ACCEPT4=1)
endif()
if(RS_HAVE_NEWOSP_HASFAILED)
  target_compile_definitions(rs_core PUBLIC RS_HAVE_NEWOSP_HASFAILED=1)
endif()

add_executable(rtspserver_demo src/main.cpp)
target_link_libraries(rtspserver_demo PRIVATE rs_core)
target_compile_options(rtspserver_demo PRIVATE ${RS_WARN_FLAGS})
if(RS_ENABLE_DEBUG_SHELL)
  target_sources(rtspserver_demo PRIVATE src/debug_shell.cpp)
  target_compile_definitions(rtspserver_demo PRIVATE RS_ENABLE_DEBUG_SHELL=1)
endif()
if(RS_ENABLE_FAULT_INJECTION)
  target_compile_definitions(rs_core PUBLIC RS_ENABLE_FAULT_INJECTION=1)
endif()

add_executable(make_test_stream tools/make_test_stream.cpp)
target_link_libraries(make_test_stream PRIVATE rs_core)

add_executable(encoder_source_demo examples/encoder_source_demo.cpp)
target_link_libraries(encoder_source_demo PRIVATE rs_core)
target_compile_options(encoder_source_demo PRIVATE ${RS_WARN_FLAGS})

enable_testing()
function(rs_add_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE rs_core)
  target_compile_options(${name} PRIVATE ${RS_WARN_FLAGS})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

rs_add_test(test_rtsp_parser)
rs_add_test(test_sdp)
rs_add_test(test_h264_packetizer)
rs_add_test(test_h265_packetizer)
rs_add_test(test_rtcp)
rs_add_test(test_frame_ingress)
rs_add_test(test_hsm)
rs_add_test(test_rtsp_session)
rs_add_test(test_server_external)
target_sources(test_server_external PRIVATE tests/e2e_client.cpp)
rs_add_test(test_ip_rate_limiter)
rs_add_test(test_alignment)
rs_add_test(test_rtp_transport)
rs_add_test(test_track_params)

# Negative compile contracts: these translation units MUST NOT compile.
# EXCLUDE_FROM_ALL keeps them out of the default build; the WILL_FAIL ctest
# builds the target on demand and turns the expected build failure into a
# pass. The same trick as coact's src/event/CMakeLists.txt.
#
# The two contracts guarded here are the ones that protect every translation
# unit in the library: no exceptions and no RTTI. A regression that drops
# -fno-exceptions or -fno-rtti would otherwise go unnoticed until an
# embedded target failed to link.
function(rs_add_neg_test name)
  add_library(${name} EXCLUDE_FROM_ALL STATIC tests/neg/${name}.cpp)
  target_link_libraries(${name} PRIVATE rs_core)
  target_compile_options(${name} PRIVATE ${RS_WARN_FLAGS})
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${name})
  set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
endfunction()

rs_add_neg_test(exceptions_neg)
rs_add_neg_test(rtti_neg)

if(RS_BUILD_E2E)
  add_executable(test_e2e tests/test_e2e.cpp tests/e2e_client.cpp)
  target_link_libraries(test_e2e PRIVATE rs_core)
  target_compile_options(test_e2e PRIVATE ${RS_WARN_FLAGS})
  target_compile_definitions(test_e2e PRIVATE
    RS_DEMO_BIN="$<TARGET_FILE:rtspserver_demo>"
    RS_STREAM_TOOL="$<TARGET_FILE:make_test_stream>")
  add_dependencies(test_e2e rtspserver_demo make_test_stream)
  add_test(NAME test_e2e COMMAND test_e2e)
  set_tests_properties(test_e2e PROPERTIES TIMEOUT 300)
endif()
