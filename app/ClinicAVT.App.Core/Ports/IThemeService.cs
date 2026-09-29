using ClinicAVT.App.Core.Preferences;

namespace ClinicAVT.App.Core.Ports;

public interface IThemeService
{
    void Apply(AppTheme theme);
}
