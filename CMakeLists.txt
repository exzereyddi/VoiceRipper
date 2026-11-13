cmake_minimum_required(VERSION 3.16)
project(VoiceRipper LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W3 /EHsc)
endif()

add_subdirectory(third_party)

add_executable(voiceripper
    src/main.cpp

    # demo/ -- GoldSrc .dem container + bit-level network message parsing
    src/demo/DemoFile.cpp
    src/demo/DemoFile.hpp
    src/demo/BitReader.hpp
    src/demo/NetMsgTypes.hpp
    src/demo/NetMsgParser.cpp
    src/demo/NetMsgParser.hpp

    # voice/ -- voicedata chunk collection, codec decode, normalize, wav/mp3 output
    src/voice/VoiceExtractor.cpp
    src/voice/VoiceExtractor.hpp
    src/voice/VoiceCodecs.cpp
    src/voice/VoiceCodecs.hpp
    src/voice/WavWriter.cpp
    src/voice/WavWriter.hpp
    src/voice/Normalize.cpp
    src/voice/Normalize.hpp

    # batch/ -- CLI: argument parsing, multi-input/zip resolution, per-demo
    # processing, dedup, report printing, UTF-8 path helpers
    src/batch/CliOptions.cpp
    src/batch/CliOptions.hpp
    src/batch/BatchInput.cpp
    src/batch/BatchInput.hpp
    src/batch/DemoProcessing.cpp
    src/batch/DemoProcessing.hpp
    src/batch/ReportPrinter.cpp
    src/batch/ReportPrinter.hpp
    src/batch/Dedup.cpp
    src/batch/Dedup.hpp
    src/batch/PathUtf8.hpp
)

target_include_directories(voiceripper PRIVATE src)
target_link_libraries(voiceripper PRIVATE vr_codecs)

# Developer-only diagnostics: feeds captured voice payloads to every codec
# hypothesis (raw opus at several rates, raw SKP_Silk, P2P-wrapped ladder,
# legacy speex frames) to identify a format. Not shipped, not needed to
# build/use the main tool.
add_executable(codecprobe tools/codecprobe.cpp)
target_link_libraries(codecprobe PRIVATE vr_codecs)
if (MSVC)
    target_compile_options(codecprobe PRIVATE /W0)
endif()

# Same purpose as codecprobe, focused on the SKP_Silk encoder: produces
# reference frames in known formats to compare against what a demo's
# voice chunks look like.
add_executable(silkenc_test tools/silkenc_test.cpp)
target_link_libraries(silkenc_test PRIVATE vr_skpsilk)
if (MSVC)
    target_compile_options(silkenc_test PRIVATE /W0)
endif()

if (WIN32)
    target_link_libraries(voiceripper PRIVATE shell32)
endif()
