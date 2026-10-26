# Support for building the WinUI 3 front end (windows/winui).
#
# This downloads the NuGet packages for the Windows App SDK and
# C++/WinRT at configure time, and runs cppwinrt to generate the C++
# projection headers for the Windows SDK and the Windows App SDK. The
# result is an INTERFACE library 'winui_sdk' to link against, and the
# function winui_copy_runtime(target), which arranges for the Windows
# App SDK bootstrapper DLL to be copied next to the built executable.
#
# If the toolchain can't build it, we warn and define nothing, so that
# the rest of PuTTY can still be built.

if(NOT MSVC)
  message(WARNING
    "Not building jputty.exe: its WinUI 3 front end needs a Microsoft "
    "Visual C++ toolchain (cl or clang-cl), because the Windows App SDK "
    "does not support MinGW")
  return()
endif()

if(CMAKE_VERSION VERSION_LESS 3.18)
  message(WARNING
    "Not building jputty.exe: its WinUI 3 front end needs CMake 3.18 or later")
  return()
endif()

# The C++/WinRT half of the front end. (This must be done here, at
# the directory scope of the putty target, rather than in a function.)
enable_language(CXX)

# (with the assertions, as for the C in cmake/setup.cmake: the C++
# includes defs.h too, which refuses NDEBUG)
string(REPLACE "/DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "/DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

set(PUTTY_WINAPPSDK_VERSION "2.5.1" CACHE STRING
  "Version of the Microsoft.WindowsAppSDK NuGet package to build against")
set(PUTTY_CPPWINRT_VERSION "2.0.240405.15" CACHE STRING
  "Version of the Microsoft.Windows.CppWinRT NuGet package")
set(PUTTY_CPPWINRT_SDK "sdk" CACHE STRING
  "Windows SDK metadata for cppwinrt to project: 'sdk', 'local', or a version")

set(WINUI_PACKAGE_DIR ${CMAKE_BINARY_DIR}/nuget)

function(winui_fetch_package id version outvar)
  set(dir ${WINUI_PACKAGE_DIR}/${id}.${version})
  if(NOT EXISTS ${dir}/.extracted)
    set(nupkg ${WINUI_PACKAGE_DIR}/${id}.${version}.nupkg)
    message(STATUS "Downloading NuGet package ${id} ${version}")
    file(DOWNLOAD
      "https://www.nuget.org/api/v2/package/${id}/${version}"
      ${nupkg} STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      list(GET status 1 msg)
      message(FATAL_ERROR "Unable to download ${id} ${version}: ${msg}")
    endif()
    file(REMOVE_RECURSE ${dir})
    file(ARCHIVE_EXTRACT INPUT ${nupkg} DESTINATION ${dir})
    file(WRITE ${dir}/.extracted "")
  endif()
  set(${outvar} ${dir} PARENT_SCOPE)
endfunction()

# Find the version of package 'depid' that the package in 'pkgdir'
# depends on, according to its .nuspec. (Versions are written either
# as a plain minimum, "1.2.3", or as an exact range, "[1.2.3]".)
function(winui_dependency_version pkgdir depid outvar)
  file(GLOB nuspec ${pkgdir}/*.nuspec)
  file(READ ${nuspec} text)
  string(REPLACE "." "\\." depid_re ${depid})
  if(NOT text MATCHES "<dependency id=\"${depid_re}\" version=\"\\[?([^]\",]+)")
    message(FATAL_ERROR "${nuspec} has no dependency on ${depid}")
  endif()
  set(${outvar} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()

# Since version 1.8, Microsoft.WindowsAppSDK is a metapackage, with
# the actual files split across component packages. We only need a
# few of the components, at the versions the metapackage names.
winui_fetch_package(Microsoft.WindowsAppSDK ${PUTTY_WINAPPSDK_VERSION}
  WINAPPSDK_META_DIR)
foreach(component Foundation InteractiveExperiences WinUI Runtime)
  winui_dependency_version(${WINAPPSDK_META_DIR}
    Microsoft.WindowsAppSDK.${component} version)
  winui_fetch_package(Microsoft.WindowsAppSDK.${component} ${version}
    WINAPPSDK_${component}_DIR)
endforeach()
winui_dependency_version(${WINAPPSDK_WinUI_DIR} Microsoft.Web.WebView2
  webview2_version)
winui_fetch_package(Microsoft.Web.WebView2 ${webview2_version} WEBVIEW2_DIR)
winui_fetch_package(Microsoft.Windows.CppWinRT ${PUTTY_CPPWINRT_VERSION}
  CPPWINRT_DIR)

# Work out which architecture's binaries we want.
if(CMAKE_GENERATOR_PLATFORM)
  string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" winui_arch)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
  set(winui_arch arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(winui_arch x64)
else()
  set(winui_arch x86)
endif()
if(winui_arch STREQUAL "win32")
  set(winui_arch x86)
endif()

# Collect the metadata. InteractiveExperiences has a copy for each of
# several Windows SDK versions; we want the newest.
file(GLOB ie_metadata_dirs LIST_DIRECTORIES true
  ${WINAPPSDK_InteractiveExperiences_DIR}/metadata/10.*)
list(SORT ie_metadata_dirs COMPARE NATURAL)
list(GET ie_metadata_dirs -1 ie_metadata_dir)
file(GLOB winui_winmds
  ${WINAPPSDK_Foundation_DIR}/metadata/*.winmd
  ${ie_metadata_dir}/*.winmd
  ${WINAPPSDK_WinUI_DIR}/metadata/*.winmd
  ${WEBVIEW2_DIR}/lib/*.winmd)
if(NOT winui_winmds)
  message(FATAL_ERROR "No .winmd files found in the Windows App SDK packages")
endif()

# Generate the C++/WinRT projection, if we haven't already for this
# set of inputs.
set(CPPWINRT_OUT ${CMAKE_BINARY_DIR}/cppwinrt)
set(cppwinrt_args -input ${PUTTY_CPPWINRT_SDK})
foreach(winmd ${winui_winmds})
  list(APPEND cppwinrt_args -input ${winmd})
endforeach()
string(SHA256 cppwinrt_stamp "${PUTTY_CPPWINRT_VERSION};${cppwinrt_args}")
if(NOT EXISTS ${CPPWINRT_OUT}/.stamp-${cppwinrt_stamp})
  message(STATUS "Generating C++/WinRT projection headers")
  # cppwinrt writes its hundreds of output files in parallel, which
  # network shares (e.g. a source tree on a VM host) don't always
  # cope with. So generate them on local disk, and copy them over.
  if(DEFINED ENV{TEMP} AND IS_DIRECTORY "$ENV{TEMP}")
    file(TO_CMAKE_PATH "$ENV{TEMP}" tmpdir)
    string(SUBSTRING ${cppwinrt_stamp} 0 16 tmpname)
    set(cppwinrt_tmp ${tmpdir}/putty-cppwinrt-${tmpname})
  else()
    set(cppwinrt_tmp ${CPPWINRT_OUT}.tmp)
  endif()
  file(REMOVE_RECURSE ${cppwinrt_tmp})
  execute_process(
    COMMAND ${CPPWINRT_DIR}/bin/cppwinrt.exe ${cppwinrt_args}
      -output ${cppwinrt_tmp}
    RESULT_VARIABLE cppwinrt_result)
  if(NOT cppwinrt_result EQUAL 0)
    file(REMOVE_RECURSE ${cppwinrt_tmp})
    message(FATAL_ERROR "cppwinrt failed (${cppwinrt_result})")
  endif()
  file(REMOVE_RECURSE ${CPPWINRT_OUT})
  file(COPY ${cppwinrt_tmp}/ DESTINATION ${CPPWINRT_OUT})
  file(REMOVE_RECURSE ${cppwinrt_tmp})
  file(WRITE ${CPPWINRT_OUT}/.stamp-${cppwinrt_stamp} "")
endif()

set(WINUI_BOOTSTRAP_DLL
  ${WINAPPSDK_Foundation_DIR}/runtimes/win-${winui_arch}/native/Microsoft.WindowsAppRuntime.Bootstrap.dll)

add_library(winui_sdk INTERFACE)
# The generated projection must come before the Windows SDK's own
# (older) copy of the C++/WinRT headers.
target_include_directories(winui_sdk BEFORE INTERFACE
  ${CPPWINRT_OUT}
  ${WINAPPSDK_Foundation_DIR}/include      # MddBootstrap.h
  ${WINAPPSDK_WinUI_DIR}/include           # XAML interop headers
  ${WINAPPSDK_Runtime_DIR}/include)        # WindowsAppSDK-VersionInfo.h
target_link_libraries(winui_sdk INTERFACE
  ${WINAPPSDK_Foundation_DIR}/lib/native/${winui_arch}/Microsoft.WindowsAppRuntime.Bootstrap.lib
  windowsapp.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib windowscodecs.lib)
target_compile_definitions(winui_sdk INTERFACE $<$<COMPILE_LANGUAGE:CXX>:NOMINMAX>)
target_compile_options(winui_sdk INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:/EHsc /permissive- /bigobj /utf-8>)

function(winui_copy_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${WINUI_BOOTSTRAP_DLL} $<TARGET_FILE_DIR:${target}>)
  # (Not added to shipped.txt, which lists the files we sign: this
  # one comes already signed by Microsoft.)
endfunction()
