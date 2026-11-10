// Starts Kodi on a Samsung Tizen TV.
//
// A TV runs only .NET and web apps, and does not let an app start a program from its
// package, which is read-only and not executable. So this app copies kodi.bin into its
// data directory, runs it there as a normal Linux program and ends with it. Kodi opens
// its own Wayland window and speaks to the compositor, the sound server and the video
// decoder of the TV itself.
using System;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Tizen.Applications;

class KodiHost : CoreUIApplication
{
    const string LogTag = "Kodi";

    [DllImport("libc.so.6")] static extern int chmod(string path, int mode);
    [DllImport("libc.so.6")] static extern int kill(int pid, int signal);

    Process kodi;
    volatile bool kodiRunning;

    // Development builds send Kodi's log to the address in res/kodi-log-url, as nothing
    // else can read the data directory of an app on a TV.
    string logUrl;
    string[] logFiles;
    long[] logSent;
    readonly object logLock = new object();

    // ProcessStartInfo.Arguments is split like a Windows command line, also on Linux. The
    // .NET of older TVs has no ArgumentList.
    static string QuoteArgument(string arg)
    {
        var quoted = new StringBuilder("\"");
        int backslashes = 0;
        foreach (char c in arg)
        {
            if (c == '\\')
            {
                ++backslashes;
                continue;
            }
            // Backslashes are literal unless they precede a quote
            quoted.Append('\\', c == '"' ? backslashes * 2 + 1 : backslashes);
            backslashes = 0;
            quoted.Append(c);
        }
        quoted.Append('\\', backslashes * 2);
        return quoted.Append('"').ToString();
    }

    void SendLogs()
    {
        lock (logLock)
        {
            using (var client = new HttpClient { Timeout = TimeSpan.FromSeconds(5) })
            {
                for (int i = 0; i < logFiles.Length; ++i)
                {
                    try
                    {
                        if (!File.Exists(logFiles[i]))
                            continue;
                        using (var stream = new FileStream(logFiles[i], FileMode.Open, FileAccess.Read,
                                                           FileShare.ReadWrite | FileShare.Delete))
                        {
                            if (stream.Length < logSent[i])
                                logSent[i] = 0;
                            while (stream.Length > logSent[i])
                            {
                                stream.Seek(logSent[i], SeekOrigin.Begin);
                                var buffer = new byte[Math.Min(stream.Length - logSent[i], 1 << 20)];
                                int read = stream.Read(buffer, 0, buffer.Length);
                                if (read <= 0)
                                    break;
                                var content = new ByteArrayContent(buffer, 0, read);
                                content.Headers.Add("X-Log", Path.GetFileName(logFiles[i]));
                                client.PostAsync(logUrl, content).Wait();
                                logSent[i] += read;
                            }
                        }
                    }
                    catch (Exception)
                    {
                    }
                }
            }
        }
    }

    // Sends Kodi a signal of the app's lifecycle once it handles it: until then, as while
    // the system loads it, the signal would end Kodi.
    void SignalKodi(int signal)
    {
        try
        {
            ulong handled = 0;
            foreach (string line in File.ReadAllLines("/proc/" + kodi.Id + "/status"))
            {
                if (line.StartsWith("SigIgn:") || line.StartsWith("SigCgt:"))
                    handled |= Convert.ToUInt64(line.Substring(7).Trim(), 16);
            }
            if ((handled & (1UL << (signal - 1))) != 0)
                kill(kodi.Id, signal);
        }
        catch (Exception)
        {
        }
    }

    static void CopyIfChanged(string source, string target)
    {
        var from = new FileInfo(source);
        var to = new FileInfo(target);
        if (to.Exists && to.Length == from.Length && to.LastWriteTimeUtc >= from.LastWriteTimeUtc)
            return;
        string partial = target + ".new";
        File.Copy(source, partial, true);
        if (File.Exists(target))
            File.Delete(target);
        File.Move(partial, target);
        chmod(target, 0x1ed); // 0755
    }

    SynchronizationContext mainContext;
    bool logThreadStarted;
    StreamWriter outputWriter;

    // Development builds: what the launcher does goes into Kodi's output log.
    void HostLog(string message)
    {
        Tizen.Log.Info(LogTag, message);
        if (outputWriter != null)
            lock (outputWriter)
                outputWriter.WriteLine("[launcher] " + message);
    }

    protected override void OnCreate()
    {
        base.OnCreate();
        mainContext = SynchronizationContext.Current;
        StartKodi();
    }

    // The app ends with Kodi. Exit() only works on the main thread, and an app that
    // stays without Kodi would show nothing when it is started again.
    void ExitApp()
    {
        HostLog("exiting");
        if (mainContext != null)
            mainContext.Post(_ => Exit(), null);
        else
            Exit();
        new Thread(() =>
        {
            Thread.Sleep(3000);
            Environment.Exit(0);
        }) { IsBackground = true }.Start();
    }

    void StartKodi()
    {
        string app = Path.GetFullPath(Path.Combine(DirectoryInfo.Resource, ".."));
        string data = DirectoryInfo.Data;
        string binary = Path.Combine(data, "kodi.bin");
        try
        {
            CopyIfChanged(Path.Combine(app, "lib", "kodi.bin"), binary);
        }
        catch (Exception e)
        {
            Tizen.Log.Error(LogTag, "cannot install kodi.bin: " + e.Message);
            ExitApp();
            return;
        }

        string logUrlFile = Path.Combine(app, "res", "kodi-log-url");
        logUrl = File.Exists(logUrlFile) ? File.ReadAllText(logUrlFile).Trim() : null;
        string output = Path.Combine(data, "kodi-output.log");

        // Development builds may come with settings for a new profile, such as Kodi's web
        // server to control it from a PC.
        string devSettings = Path.Combine(app, "res", "dev-guisettings.xml");
        string userdata = Path.Combine(data, ".kodi", "userdata");
        if (File.Exists(devSettings) && !File.Exists(Path.Combine(userdata, "guisettings.xml")))
        {
            Directory.CreateDirectory(userdata);
            File.Copy(devSettings, Path.Combine(userdata, "guisettings.xml"));
        }

        var start = new ProcessStartInfo(binary) { UseShellExecute = false, WorkingDirectory = data };
        // Development builds may start Kodi with arguments, such as a file to play.
        string argsFile = Path.Combine(app, "res", "kodi-args");
        if (File.Exists(argsFile))
        {
            var args = new StringBuilder();
            foreach (string arg in File.ReadAllLines(argsFile))
            {
                if (arg.Length > 0)
                    args.Append(args.Length > 0 ? " " : "").Append(QuoteArgument(arg));
            }
            start.Arguments = args.ToString();
        }
        if (logUrl != null)
        {
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
        }
        start.Environment["HOME"] = data;
        start.Environment["KODI_HOME"] = Path.Combine(app, "res", "kodi");
        start.Environment["KODI_BIN_HOME"] = Path.Combine(app, "lib", "kodi");
        start.Environment["LD_LIBRARY_PATH"] = Path.Combine(app, "lib");
        // Python's standard library is in lib/python3.x of the package.
        start.Environment["PYTHONHOME"] = app;
        // OpenSSL looks for the CA certificates where they were on the build machine.
        start.Environment["SSL_CERT_FILE"] = Path.Combine(app, "res", "kodi", "system", "certs", "cacert.pem");
        start.Environment["KODI_TIZEN_APP_ID"] = ApplicationInfo.ApplicationId;
        start.Environment["KODI_TIZEN_SHARED"] = DirectoryInfo.SharedData ?? "";
        start.Environment["KODI_TIZEN_EXTERNAL"] = DirectoryInfo.ExternalData ?? "";
        try
        {
            kodi = Process.Start(start);
            kodi.EnableRaisingEvents = true;
            if (logUrl != null)
            {
                var writer = new StreamWriter(output, false, Encoding.UTF8) { AutoFlush = true };
                outputWriter = writer;
                HostLog("kodi.bin started, pid " + kodi.Id);
                kodi.OutputDataReceived += (sender, args) => { if (args.Data != null) lock (writer) writer.WriteLine(args.Data); };
                kodi.ErrorDataReceived += (sender, args) => { if (args.Data != null) lock (writer) writer.WriteLine(args.Data); };
                kodi.BeginOutputReadLine();
                kodi.BeginErrorReadLine();
                logFiles = new[] { output, Path.Combine(data, ".kodi", "temp", "kodi.log") };
                logSent = new long[logFiles.Length];
                if (!logThreadStarted)
                {
                    logThreadStarted = true;
                    new Thread(() =>
                    {
                        while (true)
                        {
                            SendLogs();
                            Thread.Sleep(2000);
                        }
                    }) { IsBackground = true }.Start();
                }
            }
            // The app's process does not learn when a child ends (Process.Exited never
            // fires on a TV), so Kodi's process is watched.
            kodiRunning = true;
            int pid = kodi.Id;
            new Thread(() =>
            {
                while (kill(pid, 0) == 0)
                    Thread.Sleep(500);
                kodiRunning = false;
                HostLog("kodi.bin ended");
                if (logUrl != null)
                    SendLogs();
                ExitApp();
            }) { IsBackground = true }.Start();
        }
        catch (Exception e)
        {
            Tizen.Log.Error(LogTag, "cannot start kodi.bin: " + e.Message);
            ExitApp();
        }
    }

    // Kodi stops playing when the app goes to the background and comes to the front when
    // it is resumed (see WinSystemWaylandTizen).
    protected override void OnPause()
    {
        if (kodiRunning)
            SignalKodi(10); // SIGUSR1
        base.OnPause();
    }

    // Starting the app again while it runs: Kodi may have ended in it, or be in the
    // background, from where its window has to come to the front by itself.
    protected override void OnAppControlReceived(AppControlReceivedEventArgs e)
    {
        HostLog("app control received");
        if (kodiRunning)
            SignalKodi(12); // SIGUSR2
        else if (kodi != null)
            StartKodi();
        base.OnAppControlReceived(e);
    }

    protected override void OnResume()
    {
        if (kodiRunning)
            SignalKodi(12); // SIGUSR2
        else if (kodi != null)
            StartKodi();
        base.OnResume();
    }

    protected override void OnTerminate()
    {
        // Kodi saves its settings on SIGTERM and quits.
        if (kodiRunning)
        {
            kill(kodi.Id, 15);
            for (int i = 0; i < 50 && kodiRunning; ++i)
                Thread.Sleep(100);
        }
        base.OnTerminate();
    }

    static void Main(string[] args)
    {
        new KodiHost().Run(args);
    }
}
