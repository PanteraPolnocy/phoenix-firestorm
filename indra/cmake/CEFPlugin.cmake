# -*- cmake -*-
include(Linking)
include(Prebuilt)

if (USESYSTEMLIBS)
    set(CEFPLUGIN ON CACHE BOOL
        "CEFPLUGIN support for the llplugin/llmedia test apps.")
else (USESYSTEMLIBS)
    use_prebuilt_binary(dullahan)
    set(CEFPLUGIN ON CACHE BOOL
        "CEFPLUGIN support for the llplugin/llmedia test apps.")
    set(CEF_INCLUDE_DIR ${LIBS_PREBUILT_DIR}/include/cef)
endif (USESYSTEMLIBS)

if (WINDOWS)
    set(CEF_PLUGIN_LIBRARIES
        libcef.lib
        libcef_dll_wrapper.lib
        dullahan.lib
    )
elseif (DARWIN)
    FIND_LIBRARY(APPKIT_LIBRARY AppKit)
    if (NOT APPKIT_LIBRARY)
        message(FATAL_ERROR "AppKit not found")
    endif()

    FIND_LIBRARY(CEF_LIBRARY "Chromium Embedded Framework" ${ARCH_PREBUILT_DIRS_RELEASE})
    if (NOT CEF_LIBRARY)
        message(FATAL_ERROR "CEF not found")
    endif()

    set(CEF_PLUGIN_LIBRARIES
        ${ARCH_PREBUILT_DIRS_RELEASE}/libcef_dll_wrapper.a
        ${ARCH_PREBUILT_DIRS_RELEASE}/libdullahan.a
        ${APPKIT_LIBRARY}
        ${CEF_LIBRARY}
       )

elseif (LINUX)

  if (USESYSTEMLIBS)
    find_library( LIB_DULLAHAN "dullahan" )
    find_library( LIB_CEF "cef" )
    find_library( LIB_CEF_WRAPPER "cef_dll_wrapper" )
    set(CEF_PLUGIN_LIBRARIES ${LIB_DULLAHAN}  ${LIB_CEF}  ${LIB_CEF_WRAPPER} )
  else()
    set(CEF_PLUGIN_LIBRARIES
      dullahan
      cef
      cef_dll_wrapper.a
      )
endif()

endif (WINDOWS)
