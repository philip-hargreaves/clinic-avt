using ClinicAVT.Client.Tests.Support;

namespace ClinicAVT.Client.Tests.Contract;

/// <summary>
/// The engine/models method against the real engine and staged manifests.
/// </summary>
[Collection("engine")]
[Trait("Requires", "Engine")]
public class ModelsContractTest
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task StagedManifestsAreListedWithTheirRoleAndWhichModelEachRoleLoads()
    {
        var modelsRoot = Path.Combine(Path.GetTempPath(), $"clinicavt-models-{Guid.NewGuid():N}");
        foreach (var (id, task, tier, device, file) in new[]
                 {
                     ("silero-vad", "vad", "default", "CPU", "model.onnx"),
                     ("qwen-small", "note", "default", "GPU", "model.xml"),
                     ("qwen-large", "note", "accuracy", "GPU", "model.xml"),
                 })
        {
            Directory.CreateDirectory(Path.Combine(modelsRoot, id));
            await File.WriteAllTextAsync(
                Path.Combine(modelsRoot, id, "manifest.json"),
                $$$"""
                {"manifestVersion": 1, "id": "{{{id}}}", "task": "{{{task}}}", "tier": "{{{tier}}}",
                 "licence": "MIT", "runtime": {"device": "{{{device}}}"}, "files": {"{{{file}}}": "00"}}
                """);
        }

        try
        {
            await using var engine = EngineProcess.Start(
                $"LOCAL\\clinicavt-models-{Guid.NewGuid():N}", null, modelsRoot);
            await using var client = await engine.ConnectAsync();

            var models = (await client.RequestAsync("engine/models", null, Timeout))
                .GetProperty("models").EnumerateArray().ToList();

            Assert.Equal(3, models.Count);
            var vad = models.Single(m => m.GetProperty("id").GetString() == "silero-vad");
            Assert.Equal("silero-vad", vad.GetProperty("name").GetString());  // without a display name the name is the id
            Assert.Equal("vad", vad.GetProperty("task").GetString());
            Assert.Equal("default", vad.GetProperty("tier").GetString());
            Assert.Equal("CPU", vad.GetProperty("device").GetString());
            foreach (var note in models.Where(m => m.GetProperty("task").GetString() == "note"))
            {
                var expected = note.GetProperty("tier").GetString() == "default";
                Assert.Equal(expected, note.GetProperty("active").GetBoolean());
            }
        }
        finally
        {
            Directory.Delete(modelsRoot, recursive: true);
        }
    }
}
