using ClinicAVT.App.Core.Features.Appraisal;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.Client;

namespace ClinicAVT.App.Tests.Features.Appraisal;

public class ReflectionExportTest
{
    [Fact]
    public void FormatCarriesTitleMonthCaseStudyTickedGuidanceAnsweredQuestionsAndTheDeclarationOnlyWithACaseStudy()
    {
        var text = ReflectionExport.Format(new ReflectionEntry(
            "Elbow swelling", "September 2026", "A patient in their forties.",
            "Reassured too quickly.", "", "Check for infection first."));

        Assert.StartsWith("Elbow swelling\nSeptember 2026\n\nCase study\nA patient in their forties.\n\n", text);
        Assert.Contains("What stood out?\nReassured too quickly.\n", text);
        Assert.DoesNotContain("What did I learn?", text);
        Assert.Contains("Would I do anything differently?\nCheck for infection first.\n", text);
        Assert.EndsWith("first.\n\n" + ReflectionExport.Declaration + "\n", text);

        var bare = ReflectionExport.Format(new ReflectionEntry("", "May 2026", "", "x", "", ""));

        Assert.StartsWith("Consultation\nMay 2026\n\nWhat stood out?\nx\n", bare);
        Assert.DoesNotContain("Case study", bare);
        Assert.DoesNotContain(ReflectionExport.Declaration, bare);

        // Ticked guidance follows the case study with its link on its own line
        text = ReflectionExport.Format(new ReflectionEntry(
            "Elbow swelling", "September 2026", "A patient in their forties.", "", "Read it.", "")
        {
            References =
            [
                new ReflectionReference("nice:ng100", "NG100", "Rheumatoid arthritis in adults",
                    "https://www.nice.org.uk/guidance/ng100", "NICE"),
                new ReflectionReference("upload:doc-7", "", "Shingles leaflet", "", "Added document, page 2"),
            ],
        });

        Assert.Contains(
            "A patient in their forties.\n\nGuidance referred to\n"
            + "NG100 Rheumatoid arthritis in adults (NICE)\nhttps://www.nice.org.uk/guidance/ng100\n"
            + "Shingles leaflet (Added document, page 2)\n\nWhat did I learn?\nRead it.\n", text);
    }

    [Theory]
    [InlineData("Mrs Patel came in worried", "a name: Mrs Patel")]
    [InlineData("seen on 12/03/2026 at the surgery", "a date: 12/03/2026")]
    [InlineData("seen on 3 March", "a date: 3 March")]
    [InlineData("NHS number 943 476 5919", "an NHS number: 943 476 5919")]
    public void TheCheckNamesWhatMayIdentifyThePatient(string text, string expected)
    {
        Assert.Contains(expected, IdentifierCheck.Find(text));
        Assert.StartsWith("This may identify the patient: ", IdentifierCheck.Describe(text));
    }

    [Fact]
    public void AnonymisedTextAndBareMonthsPassClean()
    {
        const string text = "A patient in their forties presented with a week of painless swelling "
                            + "over one elbow. Olecranon bursitis was suspected; rest and an "
                            + "anti-inflammatory were agreed. I reassured too quickly.";
        Assert.Empty(IdentifierCheck.Find(text));
        Assert.Equal("", IdentifierCheck.Describe(text));

        // A month with or without its year is not a date
        Assert.Empty(IdentifierCheck.Find("seen in September 2026"));
        Assert.Empty(IdentifierCheck.Find("since March"));
        Assert.Empty(IdentifierCheck.Find("reviewed Sept 2025, then again"));
    }

    // The engine tests share this fixture. Outputs must pass the check and scrubbed inputs must fail it
    [Fact]
    public void TheEngineScrubFixtureAgreesWithTheCheck()
    {
        var rows = Fixtures.Load("summary-scrub.json");

        foreach (var row in rows.EnumerateArray())
        {
            var input = row.GetProperty("in").GetString()!;
            var output = row.GetProperty("out").GetString()!;
            Assert.Empty(IdentifierCheck.Find(output));
            if (input != output)
            {
                Assert.NotEmpty(IdentifierCheck.Find(input));
            }
        }
    }
}
