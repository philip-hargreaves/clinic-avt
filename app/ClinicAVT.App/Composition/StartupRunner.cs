using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Hosting;
using ClinicAVT.App.Core.Ports;

namespace ClinicAVT.App.Composition;

/// <summary>
/// Runs a stage's tasks in registration order. A failed task is logged and the rest still run.
/// </summary>
internal sealed class StartupRunner(IEnumerable<IStartupTask> tasks, ILogger<StartupRunner> logger)
{
    public void Run(StartupStage stage)
    {
        foreach (var task in tasks.Where(t => t.Stage == stage))
        {
            try
            {
                task.Run();
            }
            catch (Exception e)
            {
                logger.StartupTaskFailed(e, task.Name);
            }
        }
    }
}
