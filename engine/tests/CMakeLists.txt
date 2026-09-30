include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723  # v1.17.0
    SYSTEM
)
set(BUILD_GMOCK OFF)
set(INSTALL_GTEST OFF)
# gtest defaults to the static CRT; this build is dynamic
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# googletest is not ours to keep warning-clean
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
FetchContent_MakeAvailable(googletest)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

# Core logic only. Links no adapters, so a core test that needs one fails to build
add_executable(core_tests
    core/archive/record_rules_test.cpp
    core/audio/audio_ring_test.cpp
    core/audio/buffered_sink_test.cpp
    core/audio/level_meter_test.cpp
    core/audio/voice_enrolment_test.cpp
    core/common/cli_args_test.cpp
    core/common/strings_test.cpp
    core/common/version_test.cpp
    core/demo/sample_year_test.cpp
    core/diarisation/clip_cuts_test.cpp
    core/diarisation/diar_regions_test.cpp
    core/diarisation/embeddings_test.cpp
    core/diarisation/frontier_test.cpp
    core/diarisation/resplit_test.cpp
    core/diarisation/role_naming_test.cpp
    core/diarisation/slice_refinement_test.cpp
    core/diarisation/tidy_transcript_test.cpp
    core/diarisation/turn_decode_test.cpp
    core/guidance/document_units_test.cpp
    core/guidance/guidance_query_test.cpp
    core/guidance/guidance_rank_test.cpp
    core/guidance/guidance_scan_test.cpp
    core/guidance/page_clean_test.cpp
    core/guidance/page_text_test.cpp
    core/guidance/patient_screen_test.cpp
    core/guidance/recommendation_marks_test.cpp
    core/guidance/reference_tail_test.cpp
    core/metrics/metrics_test.cpp
    core/metrics/throughput_test.cpp
    core/note/model_failure_test.cpp
    core/note/note_gate_test.cpp
    core/note/note_label_test.cpp
    core/session/enrolment_test.cpp
    core/session/session_controller_enrolment_test.cpp
    core/session/session_controller_import_test.cpp
    core/session/session_controller_lifecycle_test.cpp
    core/session/session_controller_note_test.cpp
    core/session/transcribe_recording_test.cpp
    core/translate/plain_punctuation_test.cpp)
target_compile_definitions(core_tests PRIVATE CLINICAVT_CMAKE_VERSION="${clinicavt_VERSION}")
target_link_libraries(core_tests PRIVATE clinicavt_core clinicavt_warnings GTest::gtest_main)

# Adapters, and the core tests that need one or read a JSON fixture
add_executable(engine_tests
    adapters/diarisation/seg_decode_test.cpp
    core/note/summary_scrub_test.cpp
    adapters/diarisation/anchor_store_test.cpp
    adapters/diarisation/deferred_diariser_test.cpp
    adapters/demo/json_sample_source_test.cpp
    adapters/audio/capture_errors_test.cpp
    adapters/audio/capture_timeline_test.cpp
    adapters/storage/chunk_cipher_test.cpp
    adapters/models/compile_properties_test.cpp
    adapters/models/deferred_load_test.cpp
    adapters/models/note_tier_test.cpp
    adapters/models/residency_test.cpp
    adapters/vad/deferred_vad_test.cpp
    adapters/storage/db_test.cpp
    adapters/diarisation/fbank_test.cpp
    adapters/ipc/framing_test.cpp
    adapters/system/child_process_test.cpp
    adapters/system/gpu_lease_test.cpp
    adapters/system/process_scan_test.cpp
    adapters/guidance/guidance_chunker_test.cpp
    adapters/guidance/guidance_corpus_test.cpp
    adapters/guidance/guidance_indexer_test.cpp
    adapters/guidance/guidance_lane_test.cpp
    adapters/guidance/guidance_record_test.cpp
    adapters/guidance/guidance_retriever_test.cpp
    adapters/guidance/document_index_test.cpp
    adapters/guidance/document_ingest_test.cpp
    adapters/guidance/folder_scan_test.cpp
    adapters/guidance/ingest_host_test.cpp
    adapters/audio/capture_devices_test.cpp
    adapters/ipc/engine_handlers_test.cpp
    adapters/ipc/guidance_handlers_test.cpp
    adapters/ipc/import_handlers_test.cpp
    adapters/ipc/reflection_handlers_test.cpp
    adapters/ipc/session_handlers_test.cpp
    adapters/ipc/wire_events_test.cpp
    adapters/ipc/messages_test.cpp
    adapters/models/model_store_test.cpp
    adapters/note/note_tiers_test.cpp
    adapters/note/note_prompt_test.cpp
    adapters/models/ov_smoke_test.cpp
    adapters/ipc/pipe_security_test.cpp
    adapters/ipc/pipe_server_test.cpp
    adapters/system/power_throttling_test.cpp
    adapters/storage/session_store_test.cpp
    adapters/storage/store_schema_test.cpp
    adapters/diarisation/speaker_clustering_test.cpp
    adapters/storage/store_recovery_test.cpp
    adapters/audio/wav_source_test.cpp
    adapters/transcription/whisper_worker_test.cpp
    adapters/archive/archive_file_test.cpp
    adapters/ipc/archive_handlers_test.cpp
    adapters/archive/backup_test.cpp)
target_compile_definitions(engine_tests PRIVATE
    CLINICAVT_CMAKE_VERSION="${clinicavt_VERSION}"
    CLINICAVT_FIXTURE_DIR="${clinicavt_SOURCE_DIR}/schema/fixtures"
    CLINICAVT_GUIDANCE_FIXTURE_DIR="${clinicavt_SOURCE_DIR}/engine/tests/fixtures/guidance"
    CLINICAVT_DEMO_DIR="${clinicavt_SOURCE_DIR}/demo/reflections"
    CLINICAVT_ARCHIVE_FIXTURE_DIR="${clinicavt_SOURCE_DIR}/engine/tests/fixtures/archive"
    CLINICAVT_STORAGE_FIXTURE_DIR="${clinicavt_SOURCE_DIR}/engine/tests/fixtures/storage"
    CLINICAVT_CRASH_HELPER="$<TARGET_FILE:store_crash_helper>")

add_executable(store_crash_helper
    support/store_crash_helper.cpp)
target_link_libraries(store_crash_helper PRIVATE clinicavt_storage clinicavt_warnings)
add_dependencies(engine_tests store_crash_helper)

add_executable(fake_note_host
    support/fake_note_host.cpp)
target_link_libraries(fake_note_host PRIVATE clinicavt_ipc clinicavt_models clinicavt_warnings)
add_dependencies(engine_tests fake_note_host)
target_compile_definitions(engine_tests PRIVATE
    CLINICAVT_FAKE_NOTE_HOST="$<TARGET_FILE:fake_note_host>")

add_executable(fake_ingest_host
    support/fake_ingest_host.cpp)
target_link_libraries(fake_ingest_host PRIVATE clinicavt_warnings)
add_dependencies(engine_tests fake_ingest_host clinicavt_ingest_host)
target_compile_definitions(engine_tests PRIVATE
    CLINICAVT_FAKE_INGEST_HOST="$<TARGET_FILE:fake_ingest_host>"
    CLINICAVT_INGEST_HOST="$<TARGET_FILE:clinicavt_ingest_host>")
target_link_libraries(engine_tests PRIVATE
    clinicavt_archive clinicavt_ipc clinicavt_audio clinicavt_corpus clinicavt_demo clinicavt_diarisation
    clinicavt_guidance
    clinicavt_guidance_record clinicavt_models clinicavt_note clinicavt_pipe_client clinicavt_storage
    clinicavt_system clinicavt_transcription clinicavt_warnings openvino::genai GTest::gtest_main)

# Real-microphone tests live in their own binary under the microphone label,
# so the ci presets can exclude what a runner without audio cannot execute
add_executable(capture_tests
    adapters/audio/wasapi_capture_test.cpp)
target_link_libraries(capture_tests PRIVATE
    clinicavt_audio clinicavt_warnings GTest::gtest_main)

# Imported recordings decode through Media Foundation, which some Windows editions lack
add_executable(recording_tests
    adapters/audio/media_foundation_reader_test.cpp)
target_compile_definitions(recording_tests PRIVATE
    CLINICAVT_RECORDING_FIXTURE_DIR="${clinicavt_SOURCE_DIR}/engine/tests/fixtures/recordings")
target_link_libraries(recording_tests PRIVATE
    clinicavt_audio clinicavt_warnings GTest::gtest_main)

# Same mechanism for tests needing the Intel GPU
add_executable(gpu_tests
    adapters/models/ov_runtime_test.cpp)
target_link_libraries(gpu_tests PRIVATE
    clinicavt_models clinicavt_warnings openvino::genai GTest::gtest_main)

# And for tests needing staged model weights
add_executable(models_tests
    adapters/transcription/asr_wer_test.cpp
    adapters/diarisation/diariser_pipeline_test.cpp
    adapters/diarisation/fbank_parity_test.cpp
    adapters/guidance/guidance_embedder_test.cpp
    adapters/note/llm_note_writer_test.cpp
    adapters/diarisation/seg_model_test.cpp
    adapters/vad/silero_vad_test.cpp
    adapters/diarisation/speaker_embedder_test.cpp
    adapters/translate/nllb_translator_test.cpp
    adapters/note/worker_note_writer_test.cpp)
# For support/primock.hpp
target_include_directories(models_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(models_tests PRIVATE
    CLINICAVT_MODELS_DIR="${clinicavt_SOURCE_DIR}/models"
    CLINICAVT_DIAR_FIXTURE_DIR="${clinicavt_SOURCE_DIR}/engine/tests/fixtures/diarisation"
    CLINICAVT_GUIDANCE_FIXTURE_DIR="${clinicavt_SOURCE_DIR}/engine/tests/fixtures/guidance")
target_link_libraries(models_tests PRIVATE
    clinicavt_corpus clinicavt_diarisation clinicavt_guidance clinicavt_models clinicavt_note
    clinicavt_system clinicavt_transcription clinicavt_translate clinicavt_vad clinicavt_warnings
    nlohmann_json::nlohmann_json openvino::genai GTest::gtest_main)

# Dev evaluation tool for the research acceptance scorer; never shipped
add_executable(diar_eval_runner tools/diar_eval_runner.cpp)
target_link_libraries(diar_eval_runner PRIVATE
    clinicavt_diarisation clinicavt_system clinicavt_transcription clinicavt_warnings)

clinicavt_deploy_openvino(ov_test_runtime)
add_dependencies(diar_eval_runner ov_test_runtime)
add_dependencies(engine_tests ov_test_runtime)
add_dependencies(gpu_tests ov_test_runtime)
add_dependencies(models_tests ov_test_runtime clinicavt_note_host)

include(GoogleTest)
# Cold CI runners can take >5 s (the default) just to start a fresh exe
gtest_discover_tests(core_tests DISCOVERY_TIMEOUT 60)
gtest_discover_tests(engine_tests DISCOVERY_TIMEOUT 60)
gtest_discover_tests(capture_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS microphone)
gtest_discover_tests(recording_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS mediafoundation)
gtest_discover_tests(gpu_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS gpu)
gtest_discover_tests(models_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS models)
