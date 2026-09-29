using System.Windows.Input;
using Microsoft.UI.Xaml.Controls;

namespace ClinicAVT.App.Controls;

/// <summary>Flyout items built in code, for the flyouts that have no ItemsSource.</summary>
internal static class MenuItems
{
    public static RadioMenuFlyoutItem Radio(
        string text, string group, bool isChecked, Action choose, object? tag = null)
    {
        var item = new RadioMenuFlyoutItem { Text = text, GroupName = group, IsChecked = isChecked, Tag = tag };
        item.Click += (_, _) => choose();
        return item;
    }

    public static RadioMenuFlyoutItem Radio(
        string text, string group, bool isChecked, ICommand command, object? parameter) =>
        new()
        {
            Text = text,
            GroupName = group,
            IsChecked = isChecked,
            Command = command,
            CommandParameter = parameter,
        };

    public static MenuFlyoutItem Caption(string text) => new() { Text = text, IsEnabled = false };
}
