using ClinicAVT.App.Core.Features.Documents;

namespace ClinicAVT.App.Tests.Features.Documents;

public class TranscriptTurnItemTest
{
    [Theory]
    [InlineData("doctor", "Doctor")]
    [InlineData("patient", "Patient")]
    [InlineData("unknown", "Other speaker")]
    [InlineData("speaker 1", "Speaker 1")]
    [InlineData("speaker 2", "Speaker 2")]
    [InlineData("", "…")]
    public void TheEngineTagShowsAsItsLabel(string tag, string label)
    {
        Assert.Equal(label, new TranscriptTurnItem(tag, "0:00", "text").SpeakerLabel);
    }
}
