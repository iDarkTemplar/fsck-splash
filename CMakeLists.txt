cmake_minimum_required( VERSION 3.18.0 )

project(fsck-splash
	VERSION 0.0.1
	LANGUAGES C)

set(SOURCES
	main.c
	)

add_executable(fsck-splash ${SOURCES})
target_link_libraries(fsck-splash)

include(GNUInstallDirs)

install(TARGETS fsck-splash RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
