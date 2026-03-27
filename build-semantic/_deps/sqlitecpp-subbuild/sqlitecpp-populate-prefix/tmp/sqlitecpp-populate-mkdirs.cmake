# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-src")
  file(MAKE_DIRECTORY "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-src")
endif()
file(MAKE_DIRECTORY
  "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-build"
  "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-subbuild/sqlitecpp-populate-prefix"
  "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-subbuild/sqlitecpp-populate-prefix/tmp"
  "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-subbuild/sqlitecpp-populate-prefix/src/sqlitecpp-populate-stamp"
  "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-subbuild/sqlitecpp-populate-prefix/src"
  "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-subbuild/sqlitecpp-populate-prefix/src/sqlitecpp-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-subbuild/sqlitecpp-populate-prefix/src/sqlitecpp-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/krle/workspace/mcp/code/build-semantic/_deps/sqlitecpp-subbuild/sqlitecpp-populate-prefix/src/sqlitecpp-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
