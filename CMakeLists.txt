cmake_minimum_required(VERSION 3.10)
include(GNUInstallDirs)

project(obs-catpion)

set(obs-catpion_SOURCES
			forms/catpion.ui
			src/catpion.c
			src/model.c
			src/line-gen.c
			src/tinyosc.c
			src/obs-text-pthread-thread.c
			src/pipewire-audio.c
			src/catpion-ui.cpp
)

add_library(obs-catpion MODULE ${obs-catpion_SOURCES})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(PkgConfig)

if (NOT TARGET OBS::libobs)
    find_package(libobs QUIET)
endif()
if (NOT TARGET libobs)
    find_package(LibObs QUIET)
endif()
if (TARGET OBS::libobs)
    set(PLUGIN_LIBS OBS::libobs)
elseif (TARGET libobs)
    set(PLUGIN_LIBS libobs)
else()
    pkg_check_modules(LIBOBS libobs IMPORTED_TARGET REQUIRED)
    set(PLUGIN_LIBS PkgConfig::LIBOBS)
    if (HAVE_WAYLAND)
        add_definitions(-DENABLE_WAYLAND=1)
    endif()
endif()

set(PLUGIN_LIBS ${PLUGIN_LIBS} obs-frontend-api)

pkg_search_module(PIPEWIRE libpipewire-0.3 REQUIRED)

pkg_search_module(Pango pango REQUIRED)
pkg_search_module(Cairo cairo REQUIRED)
pkg_search_module(PangoCairo pangocairo REQUIRED)
pkg_search_module(AprilASR april-asr REQUIRED)

set(obs-catpion_INCLUDES
	${PIPEWIRE_INCLUDE_DIRS}
	${SPA_INCLUDE_DIRS}
	${GLib_INCLUDE_DIRS}
	${Pango_INCLUDE_DIRS}
	${Cairo_INCLUDE_DIRS}
	${PangoCairo_INCLUDE_DIRS}
	${AprilASR_INCLUDE_DIRS}
)

add_definitions(
	${PIPEWIRE_DEFINITIONS}
)

set(obs-catpion_LIBRARIES
	${PIPEWIRE_LIBRARIES}
	${Pango_LIBRARIES}
	${Cairo_LIBRARIES}
	${PangoCairo_LIBRARIES}
	${AprilASR_LIBRARIES}
	Qt::Widgets
)

set(QT_NO_CREATE_VERSIONLESS_TARGETS TRUE)
find_package(
  Qt6
  COMPONENTS Core
  REQUIRED)
set(QT_NO_CREATE_VERSIONLESS_TARGETS FALSE)

set(qt_components Widgets)
list(APPEND qt_components Gui)
find_package(Qt6 REQUIRED ${qt_components})

list(APPEND qt_components Core)
list(APPEND qt_components "GuiPrivate")
foreach(component IN LISTS qt_components)
  if(NOT TARGET Qt::${component} AND TARGET Qt6::${component})
    add_library(Qt::${component} INTERFACE IMPORTED)
    set_target_properties(Qt::${component} PROPERTIES INTERFACE_LINK_LIBRARIES Qt6::${component})
  endif()
endforeach()

target_link_libraries(obs-catpion ${obs-catpion_LIBRARIES} ${PLUGIN_LIBS})

target_include_directories(obs-catpion PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}> ${obs-catpion_INCLUDES})

set_target_properties(
  obs-catpion
  PROPERTIES PREFIX ""
             AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON
             AUTOUIC_SEARCH_PATHS forms)

install(TARGETS obs-catpion LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins)
install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_PREFIX}/share/obs/obs-plugins/obs-catpion)
