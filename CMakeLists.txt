cmake_minimum_required(VERSION 3.10)
include(GNUInstallDirs)

# find_qt: Macro to find best possible Qt version for use with the project:
macro(find_qt)
  set(multiValueArgs COMPONENTS COMPONENTS_WIN COMPONENTS_MAC COMPONENTS_LINUX)
  cmake_parse_arguments(find_qt "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # Do not use versionless targets in the first step to avoid Qt::Core being clobbered by later opportunistic
  # find_package runs
  set(QT_NO_CREATE_VERSIONLESS_TARGETS TRUE)

  message(DEBUG "Attempting to find Qt 6")
  find_package(
    Qt6
    COMPONENTS Core
    REQUIRED)

  # Enable versionless targets for the remaining Qt components
  set(QT_NO_CREATE_VERSIONLESS_TARGETS FALSE)

  set(qt_components ${find_qt_COMPONENTS})
  if(OS_WINDOWS)
    list(APPEND qt_components ${find_qt_COMPONENTS_WIN})
  elseif(OS_MACOS)
    list(APPEND qt_components ${find_qt_COMPONENTS_MAC})
  else()
    list(APPEND qt_components ${find_qt_COMPONENTS_LINUX})
  endif()
  message(DEBUG "Trying to find Qt components ${qt_components}...")

  find_package(Qt6 REQUIRED ${qt_components})

  list(APPEND qt_components Core)

  if("Gui" IN_LIST find_qt_COMPONENTS_LINUX)
    list(APPEND qt_components "GuiPrivate")
  endif()

  # Check for versionless targets of each requested component and create if necessary
  foreach(component IN LISTS qt_components)
    message(DEBUG "Checking for target Qt::${component}")
    if(NOT TARGET Qt::${component} AND TARGET Qt6::${component})
      add_library(Qt::${component} INTERFACE IMPORTED)
      set_target_properties(Qt::${component} PROPERTIES INTERFACE_LINK_LIBRARIES Qt6::${component})
    endif()
  endforeach()
endmacro()

project(obs-catpion)
find_qt(COMPONENTS Widgets COMPONENTS_LINUX Gui)

set(obs-catpion_SOURCES
			forms/catpion.ui
			src/catpion.c
			src/model.c
			src/line-gen.c
			src/obs-text-pthread-thread.c
			src/pipewire-audio.c
			src/catpion-ui.cpp
)

add_library(obs-catpion MODULE ${obs-catpion_SOURCES})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(PkgConfig)

pkg_search_module(OBS libobs REQUIRED)
pkg_search_module(PIPEWIRE libpipewire-0.3 REQUIRED)

pkg_search_module(Pango pango REQUIRED)
pkg_search_module(Cairo cairo REQUIRED)
pkg_search_module(PangoCairo pangocairo REQUIRED)
pkg_search_module(AprilASR april-asr REQUIRED)

set(obs-catpion_INCLUDES
	${OBS_INCLUDE_DIRS}
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
	${OBS_LIBRARIES}
	${PIPEWIRE_LIBRARIES}
	${Pango_LIBRARIES}
	${Cairo_LIBRARIES}
	${PangoCairo_LIBRARIES}
	${AprilASR_LIBRARIES}
	Qt::Widgets
)

target_link_libraries(obs-catpion ${obs-catpion_LIBRARIES})

include_directories(SYSTEM
	${obs-catpion_INCLUDES}
)

set_target_properties(
  obs-catpion
  PROPERTIES PREFIX ""
             AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON
             AUTOUIC_SEARCH_PATHS forms)

install(TARGETS obs-catpion LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins)
install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_PREFIX}/share/obs/obs-plugins/obs-catpion)
