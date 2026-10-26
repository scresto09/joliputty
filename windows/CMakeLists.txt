set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_sources_from_current_dir(utils
  utils/agent_mutex_name.c
  utils/agent_named_pipe_name.c
  utils/arm_arch_queries.c
  utils/aux_match_opt.c
  utils/blink_time.c
  utils/centre_window.c
  utils/cmdline_arg.c
  utils/cryptoapi.c
  utils/defaults.c
  utils/dll_hijacking_protection.c
  utils/dputs.c
  utils/escape_registry_key.c
  utils/filename.c
  utils/fontspec.c
  utils/getdlgitemtext_alloc.c
  utils/get_system_dir.c
  utils/get_username.c
  utils/gui-timing.c
  utils/interprocess_mutex.c
  utils/is_console_handle.c
  utils/load_system32_dll.c
  utils/ltime.c
  utils/makedlgitemborderless.c
  utils/make_spr_sw_abort_winerror.c
  utils/message_box.c
  utils/minefield.c
  utils/open_for_write_would_lose_data.c
  utils/pgp_fingerprints_msgbox.c
  utils/platform_get_x_display.c
  utils/registry.c
  utils/request_file.c
  utils/screenshot.c
  utils/security.c
  utils/shinydialogbox.c
  utils/split_into_argv.c
  utils/split_into_argv_w.c
  utils/subprocess_waiter.c
  utils/version.c
  utils/win_strerror.c
  unicode.c)
if(NOT HAVE_STRTOUMAX)
  add_sources_from_current_dir(utils utils/strtoumax.c)
endif()
add_sources_from_current_dir(eventloop
  cliloop.c handle-wait.c)
add_sources_from_current_dir(console
  select-cli.c nohelp.c console.c)
add_sources_from_current_dir(settings
  storage.c)
add_sources_from_current_dir(network
  network.c handle-socket.c named-pipe-client.c named-pipe-server.c local-proxy.c x11.c)
add_sources_from_current_dir(sshcommon
  noise.c)
add_sources_from_current_dir(sshclient
  agent-client.c gss.c sharing.c)
add_sources_from_current_dir(sftpclient
  sftp.c)
add_sources_from_current_dir(otherbackends
  serial.c)
add_sources_from_current_dir(agent
  agent-client.c)
add_sources_from_current_dir(guiterminal
  dialog.c controls.c config.c printing.c jump-list.c sizetip.c)
add_dependencies(guiterminal generated_licence_h) # dialog.c uses licence.h

# This object awkwardly needs to live in the network library as well
# as the eventloop library, in case it didn't get pulled in from the
# latter before handle-socket.c needed it.
add_library(handle-io OBJECT
  handle-io.c)
target_sources(eventloop PRIVATE $<TARGET_OBJECTS:handle-io>)
target_sources(network PRIVATE $<TARGET_OBJECTS:handle-io>)

add_library(guimisc STATIC
  select-gui.c)

add_executable(pageant
  pageant.c
  help.c
  pageant.rc)
add_dependencies(pageant generated_licence_h)
target_link_libraries(pageant
  guimisc eventloop agent network crypto utils
  ${platform_libraries})
set_target_properties(pageant PROPERTIES WIN32_EXECUTABLE ON)
installed_program(pageant)

add_sources_from_current_dir(plink no-jump-list.c nohelp.c plink.rc)
add_dependencies(plink generated_licence_h)

add_sources_from_current_dir(pscp no-jump-list.c nohelp.c pscp.rc)
add_dependencies(pscp generated_licence_h)

add_sources_from_current_dir(psftp no-jump-list.c nohelp.c psftp.rc)
add_dependencies(psftp generated_licence_h)

add_sources_from_current_dir(psocks nohelp.c)

# PuTTY itself uses the WinUI 3 front end in the winui subdirectory
# (PuTTYtel and pterm still use the classic Win32 one in window.c).
include(${CMAKE_SOURCE_DIR}/cmake/winui.cmake)

if(TARGET winui_sdk)
  add_executable(putty
    winui/winui-window.c
    winui/winui-config.c
    winui/winui-sessions.c
    winui/winui-host.cpp
    winui/winui-dialog.cpp
    putty.c
    help.c
    ${CMAKE_SOURCE_DIR}/stubs/no-console.c
    putty.rc)
  # (this front end's name: see also WUI_APP_NAME in winui/wui.h)
  be_list(putty JoliPuTTY SSH SERIAL OTHERBACKENDS)
  add_dependencies(putty generated_licence_h)
  # (putty.c leaves the startup config box to the WinUI front end)
  target_compile_definitions(putty PRIVATE PUTTY_WINUI)
  target_link_libraries(putty
    guiterminal guimisc eventloop sshclient otherbackends settings network crypto
    utils winui_sdk
    ${platform_libraries})
  # (the program's called jputty.exe; the target keeps its name, which
  # the rest of the build refers to it by)
  set_target_properties(putty PROPERTIES
    OUTPUT_NAME jputty
    WIN32_EXECUTABLE ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
  winui_copy_runtime(putty)
  installed_program(putty)

  # (its version, from the git tags, checked at each build like
  # cmake_commit.c: see cmake/joliputty-version.cmake)
  set(GENERATED_JOLIPUTTY_VERSION_H
    ${GENERATED_SOURCES_DIR}/joliputty-version.h)
  set(INTERMEDIATE_JOLIPUTTY_VERSION_H ${GENERATED_JOLIPUTTY_VERSION_H}.tmp)
  add_custom_target(check_joliputty_version
    BYPRODUCTS ${INTERMEDIATE_JOLIPUTTY_VERSION_H}
    COMMAND ${CMAKE_COMMAND}
      -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
      -DOUTPUT_FILE=${INTERMEDIATE_JOLIPUTTY_VERSION_H}
      -P ${CMAKE_SOURCE_DIR}/cmake/joliputty-version.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/cmake/joliputty-version.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking JoliPuTTY's version")
  add_custom_target(joliputty_version_h
    BYPRODUCTS ${GENERATED_JOLIPUTTY_VERSION_H}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${INTERMEDIATE_JOLIPUTTY_VERSION_H} ${GENERATED_JOLIPUTTY_VERSION_H}
    DEPENDS check_joliputty_version ${INTERMEDIATE_JOLIPUTTY_VERSION_H}
    COMMENT "Updating joliputty-version.h")
  add_dependencies(putty joliputty_version_h)

  # (its MSIX package, for the Microsoft Store, made on request with
  # "cmake --build . --config Release --target msix": see
  # msix/make_msix.ps1)
  if(winui_arch MATCHES "^(x64|arm64)$")
    add_custom_target(msix
      COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_CURRENT_SOURCE_DIR}/msix/make_msix.ps1
        -ExeDir $<TARGET_FILE_DIR:putty>
        -WinUIInclude ${WINAPPSDK_Runtime_DIR}/include
        -Arch ${winui_arch}
        -VersionHeader ${GENERATED_JOLIPUTTY_VERSION_H}
        -StagingDir ${CMAKE_BINARY_DIR}/msix/staging
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      DEPENDS putty
      VERBATIM)

    # (and its MSI installer, with WiX 3.14 or later, found on the PATH
    # or where its installer says, in WIX: "cmake --build . --config
    # Release --target msi", making JoliPuTTY_<version>_<arch>.msi, on a
    # joliputty-X.Y tag only; see msi/make_msi.cmake and msi/joliputty.wxs)
    find_program(WIX_CANDLE candle HINTS "$ENV{WIX}/bin")
    find_program(WIX_LIGHT light HINTS "$ENV{WIX}/bin")
    if(WIX_CANDLE AND WIX_LIGHT)
      add_custom_target(msi
        COMMAND ${CMAKE_COMMAND}
          -DVERSION_H=${GENERATED_JOLIPUTTY_VERSION_H}
          -DWIX_CANDLE=${WIX_CANDLE}
          -DWIX_LIGHT=${WIX_LIGHT}
          -DARCH=${winui_arch}
          -DBUILDDIR=$<TARGET_FILE_DIR:putty>
          -DMSI_DIR=${CMAKE_BINARY_DIR}/msi
          -DOUT_DIR=${CMAKE_BINARY_DIR}
          -DWXS=${CMAKE_CURRENT_SOURCE_DIR}/msi/joliputty.wxs
          -DWXS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/msi
          -P ${CMAKE_CURRENT_SOURCE_DIR}/msi/make_msi.cmake
        DEPENDS putty
        VERBATIM)
    else()
      message(STATUS
        "WiX not found: no 'msi' target to make JoliPuTTY's installer")
    endif()
  endif()
endif()

add_executable(puttytel
  window.c
  putty.c
  help.c
  ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
  ${CMAKE_SOURCE_DIR}/stubs/no-ca-config.c
  ${CMAKE_SOURCE_DIR}/stubs/no-console.c
  ${CMAKE_SOURCE_DIR}/stubs/no-rand.c
  ${CMAKE_SOURCE_DIR}/stubs/no-dit.c
  ${CMAKE_SOURCE_DIR}/proxy/nocproxy.c
  ${CMAKE_SOURCE_DIR}/proxy/nosshproxy.c
  puttytel.rc)
be_list(puttytel PuTTYtel SERIAL OTHERBACKENDS)
add_dependencies(puttytel generated_licence_h)
target_link_libraries(puttytel
  guiterminal guimisc eventloop otherbackends settings network utils
  ${platform_libraries})
set_target_properties(puttytel PROPERTIES WIN32_EXECUTABLE ON)
installed_program(puttytel)

add_executable(puttygen
  puttygen.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  noise.c
  no-jump-list.c
  storage.c
  help.c
  ${CMAKE_SOURCE_DIR}/sshpubk.c
  ${CMAKE_SOURCE_DIR}/sshrand.c
  controls.c
  puttygen.rc)
add_dependencies(puttygen generated_licence_h)
target_link_libraries(puttygen
  keygen guimisc crypto utils
  ${platform_libraries})
set_target_properties(puttygen PROPERTIES WIN32_EXECUTABLE ON)
installed_program(puttygen)

if(HAVE_CONPTY)
  add_executable(pterm
    window.c
    pterm.c
    help.c
    conpty.c
    ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
    ${CMAKE_SOURCE_DIR}/stubs/no-ca-config.c
    ${CMAKE_SOURCE_DIR}/stubs/no-console.c
    ${CMAKE_SOURCE_DIR}/stubs/no-rand.c
    ${CMAKE_SOURCE_DIR}/stubs/no-dit.c
    ${CMAKE_SOURCE_DIR}/proxy/nosshproxy.c
    pterm.rc)
  be_list(pterm pterm)
  add_dependencies(pterm generated_licence_h)
  target_link_libraries(pterm
    guiterminal guimisc eventloop settings network utils
    ${platform_libraries})
  set_target_properties(pterm PROPERTIES WIN32_EXECUTABLE ON)
  installed_program(pterm)
else()
  message("ConPTY not available; cannot build Windows pterm")
endif()

add_executable(test_split_into_argv
  test/test_split_into_argv.c)
target_compile_definitions(test_split_into_argv PRIVATE TEST)
target_link_libraries(test_split_into_argv utils ${platform_libraries})

add_executable(test_screenshot
  test/test_screenshot.c)
target_link_libraries(test_screenshot utils ${platform_libraries})

add_executable(test_lineedit
  ${CMAKE_SOURCE_DIR}/test/test_lineedit.c
  ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
  ${CMAKE_SOURCE_DIR}/stubs/no-logging.c
  ${CMAKE_SOURCE_DIR}/stubs/no-printing.c
  ${CMAKE_SOURCE_DIR}/stubs/no-storage.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  no-jump-list.c)
target_link_libraries(test_lineedit
  guiterminal settings eventloop utils ${platform_libraries})

add_executable(test_terminal
  ${CMAKE_SOURCE_DIR}/test/test_terminal.c
  ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
  ${CMAKE_SOURCE_DIR}/stubs/no-storage.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  no-jump-list.c)
target_link_libraries(test_terminal
  guiterminal settings eventloop utils ${platform_libraries})

add_sources_from_current_dir(test_conf no-jump-list.c handle-wait.c)
