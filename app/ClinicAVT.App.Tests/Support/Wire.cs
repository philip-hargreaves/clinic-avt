using System.Text.Json;

namespace ClinicAVT.App.Tests.Support;

internal static class Wire
{
    public static JsonElement Params(object value) => JsonSerializer.SerializeToElement(value);
}
