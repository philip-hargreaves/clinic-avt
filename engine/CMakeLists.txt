# Core and ports have their own include root, so a core file that includes an adapter fails to
# build. Core has no dependencies. OpenVINO and Win32 code lives in adapters
add_library(clinicavt_core STATIC
    domain/core/archive/backup.cpp
    domain/core/archive/record_rules.cpp
    domain/core/common/log.cpp
    domain/core/demo/sample_year.cpp
    domain/core/diarisation/resplit.cpp
    domain/core/diarisation/role_naming.cpp
    domain/core/diarisation/tidy_transcript.cpp
    domain/core/diarisation/turn_decode.cpp
    domain/core/guidance/document_units.cpp
    domain/core/guidance/guidance_query.cpp
    domain/core/guidance/guidance_rank.cpp
    domain/core/guidance/page_clean.cpp
    domain/core/guidance/page_text.cpp
    domain/core/guidance/patient_screen.cpp
    domain/core/guidance/recommendation_marks.cpp
    domain/core/guidance/reference_tail.cpp
    domain/core/records/reflections.cpp
    domain/core/records/session_records.cpp
    domain/core/session/capture_lane.cpp
    domain/core/session/enrolment.cpp
    domain/core/session/finaliser.cpp
    domain/core/session/import_job.cpp
    domain/core/session/note_lane.cpp
    domain/core/session/review.cpp
    domain/core/session/session_controller.cpp
    domain/core/session/transcribe_recording.cpp)
target_include_directories(clinicavt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/domain)
target_link_libraries(clinicavt_core PRIVATE clinicavt_warnings)
target_compile_definitions(clinicavt_core PUBLIC CLINICAVT_VERSION="${clinicavt_VERSION}")

add_library(clinicavt_adapters_root INTERFACE)
target_include_directories(clinicavt_adapters_root INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clinicavt_adapters_root INTERFACE clinicavt_core)

add_library(clinicavt_system STATIC
    src/adapters/system/awake_request.cpp
    src/adapters/system/child_process.cpp
    src/adapters/system/com_apartment.cpp
    src/adapters/system/exe_paths.cpp
    src/adapters/system/gpu_lease.cpp
    src/adapters/system/machine_memory.cpp
    src/adapters/system/power_throttling.cpp
    src/adapters/system/process_scan.cpp
    src/adapters/system/recycle_bin.cpp
    src/adapters/system/stderr_log.cpp)
target_link_libraries(clinicavt_system
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_warnings dxgi ole32 shell32)

# Pipe client the engine uses to talk to the note host
add_library(clinicavt_pipe_client STATIC
    src/adapters/ipc/pipe_client.cpp)
target_link_libraries(clinicavt_pipe_client
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_warnings)

# Toolchain is fetched and pinned by tools/get-openvino.ps1
if(NOT EXISTS "${clinicavt_SOURCE_DIR}/external/openvino")
    message(FATAL_ERROR "OpenVINO not found: run tools/get-openvino.ps1 first")
endif()
find_package(OpenVINOGenAI REQUIRED)

# Archive ships Debug and Release runtimes. Shipping trims this to the minimal set
set(_ov_root "${clinicavt_SOURCE_DIR}/external/openvino/runtime")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(GLOB _ov_dlls "${_ov_root}/bin/intel64/Debug/*.dll"
         "${_ov_root}/bin/intel64/Debug/cache.json"
         "${_ov_root}/3rdparty/tbb/bin/*_debug.dll")
else()
    file(GLOB _ov_dlls "${_ov_root}/bin/intel64/Release/*.dll"
         "${_ov_root}/bin/intel64/Release/cache.json")
    file(GLOB _tbb_dlls "${_ov_root}/3rdparty/tbb/bin/*.dll")
    list(FILTER _tbb_dlls EXCLUDE REGEX "_debug")
    list(APPEND _ov_dlls ${_tbb_dlls})
endif()
# One deployment per output directory. Targets there depend on the stamp
function(clinicavt_deploy_openvino stamp_target)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ov.deployed"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ov_dlls}
                "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/ov.deployed"
        VERBATIM)
    add_custom_target(${stamp_target} DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ov.deployed")
endfunction()

# PDFium for the ingest host. tools/get-pdfium.ps1 fetches and pins it
set(_pdfium_root "${clinicavt_SOURCE_DIR}/external/pdfium")
if(NOT EXISTS "${_pdfium_root}")
    message(FATAL_ERROR "PDFium not found: run tools/get-pdfium.ps1 first")
endif()
add_library(pdfium SHARED IMPORTED)
set_target_properties(pdfium PROPERTIES
    IMPORTED_LOCATION "${_pdfium_root}/bin/pdfium.dll"
    IMPORTED_IMPLIB "${_pdfium_root}/lib/pdfium.dll.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${_pdfium_root}/include"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_pdfium_root}/include")

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b  # v3.12.0
    SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

# Guidance result JSON, in its own library so ipc does not link the guidance adapter
add_library(clinicavt_guidance_record STATIC
    src/adapters/guidance/guidance_record.cpp)
target_link_libraries(clinicavt_guidance_record
    PUBLIC clinicavt_adapters_root nlohmann_json::nlohmann_json
    PRIVATE clinicavt_warnings)

add_library(clinicavt_ipc STATIC
    src/adapters/ipc/archive_handlers.cpp
    src/adapters/ipc/engine_handlers.cpp
    src/adapters/ipc/guidance_handlers.cpp
    src/adapters/ipc/import_handlers.cpp
    src/adapters/ipc/reflection_handlers.cpp
    src/adapters/ipc/session_handlers.cpp
    src/adapters/ipc/translate_handlers.cpp
    src/adapters/ipc/pipe_security.cpp
    src/adapters/ipc/pipe_server.cpp
    src/adapters/ipc/wire_events.cpp)
target_link_libraries(clinicavt_ipc
    PUBLIC clinicavt_adapters_root clinicavt_guidance_record clinicavt_models nlohmann_json::nlohmann_json
    PRIVATE clinicavt_warnings clinicavt_archive clinicavt_audio clinicavt_system advapi32)

FetchContent_Declare(
    sqlite3
    URL https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)

enable_language(C)
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_LOAD_EXTENSION)
# Not ours to keep warning-clean
set_target_properties(sqlite3 PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# The schema and its upgrade steps are SQL files, embedded so the engine stays a single binary
set(clinicavt_schema_dir ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/storage/schema)
file(READ ${clinicavt_schema_dir}/clinicavt.sql CLINICAVT_SCHEMA_SQL)
file(READ ${clinicavt_schema_dir}/migrate-6-to-7.sql CLINICAVT_MIGRATE_6_TO_7_SQL)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${clinicavt_schema_dir}/clinicavt.sql ${clinicavt_schema_dir}/migrate-6-to-7.sql)
configure_file(src/adapters/storage/schema.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/adapters/storage/schema.hpp @ONLY)

add_library(clinicavt_storage STATIC
    src/adapters/storage/chunk_cipher.cpp
    src/adapters/storage/db.cpp
    src/adapters/storage/reflection_json.cpp
    src/adapters/storage/sqlite_session_store.cpp
    src/adapters/storage/sqlite_store_documents.cpp
    src/adapters/storage/sqlite_store_records.cpp
    src/adapters/storage/sqlite_store_seeding.cpp
    src/adapters/storage/sqlite_store_writer.cpp
    src/adapters/storage/store_migrations.cpp)
target_include_directories(clinicavt_storage PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(clinicavt_storage
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_warnings sqlite3 bcrypt crypt32 nlohmann_json::nlohmann_json)

add_library(clinicavt_archive STATIC
    src/adapters/archive/archive_file.cpp
    src/adapters/archive/archive_lane.cpp
    src/adapters/archive/archive_password.cpp
    src/adapters/archive/archive_record.cpp)
target_link_libraries(clinicavt_archive
    PUBLIC clinicavt_adapters_root nlohmann_json::nlohmann_json
    PRIVATE clinicavt_storage clinicavt_warnings bcrypt)

# The sample year read from demo/reflections
add_library(clinicavt_demo STATIC
    src/adapters/demo/json_sample_source.cpp)
target_link_libraries(clinicavt_demo
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_warnings nlohmann_json::nlohmann_json)

add_library(clinicavt_models STATIC
    src/adapters/models/model_store.cpp
    src/adapters/models/ov_runtime.cpp
    src/adapters/system/sha256.cpp)
target_link_libraries(clinicavt_models
    PUBLIC clinicavt_adapters_root nlohmann_json::nlohmann_json openvino::genai
    PRIVATE clinicavt_system clinicavt_warnings bcrypt)

add_library(clinicavt_transcription STATIC
    src/adapters/transcription/whisper_transcriber.cpp)
target_link_libraries(clinicavt_transcription
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_models clinicavt_system clinicavt_warnings openvino::genai)

add_library(clinicavt_note STATIC
    src/adapters/note/llm_note_writer.cpp
    src/adapters/note/text_pipeline.cpp
    src/adapters/note/note_host_process.cpp
    src/adapters/note/worker_note_writer.cpp)
target_link_libraries(clinicavt_note
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_models clinicavt_pipe_client clinicavt_system clinicavt_warnings
            nlohmann_json::nlohmann_json openvino::genai)

add_library(clinicavt_translate STATIC
    src/adapters/translate/nllb_translator.cpp)
target_link_libraries(clinicavt_translate
    PUBLIC clinicavt_adapters_root nlohmann_json::nlohmann_json
    PRIVATE clinicavt_models clinicavt_warnings openvino::genai)

# The corpus schema is a SQL file, embedded like the session schema
set(clinicavt_guidance_schema_dir ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/guidance/schema)
file(READ ${clinicavt_guidance_schema_dir}/corpus.sql CLINICAVT_CORPUS_SCHEMA_SQL)
file(READ ${clinicavt_guidance_schema_dir}/index.sql CLINICAVT_INDEX_SCHEMA_SQL)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${clinicavt_guidance_schema_dir}/corpus.sql ${clinicavt_guidance_schema_dir}/index.sql)
configure_file(src/adapters/guidance/schema.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/adapters/guidance/schema.hpp @ONLY)

add_library(clinicavt_guidance STATIC
    src/adapters/guidance/document_index.cpp
    src/adapters/guidance/document_ingest.cpp
    src/adapters/guidance/folder_scan.cpp
    src/adapters/guidance/ingest_host.cpp
    src/adapters/guidance/corpus_store.cpp
    src/adapters/guidance/embedder.cpp
    src/adapters/guidance/guidance_lane.cpp
    src/adapters/guidance/retriever.cpp)
target_include_directories(clinicavt_guidance PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(clinicavt_guidance
    PUBLIC clinicavt_adapters_root clinicavt_models clinicavt_system nlohmann_json::nlohmann_json
    PRIVATE clinicavt_storage clinicavt_warnings openvino::genai bcrypt)

add_library(clinicavt_vad STATIC
    src/adapters/vad/silero_vad.cpp)
target_link_libraries(clinicavt_vad
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_models clinicavt_warnings openvino::genai)

# fastcluster average linkage, which matched sklearn in the research. It has no upstream CMake
# build, so it is compiled from source like sqlite
FetchContent_Declare(
    hclust_cpp
    GIT_REPOSITORY https://github.com/cdalitz/hclust-cpp.git
    GIT_TAG d48fff6bba1199d80422cd37f5b635107a5a0c92
    SYSTEM
)
FetchContent_MakeAvailable(hclust_cpp)
add_library(hclust STATIC ${hclust_cpp_SOURCE_DIR}/fastcluster.cpp)
target_include_directories(hclust SYSTEM PUBLIC ${hclust_cpp_SOURCE_DIR})
# Not ours to keep warning-clean
set_target_properties(hclust PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# Feature front-end for the speaker embedder, pinned to the version the
# research validated against torchaudio. kissfft is fetched and hash-pinned
# by knf's own build
set(KALDI_NATIVE_FBANK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KALDI_NATIVE_FBANK_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(
    kaldi_native_fbank
    GIT_REPOSITORY https://github.com/csukuangfj/kaldi-native-fbank.git
    GIT_TAG b09e686fe2084732ddd30d1ef80acfc0f13eaf01  # v1.22.3
    SYSTEM
)
FetchContent_MakeAvailable(kaldi_native_fbank)
# Not ours to keep warning-clean
set_target_properties(kaldi-native-fbank-core kissfft PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

add_library(clinicavt_diarisation STATIC
    src/adapters/diarisation/anchor_store.cpp
    src/adapters/diarisation/capture_stage.cpp
    src/adapters/diarisation/fbank.cpp
    src/adapters/diarisation/segmenter.cpp
    src/adapters/diarisation/speaker_clustering.cpp
    src/adapters/diarisation/speaker_diariser.cpp
    src/adapters/diarisation/speaker_embedder.cpp)
target_include_directories(clinicavt_diarisation SYSTEM PRIVATE ${kaldi_native_fbank_SOURCE_DIR})
target_link_libraries(clinicavt_diarisation
    PUBLIC clinicavt_adapters_root clinicavt_models clinicavt_vad
    PRIVATE clinicavt_warnings crypt32 hclust kaldi-native-fbank-core)

add_library(clinicavt_audio STATIC
    src/adapters/audio/capture_devices.cpp
    src/adapters/audio/media_foundation_reader.cpp
    src/adapters/audio/wasapi_capture.cpp
    src/adapters/audio/wav_source.cpp)
target_link_libraries(clinicavt_audio
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_system clinicavt_warnings ole32 advapi32 mfplat mfreadwrite mfuuid delayimp)
# Windows N editions and Server may lack Media Foundation. The engine must still start there
target_link_options(clinicavt_audio INTERFACE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)

add_executable(clinicavt_engine
    src/engine_main.cpp
    src/composition/engine_config.cpp
    src/composition/guidelines_setup.cpp
    src/composition/model_roles.cpp)
target_link_libraries(clinicavt_engine PRIVATE
    clinicavt_ipc clinicavt_audio clinicavt_demo clinicavt_diarisation clinicavt_guidance clinicavt_note
    clinicavt_storage clinicavt_system clinicavt_transcription clinicavt_translate clinicavt_vad
    clinicavt_warnings ole32 shell32)
clinicavt_deploy_openvino(ov_engine_runtime)
add_dependencies(clinicavt_engine ov_engine_runtime)

# Note model host, deployed beside clinicavt_engine, which spawns it
add_executable(clinicavt_note_host src/note_host_main.cpp)
target_link_libraries(clinicavt_note_host PRIVATE
    clinicavt_ipc clinicavt_models clinicavt_note clinicavt_system clinicavt_warnings)
add_dependencies(clinicavt_note_host ov_engine_runtime)
add_dependencies(clinicavt_engine clinicavt_note_host)

# The PDF reader, linked only by the ingest host
add_library(clinicavt_pdf STATIC
    src/adapters/pdf/pdfium_reader.cpp)
target_link_libraries(clinicavt_pdf
    PUBLIC clinicavt_adapters_root
    PRIVATE clinicavt_warnings pdfium nlohmann_json::nlohmann_json)

# Out-of-process PDF parsing so a parser crash only fails that document. wer (Windows Error
# Reporting) is for WerAddExcludedApplication
add_executable(clinicavt_ingest_host src/ingest_host_main.cpp)
target_link_libraries(clinicavt_ingest_host PRIVATE
    clinicavt_pdf clinicavt_system wer clinicavt_warnings)
add_custom_command(TARGET clinicavt_ingest_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_pdfium_root}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:clinicavt_ingest_host>"
    VERBATIM)
add_dependencies(clinicavt_engine clinicavt_ingest_host)

# Corpus build tools, which are not deployed. They use the staged embedder so index and query
# embeddings match
add_library(clinicavt_corpus STATIC
    tools/corpus/chunker.cpp
    tools/corpus/corpus_builder.cpp
    tools/corpus/indexer.cpp)
target_include_directories(clinicavt_corpus
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(clinicavt_corpus
    PUBLIC clinicavt_guidance clinicavt_models clinicavt_system nlohmann_json::nlohmann_json
    PRIVATE clinicavt_storage clinicavt_warnings openvino::genai)

add_executable(clinicavt_index tools/corpus/index_main.cpp)
target_link_libraries(clinicavt_index PRIVATE clinicavt_corpus clinicavt_warnings)
add_dependencies(clinicavt_index ov_engine_runtime)

# Prints a PDF's units, to check the chunker on real documents
add_executable(clinicavt_units tools/units_main.cpp)
target_link_libraries(clinicavt_units PRIVATE
    clinicavt_guidance clinicavt_system nlohmann_json::nlohmann_json clinicavt_warnings)
add_dependencies(clinicavt_units clinicavt_ingest_host)

add_subdirectory(tests)
