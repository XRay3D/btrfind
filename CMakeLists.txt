cmake_minimum_required(VERSION 3.20)
project(btrfind VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BTRFIND_BUILD_DAEMON "Демон btrfindd" ON)
option(BTRFIND_BUILD_GUI "GUI btrfind (Qt6 Widgets)" ON)
option(BTRFIND_BUILD_POC "Диагностическая утилита btrfs-poc" ON)
option(BTRFIND_BUILD_TESTS "Юнит-тесты" ON)

include(GNUInstallDirs)
find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network)
if(BTRFIND_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)
    # Эскизы как в Dolphin (видео, PDF, …) — если есть KDE Frameworks KIO; иначе только изображения.
    find_package(KF6KIO 6 QUIET)
    if(KF6KIO_FOUND)
        message(STATUS "Эскизы: KIO::PreviewJob (KF6 ${KF6KIO_VERSION})")
    else()
        message(STATUS "Эскизы: только изображения (KF6KIO не найден — пакет kf6-kio-devel)")
    endif()
endif()
if(BTRFIND_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()
set(CMAKE_AUTOMOC ON)

set(BTRFIND_WARNINGS -Wall -Wextra)

# ---- ядро: индекс, сканеры, сверка, запросы, поиск, протокол
add_library(btrfind_core STATIC
    src/core/fold.cpp
    src/core/index.cpp
    src/core/fsutil.cpp
    src/core/btrfs_tree.cpp
    src/core/walk_scan.cpp
    src/core/mounts.cpp
    src/core/reconcile.cpp
    src/core/storage.cpp
    src/core/matcher.cpp
    src/core/query.cpp
    src/core/search.cpp
    src/core/protocol.cpp
    src/core/rename.cpp
)
target_include_directories(btrfind_core PUBLIC src/core)
target_compile_options(btrfind_core PRIVATE ${BTRFIND_WARNINGS})
target_link_libraries(btrfind_core PUBLIC Qt6::Core Threads::Threads OpenMP::OpenMP_CXX PkgConfig::PCRE2)

# ---- диагностика
if(BTRFIND_BUILD_POC)
    add_executable(btrfs-poc src/poc/main.cpp src/poc/fan_watch.cpp)
    target_include_directories(btrfs-poc PRIVATE src/poc)
    target_compile_options(btrfs-poc PRIVATE ${BTRFIND_WARNINGS})
    target_link_libraries(btrfs-poc PRIVATE btrfind_core)
    install(TARGETS btrfs-poc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---- демон
if(BTRFIND_BUILD_DAEMON)
    add_executable(btrfindd
        src/daemon/main.cpp
        src/daemon/volume.cpp
        src/daemon/daemon.cpp
        src/daemon/watcher.cpp
        src/daemon/server.cpp
    )
    target_include_directories(btrfindd PRIVATE src/daemon)
    target_compile_options(btrfindd PRIVATE ${BTRFIND_WARNINGS})
    target_link_libraries(btrfindd PRIVATE btrfind_core Qt6::Network)
    target_compile_definitions(btrfindd PRIVATE BTRFIND_VERSION="${PROJECT_VERSION}")
    install(TARGETS btrfindd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
    configure_file(packaging/btrfindd.service.in ${CMAKE_CURRENT_BINARY_DIR}/btrfindd.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/btrfindd.service DESTINATION lib/systemd/system)
endif()

# ---- GUI (+ консольный режим --cli)
if(BTRFIND_BUILD_GUI)
    add_executable(btrfind
        src/gui/main.cpp
        src/gui/client.cpp
        src/gui/cli.cpp
        src/gui/resultmodel.cpp
        src/gui/mainwindow.cpp
        src/gui/icons.cpp
        src/gui/renamedialog.cpp
        src/gui/history.cpp
        src/gui/thumbnails.cpp
        src/gui/thumbdelegate.cpp
    )
    target_include_directories(btrfind PRIVATE src/gui)
    target_compile_options(btrfind PRIVATE ${BTRFIND_WARNINGS})
    target_link_libraries(btrfind PRIVATE btrfind_core Qt6::Network Qt6::Widgets Qt6::DBus)
    target_compile_definitions(btrfind PRIVATE BTRFIND_VERSION="${PROJECT_VERSION}")
    if(KF6KIO_FOUND)
        target_link_libraries(btrfind PRIVATE KF6::KIOGui KF6::KIOCore)
        target_compile_definitions(btrfind PRIVATE BTRFIND_HAVE_KIO)
    endif()
    install(TARGETS btrfind RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/btrfind.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    # Пункт «Искать здесь (btrfind)» в контекстном меню папок Dolphin/KDE (как «Search Everything…»)
    install(FILES packaging/btrfind-servicemenu.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/kio/servicemenus)
endif()

install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---- тесты
if(BTRFIND_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE btrfind_core Qt6::Test)
    add_test(NAME test_core COMMAND test_core)
    add_executable(test_rename tests/test_rename.cpp)
    target_link_libraries(test_rename PRIVATE btrfind_core Qt6::Test)
    add_test(NAME test_rename COMMAND test_rename)
endif()
