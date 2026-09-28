using System.Diagnostics;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Platform;

/// <summary>Memory figures through System.Diagnostics, where a process that has exited reads as null.</summary>
public sealed class ProcessMetrics : IProcessMetrics
{
    private const long Mebibyte = 1024 * 1024;

    public long? PeakWorkingSetMb(int pid) => Of(pid, p => p.PeakWorkingSet64);

    public long? PeakCommitMb(int pid) => Of(pid, p => p.PeakPagedMemorySize64);

    public long? PeakWorkingSetMbOf(string processName)
    {
        try
        {
            var processes = Process.GetProcessesByName(processName);
            try
            {
                return processes.Length == 0 ? null : processes.Max(p => p.PeakWorkingSet64) / Mebibyte;
            }
            finally
            {
                foreach (var process in processes)
                {
                    process.Dispose();
                }
            }
        }
        catch (Exception)
        {
            return null;
        }
    }

    public double CommittedGb(params string[] processNames)
    {
        try
        {
            using var self = Process.GetCurrentProcess();
            var bytes = self.PrivateMemorySize64;
            foreach (var name in processNames)
            {
                foreach (var process in Process.GetProcessesByName(name))
                {
                    using (process)
                    {
                        bytes += process.PrivateMemorySize64;
                    }
                }
            }

            return bytes / (1024.0 * Mebibyte);
        }
        catch (Exception)
        {
            return 0;
        }
    }

    private static long? Of(int pid, Func<Process, long> metric)
    {
        try
        {
            using var process = Process.GetProcessById(pid);
            return metric(process) / Mebibyte;
        }
        catch (Exception)
        {
            return null;
        }
    }
}
