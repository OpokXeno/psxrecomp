if(NOT DEFINED SOURCE OR NOT DEFINED DESTINATION)
    message(FATAL_ERROR "stage_builtin_mods.cmake requires SOURCE and DESTINATION")
endif()

set(_marker "${DESTINATION}/.builtin-packages")
set(_previous_ids)
if(EXISTS "${_marker}")
    file(STRINGS "${_marker}" _previous_ids)
endif()

file(GLOB _source_entries
    LIST_DIRECTORIES true
    RELATIVE "${SOURCE}/packages"
    "${SOURCE}/packages/*")
set(_current_ids)
foreach(_entry IN LISTS _source_entries)
    if(IS_DIRECTORY "${SOURCE}/packages/${_entry}" AND
       _entry MATCHES "^[A-Za-z0-9._-]+$")
        list(APPEND _current_ids "${_entry}")
    endif()
endforeach()

set(_owned_ids ${_previous_ids} ${_current_ids})
list(REMOVE_DUPLICATES _owned_ids)
foreach(_id IN LISTS _owned_ids)
    if(_id MATCHES "^[A-Za-z0-9._-]+$")
        file(REMOVE_RECURSE "${DESTINATION}/packages/${_id}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${DESTINATION}")
file(WRITE "${_marker}" "")
foreach(_id IN LISTS _current_ids)
    file(APPEND "${_marker}" "${_id}\n")
endforeach()
