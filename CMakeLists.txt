# Copyright (C) 2022+ AzerothCore <www.azerothcore.org>
# Released under GNU AGPL v3
# License: https://www.gnu.org/licenses/agpl-3.0.html

if(COMMAND CollectSourceFiles)
    CollectSourceFiles(
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE_SOURCES
    )
else()
    file(GLOB_RECURSE PRIVATE_SOURCES
        CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    )
endif()

if (SCRIPTS)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

    if(COMMAND AC_ADD_SCRIPT)
        foreach(source_file ${PRIVATE_SOURCES})
            AC_ADD_SCRIPT(${source_file})
        endforeach()
    elseif(TARGET game)
        target_sources(game
            PRIVATE
                ${PRIVATE_SOURCES}
        )

        if (BUILD_SHARED_LIBS)
            target_include_directories(game
                PUBLIC
                    ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
        else()
            target_include_directories(game
                PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
        endif()
    endif()
endif()
