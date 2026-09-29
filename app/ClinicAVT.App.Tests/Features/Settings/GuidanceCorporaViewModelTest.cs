using ClinicAVT.App.Core.Features.Settings;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Settings;

public class GuidanceCorporaViewModelTest
{
    [Fact]
    public void InstalledCorporaFollowTheEngineARefusedOneKeepsItsPlaceAndAnUnavailableModelSaysWhy()
    {
        var engine = new FakeEngineClient();
        var corpora = TestSession.Settings(engine).Get<GuidanceCorporaViewModel>();

        var fixture = Assert.Single(corpora.GuidanceCorpora);
        Assert.Equal("Fixture guidance corpus", fixture.Name);
        Assert.Equal("40 passages · 11 Sep 2026", fixture.Detail);
        Assert.Equal("none", fixture.Attribution);
        Assert.False(fixture.Refused);
        Assert.Equal("", corpora.GuidanceCaption);
        Assert.False(corpora.GuidanceCaptionVisible);
        Assert.True(corpora.GuidanceInstalledVisible);

        // The embedder is reloaded and comes back with a different store
        engine.GuidanceState = "loading";
        engine.GuidanceCorpora.Clear();
        engine.RaiseNotification("guidance/model");
        Assert.Empty(corpora.GuidanceCorpora);
        Assert.Equal("Loading", corpora.GuidanceCaption);

        engine.GuidanceState = "ready";
        engine.GuidanceCorpora.Add(new
        {
            name = "NICE guidance",
            licence = "OGL v3",
            attribution = "Contains public sector information",
            chunks = 22991,
            builtAt = "2026-09-11T21:03:17Z",
        });
        engine.GuidanceCorpora.Add(new
        {
            id = "nice-2026-08",
            unavailable = "corpus.db sha256 does not match the manifest",
        });
        engine.RaiseNotification("guidance/model");

        Assert.Equal(2, corpora.GuidanceCorpora.Count);
        var nice = corpora.GuidanceCorpora[0];
        Assert.Equal("22,991 passages · 11 Sep 2026", nice.Detail);
        Assert.Equal("Contains public sector information", nice.Attribution);
        var refused = corpora.GuidanceCorpora[1];
        Assert.Equal("nice-2026-08", refused.Name);
        Assert.Equal("Not used: corpus.db sha256 does not match the manifest", refused.Detail);
        Assert.True(refused.Refused);
        Assert.False(refused.Loaded);
        Assert.Equal("", corpora.GuidanceCaption);

        // Nothing installed hides the card
        engine.GuidanceCorpora.Clear();
        engine.RaiseNotification("guidance/model");
        Assert.Empty(corpora.GuidanceCorpora);
        Assert.Equal("", corpora.GuidanceCaption);
        Assert.False(corpora.GuidanceInstalledVisible);

        engine.GuidanceState = "unavailable";
        engine.GuidanceDetail = "guidance embedder gte-large-int8: tokenizer ignores max_length";
        engine.RaiseNotification("guidance/model");

        Assert.Empty(corpora.GuidanceCorpora);
        Assert.Equal(
            "Unavailable: guidance embedder gte-large-int8: tokenizer ignores max_length",
            corpora.GuidanceCaption);
        Assert.True(corpora.GuidanceCaptionVisible);
    }
}
