# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-src"
  "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-build"
  "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-subbuild/googletest-populate-prefix"
  "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-subbuild/googletest-populate-prefix/tmp"
  "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-subbuild/googletest-populate-prefix/src/googletest-populate-stamp"
  "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-subbuild/googletest-populate-prefix/src"
  "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-subbuild/googletest-populate-prefix/src/googletest-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-subbuild/googletest-populate-prefix/src/googletest-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/root/quantra_refractor/quantraserver/build_local/_deps/googletest-subbuild/googletest-populate-prefix/src/googletest-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
