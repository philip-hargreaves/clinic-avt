using System.Numerics;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;
using ClinicAVT.App.Core.Shell;
using ClinicAVT.App.Themes;

namespace ClinicAVT.App.Controls;

/// <summary>
/// A soft accent glow behind a disc button that grows with the microphone level, and a ring
/// that eases outward from the disc.
/// </summary>
public sealed partial class LevelRing : UserControl
{
    public static readonly DependencyProperty LevelProperty = DependencyProperty.Register(
        nameof(Level), typeof(double), typeof(LevelRing),
        new PropertyMetadata(0.0, (d, _) => ((LevelRing)d).Apply()));

    public static readonly DependencyProperty DiameterProperty = DependencyProperty.Register(
        nameof(Diameter), typeof(double), typeof(LevelRing),
        new PropertyMetadata(88.0, (d, _) => ((LevelRing)d).Apply()));

    private readonly RadialGradientBrush _glow = new();
    private readonly SolidColorBrush _ring = new();
    private Color _accent = Colors.CornflowerBlue;

    public LevelRing()
    {
        InitializeComponent();
        _glow.GradientStops.Add(new GradientStop { Offset = 0.0 });
        _glow.GradientStops.Add(new GradientStop { Offset = 0.55 });
        _glow.GradientStops.Add(new GradientStop { Offset = 1.0 });
        Glow.Fill = _glow;
        Ring.Stroke = _ring;
        ActualThemeChanged += (_, _) => ReadAccent();
        ReadAccent();
    }

    /// <summary>Microphone level, 0 to 1.</summary>
    public double Level
    {
        get => (double)GetValue(LevelProperty);
        set => SetValue(LevelProperty, value);
    }

    /// <summary>The disc the ring sits behind.</summary>
    public double Diameter
    {
        get => (double)GetValue(DiameterProperty);
        set => SetValue(DiameterProperty, value);
    }

    private void ReadAccent()
    {
        if (ThemedResources.Find("ColorAccent", ActualTheme) is Color colour)
        {
            _accent = colour;
        }

        Apply();
    }

    private Color WithAlpha(double alpha) =>
        Color.FromArgb((byte)Math.Round(255 * Math.Clamp(alpha, 0.0, 1.0)), _accent.R, _accent.G, _accent.B);

    private void Apply()
    {
        var level = Level;
        Glow.Width = Glow.Height = Diameter;
        Ring.Width = Ring.Height = Diameter + 6;
        Glow.CenterPoint = new Vector3((float)(Diameter / 2), (float)(Diameter / 2), 0);
        Ring.CenterPoint = new Vector3((float)((Diameter + 6) / 2), (float)((Diameter + 6) / 2), 0);
        var glow = (float)LevelCurve.GlowScale(level);
        var ring = (float)LevelCurve.RingScale(level);
        Glow.Scale = new Vector3(glow, glow, 1);
        Ring.Scale = new Vector3(ring, ring, 1);
        _glow.GradientStops[0].Color = WithAlpha(LevelCurve.GlowAlpha(level));
        _glow.GradientStops[1].Color = WithAlpha(LevelCurve.GlowAlpha(level) * 0.45);
        _glow.GradientStops[2].Color = WithAlpha(0);
        _ring.Color = WithAlpha(LevelCurve.RingAlpha(level));
    }
}
