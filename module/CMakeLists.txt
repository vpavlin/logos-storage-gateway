cmake_minimum_required(VERSION 3.14)
project(StorageGatewayModule LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT or ensure logos-module-builder is in your flake inputs.")
endif()

find_package(Threads REQUIRED)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)

# HTTP: cpp-httplib (one MIT header, vendored in src/vendor), plain http to a node on a VPN/mesh.
logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/storage_gateway_impl.h
        src/storage_gateway_impl.cpp
    INCLUDE_DIRS src
    LINK_LIBRARIES Threads::Threads
)
