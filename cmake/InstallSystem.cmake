# Keep the configured user prefix intact; only this install uses a system prefix.
set(_prefix "/usr/local")
if(DEFINED ENV{PREFIX})
  set(_prefix "$ENV{PREFIX}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${JARO_BINARY_DIR}"
  --prefix "${_prefix}" --config "${JARO_INSTALL_CONFIG}"
  RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
  message(FATAL_ERROR "System installation failed: ${_status}")
endif()
