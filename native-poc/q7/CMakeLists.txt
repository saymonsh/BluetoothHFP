cmake_minimum_required(VERSION 3.20)
project(q7_bridge LANGUAGES C CXX)
if(NOT BTSTACK_ROOT OR NOT PATCHED_TRANSPORT OR NOT PATCHED_HCI)
  message(FATAL_ERROR "Configure through native-poc/tools/Build-Q7.ps1 (needs BTSTACK_ROOT and PATCHED_TRANSPORT)")
endif()
set(R ${BTSTACK_ROOT})

# Only what the bridge needs: HCI, L2CAP, RFCOMM, SDP, HFP HF+AG, CVSD PLC.
add_library(btstack_min STATIC
  ${R}/src/btstack_linked_list.c ${R}/src/btstack_memory.c ${R}/src/btstack_memory_pool.c
  ${R}/src/btstack_run_loop.c ${R}/src/btstack_util.c ${R}/src/btstack_tlv.c ${R}/src/ad_parser.c
  ${PATCHED_HCI} ${R}/src/hci_cmd.c ${R}/src/hci_dump.c ${R}/src/l2cap.c ${R}/src/l2cap_signaling.c
  ${R}/src/classic/rfcomm.c ${R}/src/classic/sdp_server.c ${R}/src/classic/sdp_client.c
  ${R}/src/classic/sdp_client_rfcomm.c ${R}/src/classic/sdp_util.c
  ${R}/src/classic/hfp.c ${R}/src/classic/hfp_ag.c ${R}/src/classic/hfp_hf.c ${R}/src/classic/hfp_gsm_model.c
  ${R}/src/classic/btstack_cvsd_plc.c ${R}/src/classic/btstack_link_key_db_tlv.c
  ${R}/platform/windows/btstack_run_loop_windows.c ${R}/platform/windows/btstack_tlv_windows.c
  ${R}/platform/windows/btstack_stdin_windows.c ${R}/platform/windows/hci_dump_windows_fs.c
  ${PATCHED_TRANSPORT})
target_include_directories(btstack_min PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${R}/src ${R}/platform/windows ${R}/platform/posix ${R}/platform/embedded)
target_compile_definitions(btstack_min PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(q7_bridge q7_bridge.c call_tap.cpp
  ../audio/audio_bridge.cpp ../audio/wasapi_endpoint.cpp ../audio/wasapi_capture.cpp ../audio/wasapi_render.cpp)
target_compile_features(q7_bridge PRIVATE cxx_std_17)
target_compile_definitions(q7_bridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_include_directories(q7_bridge PRIVATE .. ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q7_bridge PRIVATE btstack_min setupapi winusb ole32 uuid advapi32)
if(MSVC)
  target_compile_options(q7_bridge PRIVATE /utf-8)
endif()

enable_testing()
add_executable(pcm_fifo_tests tests/pcm_fifo_tests.c)
add_test(NAME pcm_fifo COMMAND pcm_fifo_tests)
