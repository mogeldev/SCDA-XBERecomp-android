using System;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

internal static class Builder
{
    [STAThread]
    private static void Main()
    {
        Application app = new Application();
        app.Run(new BuilderWindow());
    }

    // ProcessStartInfo on .NET Framework takes a command line, not an argument list.
    internal static string Quote(string value)
    {
        StringBuilder result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in value)
        {
            if (c == '\\') { slashes++; continue; }
            result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
            result.Append(c);
            slashes = 0;
        }
        result.Append('\\', slashes * 2);
        return result.Append('"').ToString();
    }

    internal static void RunProcess(string directory, string executable, Action<string> log, params string[] arguments)
    {
        ProcessStartInfo start = new ProcessStartInfo(Path.Combine(directory, executable));
        start.WorkingDirectory = directory;
        start.Arguments = string.Join(" ", Array.ConvertAll(arguments, Quote));
        start.UseShellExecute = false;
        start.CreateNoWindow = true;
        start.RedirectStandardOutput = true;
        start.RedirectStandardError = true;
        using (Process process = new Process())
        {
            process.StartInfo = start;
            process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) { if (e.Data != null) log(e.Data); };
            process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) { if (e.Data != null) log(e.Data); };
            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();
            if (process.ExitCode != 0)
                throw new IOException(Path.GetFileName(executable) + " failed. See the build log below. Completed compilations are kept for retry.");
        }
    }

    internal static void RunScript(string directory, string script, Action<string> log, params string[] arguments)
    {
        string powershell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
            @"WindowsPowerShell\v1.0\powershell.exe");
        string[] options = { "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(directory, script) };
        string[] combined = new string[options.Length + arguments.Length];
        options.CopyTo(combined, 0);
        arguments.CopyTo(combined, options.Length);
        RunProcess(directory, powershell, log, combined);
    }

    internal static bool IsIso(string path)
    {
        return File.Exists(path) && string.Equals(Path.GetExtension(path), ".iso", StringComparison.OrdinalIgnoreCase);
    }

    internal static void ValidatePaths(string assets, string output)
    {
        if (!IsIso(assets) && (!File.Exists(Path.Combine(assets, "default.xbe"))
            || !Directory.Exists(Path.Combine(assets, "Default")) || !Directory.Exists(Path.Combine(assets, "Media"))))
            throw new IOException("Select an original Xbox ISO or the extracted game folder containing default.xbe, Default and Media.");
        string target = Path.GetFullPath(output);
        string source = Path.GetFullPath(assets).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            + Path.DirectorySeparatorChar;
        if (!target.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) || target.StartsWith(source, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Save the ZIP outside your source game folder, with a .zip extension.");
    }

    internal static void BuildGame(string source, string output, string tools, Action<string> log)
    {
        ValidatePaths(source, output);
        source = Path.GetFullPath(source);
        output = Path.GetFullPath(output);
        tools = Path.GetFullPath(tools);
        foreach (string file in new[] { "build.ps1", "package-game.ps1", "extract-xiso.exe", "compiler/xbe_recomp.exe" })
            if (!File.Exists(Path.Combine(tools, file)))
                throw new IOException("Missing " + file + ". Extract the whole builder package before running the GUI.");
        FileStream lease;
        try
        {
            lease = new FileStream(Path.Combine(tools, "builder.lock"), FileMode.OpenOrCreate,
                FileAccess.ReadWrite, FileShare.None);
        }
        catch (IOException e)
        {
            throw new IOException("Cannot lock the builder folder. Close other builder instances and extract the package to a writable folder.", e);
        }
        using (lease)
        {
            string extracted = null;
            try
            {
                string assets = source;
                if (IsIso(source))
                {
                    extracted = Path.Combine(tools, "iso-" + Guid.NewGuid().ToString("N"));
                    log("Extracting original Xbox ISO with extract-xiso (source image is read-only)...");
                    RunProcess(tools, "extract-xiso.exe", log, "-x", "-d", extracted, source);
                    assets = extracted;
                    ValidatePaths(assets, output);
                }
                log("Building Android ARM64 + x86_64 game code...");
                RunScript(tools, "build.ps1", log, "-Xbe", Path.Combine(assets, "default.xbe"));
                log("Creating import ZIP. This can take several minutes...");
                RunScript(tools, "package-game.ps1", log, "-Assets", assets, "-Library", Path.Combine(tools, "lib"),
                    "-Output", output);
                log("Ready: " + output);
            }
            finally
            {
                if (extracted != null && Directory.Exists(extracted)) Directory.Delete(extracted, true);
            }
        }
    }
}

internal static class FolderPicker
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct BrowseInfo
    {
        public IntPtr owner;
        public IntPtr root;
        public IntPtr displayName;
        [MarshalAs(UnmanagedType.LPWStr)] public string title;
        public uint flags;
        public IntPtr callback;
        public IntPtr data;
        public int image;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SHBrowseForFolder(ref BrowseInfo info);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern bool SHGetPathFromIDList(IntPtr item, StringBuilder path);

    [DllImport("ole32.dll")]
    private static extern void CoTaskMemFree(IntPtr item);

    internal static string Show(Window owner)
    {
        BrowseInfo info = new BrowseInfo {
            owner = new WindowInteropHelper(owner).Handle,
            displayName = Marshal.AllocCoTaskMem(260 * sizeof(char)),
            title = "Select the extracted Xbox game folder",
            flags = 0x0001 | 0x0040
        };
        IntPtr item = IntPtr.Zero;
        try
        {
            item = SHBrowseForFolder(ref info);
            if (item == IntPtr.Zero) return null;
            StringBuilder path = new StringBuilder(260);
            return SHGetPathFromIDList(item, path) ? path.ToString() : null;
        }
        finally
        {
            if (item != IntPtr.Zero) CoTaskMemFree(item);
            Marshal.FreeCoTaskMem(info.displayName);
        }
    }
}

internal sealed class BuilderWindow : Window
{
    private readonly TextBox source = new TextBox();
    private readonly TextBox output = new TextBox();
    private readonly TextBox log = new TextBox();
    private readonly Button browseSource = new Button { Content = "Folder..." };
    private readonly Button browseIso = new Button { Content = "ISO..." };
    private readonly Button browseOutput = new Button { Content = "Save as..." };
    private readonly Button build = new Button { Content = "Build game ZIP" };
    private readonly TextBlock status = new TextBlock();
    private readonly ProgressBar progress = new ProgressBar();
    private readonly TextBlock phase = new TextBlock();
    private readonly TextBlock detail = new TextBlock();
    private readonly TextBlock elapsed = new TextBlock();
    private readonly CheckBox followLog = new CheckBox { Content = "Follow output", IsChecked = true };
    private readonly Stopwatch clock = new Stopwatch();
    private readonly ConcurrentQueue<string> pendingLog = new ConcurrentQueue<string>();
    private readonly object logLock = new object();
    private StreamWriter fullLog;
    private bool logWriteFailed;
    private readonly DispatcherTimer logTimer;
    private readonly string toolsDirectory = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);

    // version.txt beside the EXE, written by package.ps1 from the APK's versionName
    internal static string Version(string tools)
    {
        string file = Path.Combine(tools, "version.txt");
        return File.Exists(file) ? File.ReadAllText(file).Trim() : "unknown";
    }
    private string logPath;
    private bool busy;

    internal BuilderWindow()
    {
        string version = Version(toolsDirectory);
        Title = "SCDA Android Builder " + version;
        Width = 980;
        Height = 760;
        MinWidth = 760;
        MinHeight = 620;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        FontFamily = new FontFamily("Segoe UI");
        FontSize = 14;
        Background = new SolidColorBrush(Color("#101827"));
        Foreground = new SolidColorBrush(Color("#E6EDF7"));
        WindowStyle = WindowStyle.SingleBorderWindow;
        Grid layout = new Grid { Margin = new Thickness(22) };
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(115) });
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(180) });
        for (int i = 0; i < 8; i++) layout.RowDefinitions.Add(new RowDefinition { Height = i == 5 ? new GridLength(1, GridUnitType.Star) : GridLength.Auto });
        Style buttonStyle = new Style(typeof(Button));
        buttonStyle.Setters.Add(new Setter(Control.BackgroundProperty, Brush("#24344C")));
        buttonStyle.Setters.Add(new Setter(Control.ForegroundProperty, Brush("#FFFFFF")));
        buttonStyle.Setters.Add(new Setter(Control.BorderBrushProperty, Brush("#526780")));
        buttonStyle.Setters.Add(new Setter(Control.PaddingProperty, new Thickness(14, 8, 14, 8)));
        buttonStyle.Setters.Add(new Setter(Control.CursorProperty, System.Windows.Input.Cursors.Hand));
        Trigger hover = new Trigger { Property = UIElement.IsMouseOverProperty, Value = true };
        hover.Setters.Add(new Setter(Control.BackgroundProperty, Brush("#385575")));
        hover.Setters.Add(new Setter(Control.ForegroundProperty, Brushes.Black));
        buttonStyle.Triggers.Add(hover);
        Trigger disabled = new Trigger { Property = UIElement.IsEnabledProperty, Value = false };
        disabled.Setters.Add(new Setter(UIElement.OpacityProperty, 0.5));
        buttonStyle.Triggers.Add(disabled);
        Resources.Add(typeof(Button), buttonStyle);
        Content = layout;

        StackPanel header = new StackPanel { Margin = new Thickness(0, 0, 0, 24) };
        header.Children.Add(new TextBlock { Text = "SCDA  /  ANDROID ARM64 + X86_64  /  " + version.ToUpperInvariant(), FontSize = 12,
            FontWeight = FontWeights.SemiBold, Foreground = Brush("#69E2BB") });
        header.Children.Add(new TextBlock { Text = "Prepare your game", FontSize = 30,
            FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 5, 0, 8) });
        header.Children.Add(new TextBlock { Text = "Choose your Xbox ISO or extracted game folder. We'll build a ZIP ready to import on your phone.",
            TextWrapping = TextWrapping.Wrap, Foreground = Brush("#B9C7DA") });
        Add(layout, header, 0, 0, 3);
        AddPathRow(layout, 1, "Game source", source, browseSource, browseIso);
        AddPathRow(layout, 2, "Import ZIP", output, browseOutput, null);
        output.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "SCDA-game.zip");

        browseSource.Click += delegate { string path = FolderPicker.Show(this); if (path != null) source.Text = path; };
        browseIso.Click += delegate {
            OpenFileDialog dialog = new OpenFileDialog { Title = "Select an original Xbox ISO", Filter = "Xbox ISO (*.iso)|*.iso" };
            if (dialog.ShowDialog(this) == true) source.Text = dialog.FileName;
        };
        browseOutput.Click += delegate {
            SaveFileDialog dialog = new SaveFileDialog {
                Title = "Save Android game import ZIP",
                Filter = "Game import ZIP (*.zip)|*.zip",
                FileName = Path.GetFileName(output.Text),
                DefaultExt = "zip"
            };
            if (dialog.ShowDialog(this) == true) output.Text = dialog.FileName;
        };
        build.Click += async delegate { await StartBuild(); };
        build.Background = Brush("#176A56");
        build.FontWeight = FontWeights.SemiBold;
        build.HorizontalAlignment = HorizontalAlignment.Left;
        build.Margin = new Thickness(4, 14, 0, 14);
        Add(layout, build, 3, 1);
        StackPanel activity = new StackPanel();
        Grid activityHeading = new Grid();
        phase.Text = "Ready to build";
        phase.FontWeight = FontWeights.SemiBold;
        phase.FontSize = 16;
        phase.Foreground = Brush("#69E2BB");
        activityHeading.Children.Add(phase);
        elapsed.HorizontalAlignment = HorizontalAlignment.Right;
        elapsed.Foreground = Brush("#B9C7DA");
        activityHeading.Children.Add(elapsed);
        activity.Children.Add(activityHeading);
        detail.Text = "No work running";
        detail.Foreground = Brush("#B9C7DA");
        detail.FontSize = 12;
        detail.Margin = new Thickness(0, 5, 0, 0);
        activity.Children.Add(detail);
        progress.Height = 6;
        progress.Foreground = Brush("#51CCA6");
        progress.Background = Brush("#24344C");
        progress.BorderThickness = new Thickness(0);
        progress.Maximum = 100;
        progress.Margin = new Thickness(0, 12, 0, 14);
        activity.Children.Add(progress);
        Add(layout, activity, 4, 0, 3);
        log.IsReadOnly = true;
        log.AcceptsReturn = true;
        log.VerticalScrollBarVisibility = ScrollBarVisibility.Auto;
        log.HorizontalScrollBarVisibility = ScrollBarVisibility.Auto;
        log.TextWrapping = TextWrapping.NoWrap;
        log.FontFamily = new FontFamily("Consolas");
        log.FontSize = 13;
        log.Background = Brush("#0B1220");
        log.Foreground = Brush("#CFDCEC");
        log.BorderThickness = new Thickness(0);
        log.Padding = new Thickness(12);
        log.UndoLimit = 0;
        System.Windows.Automation.AutomationProperties.SetName(log, "Build log");
        Grid logPanel = new Grid();
        logPanel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        logPanel.RowDefinitions.Add(new RowDefinition());
        DockPanel logHeader = new DockPanel { Margin = new Thickness(0, 0, 0, 8) };
        followLog.Foreground = Brush("#B9C7DA");
        followLog.HorizontalAlignment = HorizontalAlignment.Right;
        DockPanel.SetDock(followLog, Dock.Right);
        logHeader.Children.Add(followLog);
        logHeader.Children.Add(new TextBlock { Text = "BUILD OUTPUT", Foreground = Brush("#B9C7DA"), FontSize = 12 });
        Add(logPanel, logHeader, 0, 0);
        Border logCard = new Border { CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1),
            BorderBrush = Brush("#35465E"), Background = log.Background, Child = log, Padding = new Thickness(2) };
        Add(logPanel, logCard, 1, 0);
        Add(layout, logPanel, 5, 0, 3);
        status.Text = "Ready. Use SCDA Android app " + version + "; other app versions refuse this ZIP. Keep your game ZIP private.";
        status.TextWrapping = TextWrapping.Wrap;
        status.Margin = new Thickness(0, 8, 0, 8);
        Add(layout, status, 6, 0, 3);
        StackPanel footer = new StackPanel();
        WrapPanel actions = new WrapPanel();
        Button licenses = new Button { Content = "Tools / licenses", Margin = new Thickness(0, 0, 8, 0) };
        licenses.Click += delegate { Process.Start("explorer.exe", Builder.Quote(Path.Combine(toolsDirectory, "licenses"))); };
        Button openLog = new Button { Content = "Open full log", Margin = new Thickness(0, 0, 8, 0) };
        openLog.Click += delegate {
            if (logPath != null && File.Exists(logPath)) Process.Start("notepad.exe", Builder.Quote(logPath));
        };
        Button updates = new Button { Content = "Check for updates", Margin = new Thickness(0, 0, 8, 0) };
        updates.Click += delegate { CheckUpdates(updates, version); };
        actions.Children.Add(licenses);
        actions.Children.Add(openLog);
        actions.Children.Add(updates);
        footer.Children.Add(actions);
        footer.Children.Add(new TextBlock { Text = "ISO support: extract-xiso / XboxDev. This product includes software developed by in <in@fishtank.com>.",
            FontSize = 11, Foreground = Brush("#B9C7DA"), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 12, 0, 0) });
        Add(layout, footer, 7, 0, 3);
        // Drain bounded batches at background priority, never one dispatcher callback per compiler line.
        logTimer = new DispatcherTimer(DispatcherPriority.Background, Dispatcher) { Interval = TimeSpan.FromMilliseconds(120) };
        logTimer.Tick += delegate { FlushLog(); };
        logTimer.Start();
        Closed += delegate { logTimer.Stop(); };
        Closing += OnClosing;
    }

    // Only on request: the newest GitHub release (pre-releases included) against this builder's version.
    private void CheckUpdates(Button button, string version)
    {
        button.IsEnabled = false;
        button.Content = "Checking...";
        Task.Run(() =>
        {
            try { return Updates.Newest(Updates.Fetch(version)); }
            catch (Exception) { return "\0"; }
        }).ContinueWith(task =>
        {
            button.IsEnabled = true;
            button.Content = "Check for updates";
            string tag = task.Result;
            if (tag == "\0")
                MessageBox.Show(this, "Could not reach GitHub. Check the internet connection and try again.", Title,
                    MessageBoxButton.OK, MessageBoxImage.Warning);
            else if (tag != null && Updates.IsNewer(tag, version))
            {
                if (MessageBox.Show(this, "Version " + tag.TrimStart('v') + " is available. You have " + version + ".\n\n"
                        + "Download the builder and the APK of that version together; the app only imports game ZIPs from "
                        + "the builder of its own version. Open the release page?", Title, MessageBoxButton.YesNo,
                        MessageBoxImage.Information) == MessageBoxResult.Yes)
                    Process.Start(Updates.Page(tag));
            }
            else MessageBox.Show(this, "You have the newest version (" + version + ").", Title, MessageBoxButton.OK,
                MessageBoxImage.Information);
        }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    private static Color Color(string hex) { return (Color)ColorConverter.ConvertFromString(hex); }
    private static SolidColorBrush Brush(string hex) { return new SolidColorBrush(Color(hex)); }

    internal void QueueLog(string line)
    {
        if (string.IsNullOrWhiteSpace(line)) return;
        string stamped = DateTime.Now.ToString("HH:mm:ss") + "  " + line;
        lock (logLock)
        {
            if (fullLog != null && !logWriteFailed)
            {
                try { fullLog.WriteLine(stamped); }
                catch (IOException) { logWriteFailed = true; }
                catch (UnauthorizedAccessException) { logWriteFailed = true; }
            }
            // The screen is a tail, not a document containing millions of compiler diagnostics.
            string ignored;
            while (pendingLog.Count >= 2000) pendingLog.TryDequeue(out ignored);
            pendingLog.Enqueue(stamped.Length > 4096 ? stamped.Substring(0, 4096) + " [see full log]" : stamped);
        }
    }

    internal void FlushLog()
    {
        if (clock.IsRunning) elapsed.Text = clock.Elapsed.ToString(@"hh\:mm\:ss");
        StringBuilder batch = new StringBuilder();
        string line;
        for (int i = 0; i < 100 && batch.Length < 16000 && pendingLog.TryDequeue(out line); i++)
        {
            UpdatePhase(line.Length > 10 ? line.Substring(10) : line);
            batch.AppendLine(line);
        }
        if (batch.Length == 0) return;
        // Trim only occasionally and cap the visible tail to keep WPF layout/selection cheap.
        if (log.Text.Length > 120000)
        {
            log.Select(0, log.Text.Length - 80000);
            log.SelectedText = "";
            log.CaretIndex = log.Text.Length;
        }
        log.AppendText(batch.ToString());
        if (followLog.IsChecked == true) log.ScrollToEnd();
    }

    private void UpdatePhase(string line)
    {
        if (!busy) return;
        if (line.Contains("Extracting original Xbox ISO")) { phase.Text = "01 / Extracting disc image"; detail.Text = "Reading the ISO. The source image is not modified."; progress.IsIndeterminate = true; }
        else if (line.Contains("Building Android ARM64")) { phase.Text = "02 / Compiling for Android ARM64 + x86_64"; detail.Text = "Completed objects are reused on retry."; progress.IsIndeterminate = true; }
        else if (line.Contains("Creating import ZIP")) { phase.Text = "03 / Packaging your import ZIP"; detail.Text = "This may take a while for a full game dump."; progress.IsIndeterminate = true; }
        else if (line.StartsWith("ZIP:")) { detail.Text = line.Substring(4).Trim(); }
        else
        {
            System.Text.RegularExpressions.Match step = System.Text.RegularExpressions.Regex.Match(line, @"\[(\d+)/(\d+)\]");
            if (step.Success)
            {
                double current, total;
                if (double.TryParse(step.Groups[1].Value, out current) && double.TryParse(step.Groups[2].Value, out total) && total > 0)
                {
                    progress.IsIndeterminate = false;
                    progress.Value = Math.Min(100, 100 * current / total);
                    phase.Text = "02 / Compiling  " + current + " of " + total + " steps";
                }
            }
        }
    }

    private static void Add(Grid grid, UIElement element, int row, int column, int span = 1)
    {
        Grid.SetRow(element, row);
        Grid.SetColumn(element, column);
        if (span > 1) Grid.SetColumnSpan(element, span);
        grid.Children.Add(element);
    }

    private static void AddPathRow(Grid grid, int row, string label, TextBox box, Button first, Button second)
    {
        TextBlock name = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center };
        Add(grid, name, row, 0);
        System.Windows.Automation.AutomationProperties.SetName(box, label);
        box.Margin = new Thickness(4, 4, 8, 4);
        box.Padding = new Thickness(10, 8, 10, 8);
        box.Background = Brush("#19263A");
        box.Foreground = Brushes.White;
        box.CaretBrush = Brushes.White;
        box.BorderBrush = Brush("#526780");
        box.VerticalContentAlignment = VerticalAlignment.Center;
        Add(grid, box, row, 1);
        StackPanel buttons = new StackPanel { Orientation = Orientation.Horizontal };
        first.Margin = new Thickness(2);
        buttons.Children.Add(first);
        if (second != null) { second.Margin = new Thickness(2); buttons.Children.Add(second); }
        Add(grid, buttons, row, 2);
    }

    private async Task StartBuild()
    {
        if (busy) return;
        string assets = source.Text.Trim();
        string destination = output.Text.Trim();
        busy = true;
        source.IsEnabled = output.IsEnabled = browseSource.IsEnabled = browseIso.IsEnabled = browseOutput.IsEnabled = build.IsEnabled = false;
        phase.Text = "Checking your game files";
        phase.Foreground = Brush("#69E2BB");
        status.Text = "Working in the background. You can resize the window or review the log.";
        detail.Text = "Preparing...";
        progress.IsIndeterminate = true;
        clock.Restart();
        try
        {
            // Even validation can block on a slow drive or network share.
            bool exists = await Task.Run(() => { Builder.ValidatePaths(assets, destination); return File.Exists(destination); });
            if (exists && MessageBox.Show(this, "Replace the existing ZIP?", Title, MessageBoxButton.YesNo,
                    MessageBoxImage.Question) != MessageBoxResult.Yes) { phase.Text = "Ready to build"; return; }
            log.Clear();
            string ignored;
            while (pendingLog.TryDequeue(out ignored)) { }
            await Task.Run(() => {
                logPath = Path.Combine(toolsDirectory, "build.log");
                using (FileStream lease = new FileStream(Path.Combine(toolsDirectory, "gui.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
                using (StreamWriter writer = new StreamWriter(logPath, false, Encoding.UTF8))
                {
                    lock (logLock) { fullLog = writer; logWriteFailed = false; }
                    try
                    {
                        QueueLog("SCDA Android builder | " + DateTime.Now.ToString("yyyy-MM-dd"));
                        QueueLog("Source: " + assets);
                        QueueLog("Output: " + destination);
                        Builder.BuildGame(assets, destination, toolsDirectory, QueueLog);
                    }
                    catch (Exception e) { QueueLog("ERROR: " + e.Message); throw; }
                    finally { lock (logLock) { fullLog = null; } }
                }
            });
            phase.Text = "Your game ZIP is ready";
            detail.Text = "Safe to copy to your phone.";
            progress.Value = 100;
            status.Text = "Copy the ZIP to your phone's Downloads, then choose Import game ZIP in SCDA. Keep the ZIP private.";
            if (logWriteFailed) status.Text += " Full log could not be written; check disk space.";
        }
        catch (Exception e)
        {
            QueueLog("ERROR: " + e.Message);
            phase.Text = "Build needs attention";
            detail.Text = "See the output below or open the full log.";
            phase.Foreground = Brush("#FFBC85");
            status.Text = e.Message + " Fix the issue and retry; completed compilations are kept.";
        }
        finally
        {
            busy = false;
            clock.Stop();
            elapsed.Text = clock.Elapsed.ToString(@"hh\:mm\:ss");
            source.IsEnabled = output.IsEnabled = browseSource.IsEnabled = browseIso.IsEnabled = browseOutput.IsEnabled = build.IsEnabled = true;
            progress.IsIndeterminate = false;
        }
    }

    private void OnClosing(object sender, CancelEventArgs e)
    {
        if (busy)
        {
            e.Cancel = true;
            MessageBox.Show(this, "Please wait for the build to finish before closing. Completed objects are kept if a build fails.",
                Title, MessageBoxButton.OK, MessageBoxImage.Information);
        }
    }
}

// The GitHub release check; android/.../UpdateCheck.java compares the same way.
internal static class Updates
{
    private const string Repository = "mogeldev/SCDA-XBERecomp-android";
    private static readonly System.Text.RegularExpressions.Regex Tag =
        new System.Text.RegularExpressions.Regex(@"""tag_name""\s*:\s*""([^""]+)""");
    // RELEASING.md versions: 0.1.0-testing.N, then 0.1.0-rc.N, then 0.1.0
    private static readonly System.Text.RegularExpressions.Regex Version =
        new System.Text.RegularExpressions.Regex(@"^v?(\d+)\.(\d+)\.(\d+)(?:-(testing|rc)\.(\d+))?$");

    internal static string Page(string tag) { return "https://github.com/" + Repository + "/releases/tag/" + tag; }

    // major, minor, patch, stage (testing < rc < stable), number; null for any other name
    internal static long[] Parse(string version)
    {
        System.Text.RegularExpressions.Match m = Version.Match((version ?? "").Trim());
        if (!m.Success) return null;
        int stage = !m.Groups[4].Success ? 2 : m.Groups[4].Value == "rc" ? 1 : 0;
        return new[] { long.Parse(m.Groups[1].Value), long.Parse(m.Groups[2].Value), long.Parse(m.Groups[3].Value), stage,
            m.Groups[5].Success ? long.Parse(m.Groups[5].Value) : 0 };
    }

    private static int Compare(long[] a, long[] b)
    {
        for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return a[i].CompareTo(b[i]);
        return 0;
    }

    internal static string Newest(string json)
    {
        string best = null;
        long[] bestVersion = null;
        foreach (System.Text.RegularExpressions.Match m in Tag.Matches(json))
        {
            long[] version = Parse(m.Groups[1].Value);
            if (version != null && (bestVersion == null || Compare(version, bestVersion) > 0))
            {
                best = m.Groups[1].Value;
                bestVersion = version;
            }
        }
        return best;
    }

    // an unknown own version counts every release as newer
    internal static bool IsNewer(string tag, string current)
    {
        long[] mine = Parse(current);
        return mine == null || Compare(Parse(tag), mine) > 0;
    }

    internal static string Fetch(string current)
    {
        System.Net.ServicePointManager.SecurityProtocol |= System.Net.SecurityProtocolType.Tls12;
        System.Net.HttpWebRequest request = (System.Net.HttpWebRequest)System.Net.WebRequest.Create(
            "https://api.github.com/repos/" + Repository + "/releases?per_page=30");
        request.Timeout = 10000;
        request.ReadWriteTimeout = 10000;
        request.Accept = "application/vnd.github+json";
        request.UserAgent = "SCDA-Android-Builder/" + current;
        using (System.Net.WebResponse response = request.GetResponse())
        using (StreamReader reader = new StreamReader(response.GetResponseStream(), Encoding.UTF8))
            return reader.ReadToEnd();
    }
}
