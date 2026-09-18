# usdGenConfig.cmake.in — CMake package config for downstream consumers

####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was usdGenConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

# Resolve package-relative paths before any nested dependency find
# (sol S-5/X-4), so a failing dependency cannot shadow them.
set_and_check(usdGen_PLUGIN_DIR "${PACKAGE_PREFIX_DIR}/lib/usd")
set_and_check(usdGen_LIBRARY_DIR "${PACKAGE_PREFIX_DIR}/lib")
if(OFF)
    set_and_check(usdGen_VULKAN_WIDTH_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/width.spv")
    set_and_check(usdGen_VULKAN_LENGTH_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/lengthScale.spv")
    set_and_check(usdGen_VULKAN_LENGTH_COMPACTION_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/lengthCompaction.spv")
    set_and_check(usdGen_VULKAN_LENGTH_SET_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/lengthSet.spv")
    set_and_check(usdGen_VULKAN_LENGTH_CUT_EXTEND_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/lengthCutExtend.spv")
    set_and_check(usdGen_VULKAN_LENGTH_REPARAM_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/lengthReparam.spv")
    set_and_check(usdGen_VULKAN_LENGTH_MINIMUM_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/lengthMinimum.spv")
    set_and_check(usdGen_VULKAN_LENGTH_LITERAL_V1_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/lengthLiteralV1.spv")
    set_and_check(usdGen_VULKAN_LENGTH_ENVELOPE_V1_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/lengthEnvelopeV1.spv")
    set_and_check(usdGen_VULKAN_WIDTH_BLEND_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/widthBlend.spv")
    set_and_check(usdGen_VULKAN_NON_WIDTH_COMPARE_SHADER "${PACKAGE_PREFIX_DIR}/share/usdGen/vulkan/nonWidthCompare.spv")
endif()

# Plugin locations, so a consuming build can compose PXR_PLUGINPATH_NAME
# without hardcoding the layout.
set(usdGen_PLUGINPATHS
    "${usdGen_PLUGIN_DIR}/usdGenSchema/resources"
    "${usdGen_PLUGIN_DIR}/usdGenImaging/resources")

include(CMakeFindDependencyMacro)

if(OFF)
    # Vulkan is required only by consumers linking usdGenVulkanNative or
    # usdGenVulkanFactory (whose interfaces name Vulkan::Vulkan). Probe
    # quietly so core-only consumers configure without a Vulkan SDK; a
    # consumer that links the Vulkan targets without one fails at generate
    # time on the missing imported target.
    find_package(Vulkan 1.2 QUIET)
    if(NOT Vulkan_FOUND)
        message(STATUS "usdGen: no Vulkan SDK; usdGenVulkanNative/usdGenVulkanFactory unavailable")
    endif()
    find_dependency(Threads)
endif()

# OpenUSD's garch interface references OpenGL::GL. Create it before loading
# pxr's imported targets, on Windows as well as Unix-like hosts.
if(NOT TARGET OpenGL::GL)
    find_dependency(OpenGL)
endif()

# The public link interface of usdGen references pxr imported targets, TBB and
# OpenGL. The `if(NOT TARGET usd)` guard makes usdGen coexist with a build that
# already imported pxr (e.g. OpenUSD itself, or rigExec pulling pxr first):
# pxrConfig is single-pass and a second find_package(pxr CONFIG) detonates
# (plan §3.2; sol S-5/X-4).
if(NOT TARGET usd)
    # The stock Windows build_usd.py prefix has OpenSubdiv headers and
    # libraries but not OpenSubdivConfig.cmake. Reconstruct the two targets
    # pxr's export expects from the prefix supplied to CMAKE_PREFIX_PATH.
    if(WIN32 AND NOT DEFINED PXR_FIND_OPENSUBDIV_IN_CONFIG)
        set(PXR_FIND_OPENSUBDIV_IN_CONFIG OFF)
    endif()
    if(WIN32 AND NOT TARGET OpenSubdiv::osdCPU_static)
        set(_usdgen_pxr_prefix "")
        foreach(_usdgen_pxr_hint IN LISTS CMAKE_PREFIX_PATH)
            if(EXISTS "${_usdgen_pxr_hint}/pxrConfig.cmake")
                set(_usdgen_pxr_prefix "${_usdgen_pxr_hint}")
                break()
            endif()
        endforeach()
        if(NOT _usdgen_pxr_prefix AND DEFINED pxr_DIR AND
           EXISTS "${pxr_DIR}/pxrConfig.cmake")
            set(_usdgen_pxr_prefix "${pxr_DIR}")
        endif()
        if(_usdgen_pxr_prefix AND
           EXISTS "${_usdgen_pxr_prefix}/lib/osdCPU.lib" AND
           EXISTS "${_usdgen_pxr_prefix}/lib/osdGPU.lib")
            foreach(_usdgen_osd_target osdCPU osdGPU)
                add_library(OpenSubdiv::${_usdgen_osd_target}_static STATIC IMPORTED)
                set_target_properties(OpenSubdiv::${_usdgen_osd_target}_static PROPERTIES
                    IMPORTED_LOCATION "${_usdgen_pxr_prefix}/lib/${_usdgen_osd_target}.lib"
                    INTERFACE_INCLUDE_DIRECTORIES "${_usdgen_pxr_prefix}/include"
                    INTERFACE_COMPILE_DEFINITIONS
                        "OPENSUBDIV_HAS_PATCH_SHADER_SOURCE_GLSL;OPENSUBDIV_HAS_GLSL_TRANSFORM_FEEDBACK;OPENSUBDIV_HAS_GLSL_COMPUTE;OPENSUBDIV_HAS_OPENGL")
            endforeach()
        endif()
        unset(_usdgen_osd_target)
        unset(_usdgen_pxr_hint)
        unset(_usdgen_pxr_prefix)
    endif()
    find_dependency(pxr CONFIG)
endif()
# TBB::tbb is provided by pxrConfig when PXR_FIND_TBB_IN_CONFIG is OFF
# (the default, and the configuration usdGen itself is built with): it becomes
# an imported target pointing at ${usd}/lib/libtbb.so. Only fall back to a
# system/config TBB search when that target does not already exist.
if(NOT TARGET TBB::tbb)
    find_dependency(TBB)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/usdGenTargets.cmake")
