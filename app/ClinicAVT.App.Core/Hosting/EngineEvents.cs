using Microsoft.Extensions.Logging;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.Client;

namespace ClinicAVT.App.Core.Hosting;

/// <summary>
/// Moves engine events to the UI thread and hands each to its subscribers in subscription order.
/// A handler that throws is logged and the rest still run.
/// </summary>
public sealed class EngineEvents : IEngineEvents
{
    private readonly IEngineLink _engine;
    private readonly IUiDispatcher _dispatcher;
    private readonly ILogger<EngineEvents> _logger;
    private readonly object _gate = new();
    private Subscription[] _notifications = [];
    private Subscription[] _connection = [];

    public EngineEvents(IEngineLink engine, IUiDispatcher dispatcher, ILogger<EngineEvents> logger)
    {
        _engine = engine;
        _dispatcher = dispatcher;
        _logger = logger;
        engine.NotificationReceived += notification => dispatcher.Post(() => Deliver(notification));
        engine.ConnectedChanged += connected => dispatcher.Post(() => Deliver(connected));
    }

    public bool Connected => _engine.Connected;

    public IDisposable Subscribe<T>(Action<T> handler)
        where T : EngineNotification =>
        Add(ref _notifications, value =>
        {
            if (value is T notification)
            {
                handler(notification);
            }
        });

    public IDisposable SubscribeConnection(Action<bool> handler) =>
        Add(ref _connection, value => handler((bool)value));

    private void Deliver(EngineNotification notification)
    {
        foreach (var subscription in Volatile.Read(ref _notifications))
        {
            try
            {
                subscription.Handle(notification);
            }
            catch (Exception e)
            {
                _logger.NotificationHandlerFailed(notification.GetType().Name, e.Message);
            }
        }
    }

    private void Deliver(bool connected)
    {
        foreach (var subscription in Volatile.Read(ref _connection))
        {
            subscription.Handle(connected);
        }
    }

    private Subscription Add(ref Subscription[] list, Action<object> handle)
    {
        var subscription = new Subscription(this, handle);
        lock (_gate)
        {
            list = [.. list, subscription];
        }

        return subscription;
    }

    private void Remove(Subscription subscription)
    {
        lock (_gate)
        {
            _notifications = [.. _notifications.Where(s => s != subscription)];
            _connection = [.. _connection.Where(s => s != subscription)];
        }
    }

    private sealed class Subscription(EngineEvents owner, Action<object> handle) : IDisposable
    {
        public void Handle(object value) => handle(value);

        public void Dispose() => owner.Remove(this);
    }
}

public static class EngineEventsExtensions
{
    /// <summary>Runs the action now when connected, and again on every connect.</summary>
    public static IDisposable OnConnected(this IEngineEvents events, Action action)
    {
        var subscription = events.SubscribeConnection(connected =>
        {
            if (connected)
            {
                action();
            }
        });
        if (events.Connected)
        {
            action();
        }

        return subscription;
    }
}
