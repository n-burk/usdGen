#----------------------------------------------------------------
# Generated CMake target import file for configuration "MinSizeRel".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "usdGen::usdGenExecutionResources" for configuration "MinSizeRel"
set_property(TARGET usdGen::usdGenExecutionResources APPEND PROPERTY IMPORTED_CONFIGURATIONS MINSIZEREL)
set_target_properties(usdGen::usdGenExecutionResources PROPERTIES
  IMPORTED_IMPLIB_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGenExecutionResources.lib"
  IMPORTED_LOCATION_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGenExecutionResources.dll"
  )

list(APPEND _cmake_import_check_targets usdGen::usdGenExecutionResources )
list(APPEND _cmake_import_check_files_for_usdGen::usdGenExecutionResources "${_IMPORT_PREFIX}/lib/usdGenExecutionResources.lib" "${_IMPORT_PREFIX}/lib/usdGenExecutionResources.dll" )

# Import target "usdGen::usdGen" for configuration "MinSizeRel"
set_property(TARGET usdGen::usdGen APPEND PROPERTY IMPORTED_CONFIGURATIONS MINSIZEREL)
set_target_properties(usdGen::usdGen PROPERTIES
  IMPORTED_IMPLIB_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGen.lib"
  IMPORTED_LINK_DEPENDENT_LIBRARIES_MINSIZEREL "arch;js;plug;ts;ar;work;trace;hio;pxOsd"
  IMPORTED_LOCATION_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGen.dll"
  )

list(APPEND _cmake_import_check_targets usdGen::usdGen )
list(APPEND _cmake_import_check_files_for_usdGen::usdGen "${_IMPORT_PREFIX}/lib/usdGen.lib" "${_IMPORT_PREFIX}/lib/usdGen.dll" )

# Import target "usdGen::usdGenImaging" for configuration "MinSizeRel"
set_property(TARGET usdGen::usdGenImaging APPEND PROPERTY IMPORTED_CONFIGURATIONS MINSIZEREL)
set_target_properties(usdGen::usdGenImaging PROPERTIES
  IMPORTED_IMPLIB_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGenImaging.lib"
  IMPORTED_LINK_DEPENDENT_LIBRARIES_MINSIZEREL "hio;usdGen::usdGenSchema"
  IMPORTED_LOCATION_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGenImaging.dll"
  )

list(APPEND _cmake_import_check_targets usdGen::usdGenImaging )
list(APPEND _cmake_import_check_files_for_usdGen::usdGenImaging "${_IMPORT_PREFIX}/lib/usdGenImaging.lib" "${_IMPORT_PREFIX}/lib/usdGenImaging.dll" )

# Import target "usdGen::usdGenSchema" for configuration "MinSizeRel"
set_property(TARGET usdGen::usdGenSchema APPEND PROPERTY IMPORTED_CONFIGURATIONS MINSIZEREL)
set_target_properties(usdGen::usdGenSchema PROPERTIES
  IMPORTED_IMPLIB_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGenSchema.lib"
  IMPORTED_LINK_DEPENDENT_LIBRARIES_MINSIZEREL "tf;usdGeom"
  IMPORTED_LOCATION_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGenSchema.dll"
  )

list(APPEND _cmake_import_check_targets usdGen::usdGenSchema )
list(APPEND _cmake_import_check_files_for_usdGen::usdGenSchema "${_IMPORT_PREFIX}/lib/usdGenSchema.lib" "${_IMPORT_PREFIX}/lib/usdGenSchema.dll" )

# Import target "usdGen::usdGenTonic" for configuration "MinSizeRel"
set_property(TARGET usdGen::usdGenTonic APPEND PROPERTY IMPORTED_CONFIGURATIONS MINSIZEREL)
set_target_properties(usdGen::usdGenTonic PROPERTIES
  IMPORTED_IMPLIB_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGenTonic.lib"
  IMPORTED_LINK_DEPENDENT_LIBRARIES_MINSIZEREL "usd;usdGeom;hio;usdGen::usdGen"
  IMPORTED_LOCATION_MINSIZEREL "${_IMPORT_PREFIX}/lib/usdGenTonic.dll"
  )

list(APPEND _cmake_import_check_targets usdGen::usdGenTonic )
list(APPEND _cmake_import_check_files_for_usdGen::usdGenTonic "${_IMPORT_PREFIX}/lib/usdGenTonic.lib" "${_IMPORT_PREFIX}/lib/usdGenTonic.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
