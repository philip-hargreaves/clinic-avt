using System.Text.Json;

namespace ClinicAVT.App.Tests.Support;

internal static class Waits
{
    private static readonly TimeSpan DefaultLimit = TimeSpan.FromSeconds(5);

    // A cold engine verifies its models before it answers
    private static readonly TimeSpan ConnectBudget = TimeSpan.FromSeconds(60);

    public static async Task WaitUntilAsync(Func<bool> condition, TimeSpan? limit = null)
    {
        var deadline = DateTime.UtcNow + (limit ?? DefaultLimit);
        while (!condition())
        {
            Assert.True(DateTime.UtcNow < deadline, "condition not reached in time");
            await Task.Delay(10);
        }
    }

    /// <summary>Repeats a request that fails fast while the connection dials or redials.</summary>
    public static async Task<JsonElement> RetryAsync(Func<Task<JsonElement>> request, TimeSpan? budget = null)
    {
        var deadline = DateTime.UtcNow + (budget ?? ConnectBudget);
        while (true)
        {
            try
            {
                return await request();
            }
            catch (IOException e) when (DateTime.UtcNow < deadline)
            {
                if (DateTime.UtcNow + TimeSpan.FromMilliseconds(50) >= deadline)
                {
                    throw new IOException($"gave up: {e}", e);
                }

                await Task.Delay(50);
            }
        }
    }
}
