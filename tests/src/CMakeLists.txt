# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(Base)
add_subdirectory(App)
if(BUILD_API)
    add_subdirectory(Api)
endif()
add_subdirectory(Gui)
add_subdirectory(Mod)
add_subdirectory(zipios++)
