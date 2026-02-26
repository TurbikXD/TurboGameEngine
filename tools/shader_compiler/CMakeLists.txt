find_program(DXC_EXECUTABLE dxc)

if(DXC_EXECUTABLE)
    set(HLSL_INPUT "${CMAKE_SOURCE_DIR}/assets/shaders_hlsl/basic.hlsl")
    set(HLSL_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders_hlsl")
    file(MAKE_DIRECTORY "${HLSL_OUTPUT_DIR}")

    add_custom_target(shader_compile_hlsl
        COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -Fo "${HLSL_OUTPUT_DIR}/basic_vs.cso" "${HLSL_INPUT}"
        COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -Fo "${HLSL_OUTPUT_DIR}/basic_ps.cso" "${HLSL_INPUT}"
        COMMENT "Compiling HLSL shaders with dxc"
        VERBATIM
    )
else()
    add_custom_target(shader_compile_hlsl
        COMMAND ${CMAKE_COMMAND} -E echo "dxc not found on PATH. Skipping HLSL shader compile."
    )
endif()
