# Existing dependency trees may already contain this fix. Permit incremental
# builds, but fail clearly if a future wxWidgets update changes the affected code.
set(patch "${CMAKE_CURRENT_LIST_DIR}/0002-macos-webview-shortcut-focus.patch")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET
)
if(already_applied EQUAL 0)
    return()
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --ignore-space-change --whitespace=fix "${patch}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE result
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not apply the macOS webview shortcut fix: ${error}")
endif()
