# Installation and packages: Linux .deb (CPack), macOS app bundle, Windows folder (+ Inno Setup in CI).
# Ethernet capture uses Wireshark's dumpcap: the .deb depends on it and sets up capture permission for the installing
# user (postinst); the Windows installer offers to install Wireshark (with Npcap); on macOS the user installs
# Wireshark with ChmodBPF (README).
# ffmpeg drives the camera on Windows (DirectShow) and macOS (AVFoundation) and is shipped there; on Linux the
# camera is read directly (V4L2) and ffmpeg is a dependency of the .deb for other cameras and inputs.
include(GNUInstallDirs)

if(LIBRETAP_GUI)
  set_target_properties(libretap-gui PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.ohlincha.libretap
    MACOSX_BUNDLE_BUNDLE_NAME LibreTap
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_ICON_FILE LibreTap.icns
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in)   # asks for the camera
  if(APPLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.icns)   # made from icon.png in CI (iconutil)
    configure_file(resources/icon.icns ${CMAKE_CURRENT_BINARY_DIR}/LibreTap.icns COPYONLY)
    target_sources(libretap-gui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/LibreTap.icns)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/LibreTap.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  elseif(WIN32)
    set(ICON_FILE ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico)
    configure_file(packaging/windows/libretap.rc ${CMAKE_CURRENT_BINARY_DIR}/libretap.rc @ONLY)
    target_sources(libretap-gui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libretap.rc)
  endif()
endif()

if(WIN32)
  install(TARGETS libretap-cli RUNTIME DESTINATION .)
  if(LIBRETAP_GUI)
    install(TARGETS libretap-gui RUNTIME DESTINATION .)
  endif()
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE DESTINATION . RENAME LICENSE.txt OPTIONAL)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
  set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
  include(InstallRequiredSystemLibraries)
elseif(APPLE)
  if(LIBRETAP_GUI)
    install(TARGETS libretap-gui BUNDLE DESTINATION .)
    install(TARGETS libretap-cli RUNTIME DESTINATION LibreTap.app/Contents/MacOS)
  else()
    install(TARGETS libretap-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
else()
  install(TARGETS libretap-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(LIBRETAP_GUI)
    install(TARGETS libretap-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/io.github.ohlincha.libretap.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES resources/icon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
            RENAME io.github.ohlincha.libretap.png)
  endif()
  install(FILES ../docs/FORMAT.md ../docs/PROTOCOL-FORMAT.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/libretap)
  install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DATADIR}/libretap/examples)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/libretap RENAME copyright)
  endif()

  set(CPACK_GENERATOR DEB)
  set(CPACK_PACKAGE_NAME libretap)
  set(CPACK_PACKAGE_VENDOR "LibreTap contributors")
  set(CPACK_PACKAGE_CONTACT "LibreTap contributors <https://github.com/ohlincha/libretap/issues>")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Record what old vendor software says to its instrument, next to its screen")
  set(CPACK_PACKAGE_DESCRIPTION "Your instrument outlived its computer. LibreTap listens passively to the link between\n"
      " an old instrument and its old PC (serial tap) while a camera keeps pictures of the PC's screen, and shows both\n"
      " on one timeline. Controllable from scripts and AI agents (LibreTapCLI, JSON).")
  string(REPLACE ";" "" CPACK_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ohlincha/libretap")
  set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
  set(CPACK_STRIP_FILES ON)
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION science)
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # ffmpeg: cameras; wireshark-common: dumpcap for Ethernet capture (the postinst lets the installing user capture,
  # see packaging/linux/postinst and the README); libcap2-bin: setcap
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "ffmpeg, wireshark-common, libcap2-bin")
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland, v4l-utils")
  include(CPack)
endif()
