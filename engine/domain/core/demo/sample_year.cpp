#include "core/demo/sample_year.hpp"

#include <algorithm>
#include <stdexcept>

#include "core/common/iso8601.hpp"
#include "core/note/summary_scrub.hpp"

namespace clinicavt::demo {

namespace {

std::chrono::sys_seconds StartIn(std::chrono::year_month ym, const Sample& sample) {
    using namespace std::chrono;
    const auto month_end =
        static_cast<unsigned>(year_month_day_last{ym.year(), month_day_last{ym.month()}}.day());
    const auto day_number = std::clamp(static_cast<unsigned>(sample.day), 1u, month_end);
    const year_month_day ymd{ym.year(), ym.month(), day{day_number}};
    return sys_days{ymd} + hours{sample.hour} + minutes{sample.minute};
}

}  // namespace

std::chrono::sys_seconds SampleStart(const Sample& sample, std::chrono::sys_seconds now) {
    using namespace std::chrono;
    const year_month_day today{floor<days>(now)};
    const year_month ym = year_month{today.year(), today.month()} - months{sample.months_back};
    const auto start = StartIn(ym, sample);
    // A sample in the current month can fall after now, so it moves back a month
    return start <= now ? start : StartIn(ym - months{1}, sample);
}

std::size_t SeedSampleYear(store::IDocumentStore& documents, store::ISampleStore& sample_store,
                           const records::IReflectionCodec& codec,
                           const std::vector<Sample>& samples, std::chrono::sys_seconds now) {
    std::size_t written = 0;
    for (const Sample& sample : samples) {
        store::SessionSeed seed;
        const auto start = SampleStart(sample, now);
        seed.started_at = Iso8601(start);
        seed.ended_at =
            Iso8601(start + std::chrono::seconds(static_cast<long long>(sample.audio_seconds)));
        seed.turns = sample.turns;
        const auto id = sample_store.Seed(seed);
        documents.SaveDocument(id, store::DocumentKind::kLabel, {.text = sample.label});
        documents.SaveDocument(id, store::DocumentKind::kNote,
                               {.text = sample.note, .style = "prose", .detail = "concise"});
        documents.SaveDocument(id, store::DocumentKind::kPatient, {.text = sample.patient});
        documents.SaveDocument(id, store::DocumentKind::kSummary,
                               {.text = note::ScrubSummary(sample.summary)});
        // The sample files have no references, so none are written
        const records::Answers answers{
            .happened = sample.happened, .learned = sample.learned, .next = sample.next};
        documents.SaveDocument(id, store::DocumentKind::kReflection,
                               {.text = codec.Encode(answers)});
        written += 1;
    }
    return written;
}

bool HasSamples(store::ISessionCatalog& sessions) {
    for (const auto& session : sessions.ListSessions()) {
        if (session.sample) return true;
    }
    return false;
}

std::size_t DemoSamples::SeedOnce(std::chrono::sys_seconds now) {
    if (HasSamples(catalog_)) return 0;
    const auto samples = source_.Load();
    if (samples.empty()) throw std::runtime_error("no sample content beside the engine");
    return SeedSampleYear(documents_, sample_store_, codec_, samples, now);
}

std::size_t DemoSamples::Clear() {
    return sample_store_.ClearDemo();
}

}  // namespace clinicavt::demo
