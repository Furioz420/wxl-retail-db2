# Developer-only WDC5 validator. It links the host-owned decoder directly but is not deployed.
add_executable(wxl-db2-validate
    "${CMAKE_CURRENT_LIST_DIR}/tools/Wdc5Validate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/wxl-host-extension/host/db2/Wdc5.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/wxl-host-extension/shared/db2/Wdc5Table.cpp")
target_include_directories(wxl-db2-validate PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
target_compile_definitions(wxl-db2-validate PRIVATE ${WXL_DEFS})

# Metadata-only validation for the complete generated wowdbd schema/relationship graph.
add_executable(wxl-db2-schema-check
    "${CMAKE_CURRENT_LIST_DIR}/tools/SchemaCatalogValidate.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/SchemaCatalog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/wxl-host-extension/host/db2/Wdc5.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/wxl-host-extension/shared/db2/Wdc5Table.cpp")
target_include_directories(wxl-db2-schema-check PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
target_compile_definitions(wxl-db2-schema-check PRIVATE ${WXL_DEFS})
