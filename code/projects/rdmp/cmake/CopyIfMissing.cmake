# cmake -DSRC=<file> -DDST=<file> -P CopyIfMissing.cmake : copies SRC to DST unless DST already exists.
if(NOT EXISTS "${DST}")
    configure_file("${SRC}" "${DST}" COPYONLY)
    message(STATUS "Created ${DST}")
endif()
