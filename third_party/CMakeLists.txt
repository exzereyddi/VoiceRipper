cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Speex (narrowband voice codec, used by legacy GoldSrc "voice_speex").
# ---------------------------------------------------------------------------
file(GLOB VR_SPEEX_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/speex/src/*.c")
add_library(vr_speex STATIC ${VR_SPEEX_SRC})
target_include_directories(vr_speex PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/speex/include"
)
target_include_directories(vr_speex PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/speex/src"
)
if (MSVC)
    target_compile_options(vr_speex PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# SKP_Silk (older reference Silk codec, used by early Steam voice chat).
# ---------------------------------------------------------------------------
file(GLOB VR_SKPSILK_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/silk/src/*.c")
add_library(vr_skpsilk STATIC ${VR_SKPSILK_SRC})
target_include_directories(vr_skpsilk PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/silk/include"
)
target_include_directories(vr_skpsilk PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/silk/src"
)
if (MSVC)
    target_compile_options(vr_skpsilk PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# Opus (CELT + the newer Silk variant it bundles internally), used by
# current Steam voice chat / SteamP2P voice frames.
# ---------------------------------------------------------------------------
file(GLOB VR_OPUS_CELT_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opus/celt/*.c")
file(GLOB VR_OPUS_SILK_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opus/silk/*.c")
file(GLOB VR_OPUS_SILK_FLOAT_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opus/silk/float/*.c")
file(GLOB VR_OPUS_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opus/src/*.c")

add_library(vr_opus STATIC
    ${VR_OPUS_CELT_SRC}
    ${VR_OPUS_SILK_SRC}
    ${VR_OPUS_SILK_FLOAT_SRC}
    ${VR_OPUS_SRC}
)
target_include_directories(vr_opus PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/opus/include"
)
target_include_directories(vr_opus PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/opus/celt"
    "${CMAKE_CURRENT_SOURCE_DIR}/opus/silk"
    "${CMAKE_CURRENT_SOURCE_DIR}/opus/silk/float"
    "${CMAKE_CURRENT_SOURCE_DIR}/opus/src"
)
target_compile_definitions(vr_opus PRIVATE
    OPUS_BUILD
    USE_ALLOCA
    _CRT_SECURE_NO_WARNINGS
)
if (MSVC)
    target_compile_options(vr_opus PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# Codec glue: thin wrappers (VoiceEncoder_Speex/Silk/Opus, SteamP2PCodec,
# VoiceCodec_Frame, CUtlBuffer, Buffer) wiring the raw codec libs into the
# IVoiceCodec interface VoiceExtractor consumes. Taken from
# UnrealKaraulov/UnrealDemoScanner's ReVoiceDecoder module.
# ---------------------------------------------------------------------------
add_library(vr_codec_glue STATIC
    codec_glue/Buffer.cpp
    codec_glue/SteamP2PCodec.cpp
    codec_glue/utlbuffer.cpp
    codec_glue/VoiceEncoder_Opus.cpp
    codec_glue/VoiceEncoder_Silk.cpp
    codec_glue/VoiceCodec_RawSilk.cpp
    codec_glue/VoiceEncoder_Speex.cpp
    codec_glue/voice_codec_frame.cpp
    codec_glue/wave_header.cpp
)
target_include_directories(vr_codec_glue PUBLIC codec_glue)
target_include_directories(vr_codec_glue PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/speex/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/silk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/opus/include"
)
target_link_libraries(vr_codec_glue PUBLIC vr_speex vr_skpsilk vr_opus)
if (MSVC)
    target_compile_options(vr_codec_glue PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# miniz (single-file public-domain ZIP read/write) -- used so VoiceRipper
# can batch-process a .zip of multiple .dem files without the user having
# to extract it first.
# ---------------------------------------------------------------------------
add_library(vr_miniz STATIC miniz/miniz.c)
target_include_directories(vr_miniz PUBLIC miniz)
if (MSVC)
    target_compile_options(vr_miniz PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# Umbrella target the main executable links against.
# ---------------------------------------------------------------------------
add_library(vr_codecs INTERFACE)
target_link_libraries(vr_codecs INTERFACE vr_codec_glue vr_miniz)

# ---------------------------------------------------------------------------
# Optional MP3 output via system libmp3lame. WAV output always works
# regardless; MP3 is just a convenience re-encode on top of it.
# ---------------------------------------------------------------------------
find_library(VR_MP3LAME_LIB NAMES mp3lame libmp3lame)
find_path(VR_MP3LAME_INCLUDE_DIR NAMES lame/lame.h)

if (VR_MP3LAME_LIB AND VR_MP3LAME_INCLUDE_DIR)
    message(STATUS "VoiceRipper: libmp3lame found, MP3 export enabled")
    target_compile_definitions(vr_codecs INTERFACE VR_WITH_MP3)
    target_include_directories(vr_codecs INTERFACE ${VR_MP3LAME_INCLUDE_DIR})
    target_link_libraries(vr_codecs INTERFACE ${VR_MP3LAME_LIB})
else()
    message(STATUS "VoiceRipper: libmp3lame not found, building WAV-only (install libmp3lame-dev to enable MP3 export)")
endif()

