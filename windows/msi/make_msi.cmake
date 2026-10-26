# Pure cmake script to make JoliPuTTY's MSI installer,
# JoliPuTTY_<version>_<arch>.msi in OUT_DIR, with WiX's candle and light
# (WIX_CANDLE, WIX_LIGHT): run by the msi target in windows/CMakeLists.txt.
#
# The version is the one of the build, from VERSION_H (written by
# cmake/joliputty-version.cmake), and only a release is packaged: the
# current commit must be the joliputty-X.Y tag, with nothing uncommitted.

file(STRINGS "${VERSION_H}" lines REGEX "^#define JOLIPUTTY_")
foreach(line ${lines})
  if(line MATCHES "^#define (JOLIPUTTY_[A-Z_]+) (.*)$")
    # (the name kept first: the REGEX REPLACE sets CMAKE_MATCH_1 again)
    set(name "${CMAKE_MATCH_1}")
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" value "${CMAKE_MATCH_2}")
    set(${name} "${value}")
  endif()
endforeach()

if(NOT JOLIPUTTY_RELEASE)
  message(FATAL_ERROR "JoliPuTTY ${JOLIPUTTY_VERSION} is not a release: \
the MSI is only made on a joliputty-X.Y tag, with nothing uncommitted")
endif()

# (as four numbers, as Windows Installer wants it)
string(REPLACE "," "." winver "${JOLIPUTTY_BINARY_VERSION}")

# (the Start menu's link to the manual, for this version: see
# JOLIPUTTY_HELP in winui/winui-window.c)
file(MAKE_DIRECTORY "${MSI_DIR}")
file(WRITE "${MSI_DIR}/manual.url" "[InternetShortcut]\nURL=https://scresto09.github.io/joliputty/${JOLIPUTTY_VERSION}/\n")

set(msi_file "${OUT_DIR}/JoliPuTTY_${JOLIPUTTY_VERSION}_${ARCH}.msi")
execute_process(
  COMMAND "${WIX_CANDLE}" -nologo -arch ${ARCH}
    -dRealPlatform=${ARCH}
    -dBuilddir=${BUILDDIR}/
    -dWinver=${winver}
    "-dTextver=${JOLIPUTTY_TEXTVER}"
    -dManualFile=${MSI_DIR}/manual.url
    -out ${MSI_DIR}/joliputty.wixobj
    ${WXS}
  # (so that the .wxs's paths, relative to it, are found)
  WORKING_DIRECTORY "${WXS_DIR}"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "candle failed")
endif()
# (without the ICE validation, as PuTTY's own installer is made, in
# Buildscr: it complains of the per-user shortcuts with per-machine key
# paths, which are as PuTTY's are, and of FeaturesDlg's Next, in a line
# taken from it as it is)
execute_process(
  COMMAND "${WIX_LIGHT}" -nologo -ext WixUIExtension -ext WixUtilExtension
    -sval -spdb -out ${msi_file} ${MSI_DIR}/joliputty.wixobj
  WORKING_DIRECTORY "${WXS_DIR}"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "light failed")
endif()
message(STATUS "Made ${msi_file}")
