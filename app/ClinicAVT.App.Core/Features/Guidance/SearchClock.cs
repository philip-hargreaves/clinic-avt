namespace ClinicAVT.App.Core.Features.Guidance;

internal static class SearchClock
{
    // A search the clock never timed, such as a stored record, shows nothing
    public static string FoundIn(TimeProvider time, ref long? started)
    {
        if (started is not { } since)
        {
            return "";
        }

        started = null;
        return $"found in {time.GetElapsedTime(since).TotalSeconds:0.0} s";
    }
}
