using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

// Compile alongside Builder.cs with /main:GuiTest; no test framework or game files required.
internal static class GuiTest
{
    private static T Field<T>(BuilderWindow window, string name)
    {
        return (T)typeof(BuilderWindow).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance).GetValue(window);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    [STAThread]
    private static int Main(string[] args)
    {
        Application app = new Application();
        BuilderWindow window = new BuilderWindow();
        Exception failure = null;
        int ticks = 0;
        long previous = 0, maxGap = 0;
        Stopwatch clock = Stopwatch.StartNew();
        Task worker = null;
        DispatcherTimer heartbeat = new DispatcherTimer(DispatcherPriority.Normal) { Interval = TimeSpan.FromMilliseconds(20) };
        heartbeat.Tick += delegate {
            long now = clock.ElapsedMilliseconds;
            if (previous != 0) maxGap = Math.Max(maxGap, now - previous);
            previous = now;
            ticks++;
        };
        window.Loaded += async delegate {
            try
            {
                // Cover long lines and a sustained output burst, not just a window that opens idle.
                heartbeat.Start();
                worker = Task.Run(() => {
                    for (int batch = 0; batch < 12; batch++)
                    {
                        for (int line = 0; line < 2000; line++) window.QueueLog("warning: " + new string('x', 300));
                        Thread.Sleep(150);
                    }
                    window.QueueLog("Final compiler line");
                });
                await worker;
                await Task.Delay(500);
                Check(ticks > 20, "UI heartbeat stopped during compiler output.");
                Check(maxGap < 1000, "UI stalled for " + maxGap + " ms.");
                Check(Field<ConcurrentQueue<string>>(window, "pendingLog").Count <= 2000, "Output queue grew without limit.");
                Check(Field<TextBox>(window, "log").Text.Length < 145000, "Visible log grew without limit.");
                Check(Field<TextBox>(window, "log").Text.Length > 0, "No output displayed.");
                // Explicit phase/progress tests with no pending historical lines.
                string ignored;
                while (Field<ConcurrentQueue<string>>(window, "pendingLog").TryDequeue(out ignored)) { }
                typeof(BuilderWindow).GetField("busy", BindingFlags.NonPublic | BindingFlags.Instance).SetValue(window, true);
                window.QueueLog("[42/131] Compiling generated/x86_recomp.41.cpp");
                window.FlushLog();
                Check(Field<TextBlock>(window, "phase").Text.Contains("42 of 131"), "Compilation status did not update.");
                Check(!Field<ProgressBar>(window, "progress").IsIndeterminate, "Compile progress was not determinate.");
                window.QueueLog("Creating import ZIP. This can take several minutes...");
                window.QueueLog("ZIP: 1024.0 MiB packed | assets/Default/data.bin");
                window.FlushLog();
                Check(Field<TextBlock>(window, "phase").Text.Contains("Packaging"), "Packaging phase missing.");
                Check(Field<TextBlock>(window, "detail").Text.Contains("1024.0 MiB"), "ZIP progress missing.");
                typeof(BuilderWindow).GetField("busy", BindingFlags.NonPublic | BindingFlags.Instance).SetValue(window, false);
                // Keep inspection useful: render a representative busy state without capturing the desktop.
                if (args.Length == 1)
                {
                    window.UpdateLayout();
                    RenderTargetBitmap image = new RenderTargetBitmap((int)window.ActualWidth, (int)window.ActualHeight, 96, 96, PixelFormats.Pbgra32);
                    image.Render(window);
                    PngBitmapEncoder encoder = new PngBitmapEncoder();
                    encoder.Frames.Add(BitmapFrame.Create(image));
                    using (Stream stream = File.Create(args[0])) encoder.Save(stream);
                }
                Console.WriteLine("PASS: 24,000-line flood, UI heartbeat (max gap " + maxGap + " ms), bounded queue/log, compile and ZIP progress.");
            }
            catch (Exception e) { failure = e; }
            finally
            {
                heartbeat.Stop();
                typeof(BuilderWindow).GetField("busy", BindingFlags.NonPublic | BindingFlags.Instance).SetValue(window, false);
                window.Close();
            }
        };
        app.Run(window);
        if (failure != null) { Console.Error.WriteLine(failure); return 1; }
        return 0;
    }
}
