using Microsoft.Extensions.DependencyInjection;
using ClinicAVT.App.Core.Features.Guidance;
using ClinicAVT.App.Core.Ports;
using ClinicAVT.App.Tests.Support;
using ClinicAVT.App.Tests.TestDoubles;

namespace ClinicAVT.App.Tests.Features.Guidance;

public class PageViewModelTest
{
    private static GuidanceRecommendation Found(int page = 1, int pages = 5) =>
        GuidanceRecords.Found(GuidanceRecords.DocumentResult(page, pages, document: 7), "", false);

    private static object Box(int page, double left, double top, double right, double bottom) =>
        new { page, left, top, right, bottom };

    private static object Reply(params object[] boxes) =>
        new { path = @"C:\scratch\page-7-1.bmp", width = 1000, height = 1400, pages = 5, boxes };

    private static (PageViewModel View, FakeEngineClient Engine) Create(object? reply) =>
        Create(reply, new FakeLauncher(), new FakeClipboard());

    private static (PageViewModel View, FakeEngineClient Engine) Create(
        object? reply, FakeLauncher launcher, FakeClipboard clipboard)
    {
        var engine = new FakeEngineClient { PageReply = reply };
        var view = new TestShell(engine, configure: services =>
        {
            services.AddSingleton<ILauncher>(launcher);
            services.AddSingleton<IClipboard>(clipboard);
        }).Get<PageViewModel>();
        return (view, engine);
    }

    [Fact]
    public async Task EveryShowIsAnnouncedOpenHandsTheCopyToTheViewerAndThePaneClosesByCommandOrWhenItsCardGoes()
    {
        var clipboard = new FakeClipboard();
        var launcher = new FakeLauncher();
        var (view, engine) = Create(Reply(Box(1, 0.1, 0.2, 0.6, 0.3)), launcher, clipboard);
        engine.OpenedPath = @"C:\scratch\7.pdf";
        var shown = 0;
        view.Shown += () => shown++;

        await view.ShowAsync(Found());
        await view.ShowAsync(Found());  // announced even while the page is already open
        Assert.True(view.Visible);
        Assert.Equal(2, shown);

        await view.CopyCitationCommand.ExecuteAsync(null);
        view.CloseCommand.Execute(null);
        Assert.Equal(["BSR PMR guidelines 2009, page 2, 1.2 (added 15 Sep 2026)"], clipboard.Copied);
        Assert.False(view.Visible);

        await view.ShowAsync(Found());
        view.KeepOnlyFor([new GuidanceCard([Found()])]);
        Assert.True(view.Visible);
        view.KeepOnlyFor([]);
        Assert.False(view.Visible);

        await view.OpenAsync(Found());
        Assert.Equal([@"C:\scratch\7.pdf"], launcher.Files);
        var request = Assert.Single(engine.Requests, r => r.Method == "guidance/documents/open");
        Assert.Equal("{\"id\":7}", request.Params);
    }

    [Fact]
    public async Task ShowingAPassageAsksForItsPageScalesTheBoxesAndThePagesTurnOneAtATime()
    {
        var (view, engine) =
            Create(Reply(Box(1, 0.1, 0.8, 0.9, 0.95), Box(2, 0.1, 0.05, 0.9, 0.2)));

        await view.ShowAsync(Found());

        Assert.True(view.Visible);
        Assert.False(view.Loading);
        Assert.False(view.Failed);
        Assert.Equal("BSR PMR guidelines 2009", view.DocumentName);
        Assert.Equal("Page 2 of 5", view.PageLabel);
        Assert.Equal(@"C:\scratch\page-7-1.bmp", view.ImagePath);
        Assert.Equal(1000, view.Width);
        Assert.Equal(1400, view.Height);
        var box = Assert.Single(view.Boxes);
        Assert.Equal(100, box.Left, 3);
        Assert.Equal(0.8 * 1400, box.Top, 3);
        Assert.Equal(800, box.Width, 3);
        Assert.Equal(0.15 * 1400, box.Height, 3);
        Assert.NotNull(view.Focus);
        Assert.Equal(100, view.Focus.Left, 3);
        Assert.Equal(800, view.Focus.Width, 3);
        Assert.True(view.CanGoBack);
        Assert.True(view.CanGoForward);
        Assert.False(view.OffPassage);
        Assert.Equal("Page 2 of BSR PMR guidelines 2009, 1.2 highlighted", view.ImageName);
        var request = Assert.Single(engine.Requests, r => r.Method == "guidance/page");
        Assert.Equal("{\"id\":7,\"page\":1,\"chunkId\":\"upload:7-4\"}", request.Params);

        await view.NextPageCommand.ExecuteAsync(null);

        Assert.Equal(2, engine.Requests.Count(r => r.Method == "guidance/page"));
        Assert.Contains("\"page\":2", engine.Requests[^1].Params);
        Assert.Equal("Page 3 of 5", view.PageLabel);
        Assert.Equal(0.05 * 1400, Assert.Single(view.Boxes).Top, 3);
        Assert.True(view.OffPassage);

        await view.NextPageCommand.ExecuteAsync(null);

        Assert.Equal("Page 4 of 5", view.PageLabel);
        Assert.Empty(view.Boxes);
        Assert.Null(view.Focus);
        Assert.Equal("Page 4 of BSR PMR guidelines 2009", view.ImageName);

        await view.NextPageCommand.ExecuteAsync(null);

        Assert.Equal("Page 5 of 5", view.PageLabel);
        Assert.False(view.CanGoForward);
        Assert.False(view.NextPageCommand.CanExecute(null));

        await view.BackToPassageCommand.ExecuteAsync(null);

        Assert.Equal("Page 2 of 5", view.PageLabel);
        Assert.False(view.OffPassage);
        Assert.Single(view.Boxes);

        await view.ShowAsync(Found(page: 0));

        Assert.False(view.CanGoBack);
        Assert.False(view.PreviousPageCommand.CanExecute(null));
        Assert.True(view.CanGoForward);
    }

    [Fact]
    public async Task AFailedDrawSaysSoAndTryAgainAsksAgain()
    {
        var (view, engine) = Create(Reply(Box(1, 0.1, 0.2, 0.6, 0.3)));
        engine.FailNext = method =>
            method == "guidance/page" ? new InvalidOperationException("host crashed") : null;

        await view.ShowAsync(Found());

        Assert.True(view.Visible);
        Assert.True(view.Failed);
        Assert.False(view.Loading);
        Assert.Empty(view.Boxes);

        await view.TryAgainCommand.ExecuteAsync(null);

        Assert.False(view.Failed);
        Assert.Single(view.Boxes);
        Assert.Equal(2, engine.Requests.Count(r => r.Method == "guidance/page"));
    }
}
