using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace ClinicAVT.Client.Tests.TestDoubles;

/// <summary>In-process pipe server driven by a per-request script.</summary>
internal sealed class FakeEngine : IAsyncDisposable
{
    private readonly NamedPipeServerStream _pipe;
    private readonly Func<JsonDocument, IEnumerable<string>> _script;
    private readonly Task _serveLoop;

    public FakeEngine(string pipeName, Func<JsonDocument, IEnumerable<string>> script)
    {
        _pipe = new NamedPipeServerStream(
            pipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        _script = script;
        _serveLoop = ServeAsync();
    }

    public string LastRequestJson { get; private set; } = "";

    private async Task ServeAsync()
    {
        await _pipe.WaitForConnectionAsync().ConfigureAwait(false);
        while (true)
        {
            var body = await Framing.ReadFrameAsync(_pipe).ConfigureAwait(false);
            LastRequestJson = Encoding.UTF8.GetString(body);
            using var request = JsonDocument.Parse(body);
            foreach (var reply in _script(request))
            {
                var frame = Framing.Encode(Encoding.UTF8.GetBytes(reply));
                await _pipe.WriteAsync(frame).ConfigureAwait(false);
            }
        }
    }

    public async ValueTask DisposeAsync()
    {
        await _pipe.DisposeAsync().ConfigureAwait(false);
        try
        {
            await _serveLoop.ConfigureAwait(false);
        }
        catch (Exception)
        {
            // Disposal closes the pipe while the loop is still reading
        }
    }
}
