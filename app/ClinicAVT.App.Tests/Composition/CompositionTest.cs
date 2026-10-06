using ClinicAVT.App.Tests.Support;

namespace ClinicAVT.App.Tests.Composition;

/// <summary>
/// The app's view-model graph over fakes, so a new constructor parameter fails here before launch.
/// </summary>
public class CompositionTest
{
    [Fact]
    public void EveryRegisteredServiceResolves()
    {
        using var shell = new TestShell();

        foreach (var descriptor in shell.Registrations.Where(d => !d.ServiceType.IsGenericTypeDefinition))
        {
            var service = shell.GetService(descriptor.ServiceType);
            Assert.NotNull(service);
            // A dialog's factory builds its view model only when called
            if (descriptor.ServiceType.IsGenericType
                && descriptor.ServiceType.GetGenericTypeDefinition() == typeof(Func<>))
            {
                Assert.NotNull(((Delegate)service).DynamicInvoke());
            }
        }
    }
}
