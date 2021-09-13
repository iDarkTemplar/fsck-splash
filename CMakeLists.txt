cmake_minimum_required( VERSION 3.18.0 )

project(fsck-splash
	VERSION 0.1.2
	LANGUAGES C)

find_package(PkgConfig REQUIRED)

pkg_check_modules(PlyBootClient REQUIRED IMPORTED_TARGET ply-boot-client)

set(SOURCES
	main.c
	)

add_executable(fsck-splash ${SOURCES})
target_link_libraries(fsck-splash PkgConfig::PlyBootClient)

include(GNUInstallDirs)

install(TARGETS fsck-splash RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
