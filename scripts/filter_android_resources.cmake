# Keep only the ADB build for the packaged host. Server 2012 uses an installed
# compatible ADB; the current Google tool is bundled only with Windows 10+.
if(NOT IS_ABSOLUTE "${RESOURCE_DIR}" OR NOT IS_DIRECTORY "${RESOURCE_DIR}/servers")
    message(FATAL_ERROR "RESOURCE_DIR must name a copied HarmonyCast resources directory")
endif()
foreach(host macos windows-x64)
    if(NOT host STREQUAL KEEP_HOST)
        file(REMOVE_RECURSE "${RESOURCE_DIR}/adb/${host}")
    endif()
endforeach()
