
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(actors-wilson-protos)
target_link_libraries(actors-wilson-protos PUBLIC
  contrib-libs-cxxsupp
  yutil
  contrib-libs-protobuf
)
target_proto_messages(actors-wilson-protos PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/wilson/protos/common.proto
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/wilson/protos/resource.proto
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/wilson/protos/trace.proto
)
target_proto_addincls(actors-wilson-protos
  ./
  ${CMAKE_SOURCE_DIR}/
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/contrib/libs/protobuf/src
)
target_proto_outs(actors-wilson-protos
  --cpp_out=${CMAKE_BINARY_DIR}/
  --cpp_styleguide_out=${CMAKE_BINARY_DIR}/
)
