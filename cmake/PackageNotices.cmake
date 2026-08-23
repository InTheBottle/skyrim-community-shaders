# Package-only relocation table for documents that would otherwise be merged
# into generic Data paths. Source files stay beside the assets they license;
# staged archives install renamed copies under Community Shaders' own folder.

set(
    CS_PACKAGE_NOTICE_SOURCE_RELATIVE
    "package/Shaders/LICENSE"
    "package/Shaders/Common/Spherical Harmonics/LICENSE"
    "features/Upscaling/Shaders/Upscaling/FidelityFX/license.md"
    "features/Upscaling/Shaders/Upscaling/Streamline/license.txt"
    "features/Upscaling/Shaders/Upscaling/Streamline/nvngx_dlss.license.txt"
    "features/Upscaling/Shaders/Upscaling/Streamline/reflex.license.txt"
    "features/RenderDoc/Renderdoc/LICENSE.md"
    "features/RenderDoc/Renderdoc/README.md"
    "features/Terrain Shadows/textures/heightmaps/readme.txt"
)

set(
    CS_PACKAGE_NOTICE_STAGE_RELATIVE
    "Shaders/LICENSE"
    "Shaders/Common/Spherical Harmonics/LICENSE"
    "Shaders/Upscaling/FidelityFX/license.md"
    "Shaders/Upscaling/Streamline/license.txt"
    "Shaders/Upscaling/Streamline/nvngx_dlss.license.txt"
    "Shaders/Upscaling/Streamline/reflex.license.txt"
    "Renderdoc/LICENSE.md"
    "Renderdoc/README.md"
    "textures/heightmaps/readme.txt"
)

set(
    CS_PACKAGE_NOTICE_DESTINATION_RELATIVE
    "SKSE/Plugins/CommunityShaders/Notices/Shader-source-MIT-Ilya-Perapechka.txt"
    "SKSE/Plugins/CommunityShaders/Notices/Spherical-Harmonics-SebH-MIT.txt"
    "SKSE/Plugins/CommunityShaders/Notices/FidelityFX-SDK-License.txt"
    "SKSE/Plugins/CommunityShaders/Notices/NVIDIA-Streamline-License.txt"
    "SKSE/Plugins/CommunityShaders/Notices/NVIDIA-DLSS-License.txt"
    "SKSE/Plugins/CommunityShaders/Notices/NVIDIA-Reflex-License.txt"
    "SKSE/Plugins/CommunityShaders/Notices/RenderDoc-MIT.txt"
    "SKSE/Plugins/CommunityShaders/Documentation/RenderDoc-Runtime.txt"
    "SKSE/Plugins/CommunityShaders/Documentation/Terrain-Shadows-Heightmap-Naming.txt"
)

function(cs_get_package_notice_sources OUT_VAR SOURCE_ROOT)
    set(_sources "")
    foreach(_relative IN LISTS CS_PACKAGE_NOTICE_SOURCE_RELATIVE)
        list(APPEND _sources "${SOURCE_ROOT}/${_relative}")
    endforeach()
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()
function(cs_normalize_package_notices SOURCE_ROOT STAGE_ROOT INCLUDE_ALL)
    list(LENGTH CS_PACKAGE_NOTICE_SOURCE_RELATIVE _source_count)
    list(LENGTH CS_PACKAGE_NOTICE_STAGE_RELATIVE _stage_count)
    list(LENGTH CS_PACKAGE_NOTICE_DESTINATION_RELATIVE _destination_count)
    if(NOT _source_count EQUAL _stage_count OR NOT _source_count EQUAL _destination_count)
        message(FATAL_ERROR "Community Shaders package-notice relocation lists have different lengths")
    endif()

    math(EXPR _last_index "${_source_count} - 1")
    foreach(_index RANGE 0 ${_last_index})
        list(GET CS_PACKAGE_NOTICE_SOURCE_RELATIVE ${_index} _source_relative)
        list(GET CS_PACKAGE_NOTICE_STAGE_RELATIVE ${_index} _stage_relative)
        list(GET CS_PACKAGE_NOTICE_DESTINATION_RELATIVE ${_index} _destination_relative)
        set(_source "${SOURCE_ROOT}/${_source_relative}")
        set(_old_stage_path "${STAGE_ROOT}/${_stage_relative}")
        set(_destination "${STAGE_ROOT}/${_destination_relative}")

        if(NOT EXISTS "${_source}")
            message(FATAL_ERROR "Required package notice is missing: ${_source}")
        endif()

        set(_copy_notice ${INCLUDE_ALL})
        if(EXISTS "${_old_stage_path}")
            set(_copy_notice ON)
        endif()
        file(REMOVE "${_old_stage_path}")

        if(_copy_notice)
            get_filename_component(_destination_directory "${_destination}" DIRECTORY)
            file(MAKE_DIRECTORY "${_destination_directory}")
            file(COPY_FILE "${_source}" "${_destination}" ONLY_IF_DIFFERENT)
        endif()
    endforeach()
endfunction()
