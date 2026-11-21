using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using UnityEditor;
using UnityEditor.PackageManager;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace MoePus.Dxcb5.Editor
{
    public enum Dxcb5InstallState
    {
        NotInstalled,
        Installed,
        PartialOrCorrupt,
        UnsupportedPlatform
    }

    public struct Dxcb5Config
    {
        public bool Enabled;
        public bool NanSafe;
        public bool LogStats;
        public bool RequirePackage;
    }

    public struct Dxcb5FallbackEntry
    {
        public string Target;
        public string Entrypoint;
        public string Reason;
    }

    public struct Dxcb5StatsSummary
    {
        public int Hits;
        public int Fallbacks;
        public double AvgHitMs;
        public List<Dxcb5FallbackEntry> RecentFallbacks;
    }

    [InitializeOnLoad]
    public static class Dxcb5Installer
    {
        private const string PackageName = "com.moepus.dxcb5";
        private const string ProxyDllName = "D3DCompiler_47.dll";
        private const string BackupDllName = "D3DCompiler_47_fxc.dll";
        private const string CompilerDllName = "dxcb5_dxcompiler.dll";
        private const string ProxyPayloadName = "D3DCompiler_47.dll.bin";
        private const string CompilerPayloadName = "dxcb5_dxcompiler.dll.bin";

        static Dxcb5Installer()
        {
            if (!IsWindowsEditor)
                return;
            string projectRoot = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
            Environment.SetEnvironmentVariable("DXCB5_UNITY_PROJECT_DIR", projectRoot);
            Events.registeringPackages += OnRegisteringPackages;
        }

        private static void OnRegisteringPackages(PackageRegistrationEventArgs args)
        {
            if (args == null || args.removed == null)
                return;
            bool removingSelf = false;
            foreach (var pkg in args.removed)
            {
                if (pkg != null && pkg.name == PackageName)
                {
                    removingSelf = true;
                    break;
                }
            }
            if (!removingSelf)
                return;

            Environment.SetEnvironmentVariable("DXCB5_UNITY_PROJECT_DIR", null);
            if (GetInstallState() == Dxcb5InstallState.Installed)
            {
                bool restoreNow = EditorUtility.DisplayDialog(
                    "DXCB5 Package Removed",
                    "com.moepus.dxcb5 is being removed.\n\n" +
                    "Restore Unity's original FXC D3DCompiler_47.dll now?\n\n" +
                    "(Note: In default Project-Scoped mode, stock FXC is already restored automatically once this package is removed.)",
                    "Restore Original FXC Now",
                    "Keep Proxy for Other Projects");
                if (restoreNow)
                {
                    if (!UninstallProxy(out string msg))
                        Debug.LogWarning("[DXCB5] Auto-restore failed: " + msg);
                }
            }
        }

        public static bool IsWindowsEditor =>
            Application.platform == RuntimePlatform.WindowsEditor;

        public static string GetUnityToolsDir()
        {
            return Path.GetFullPath(Path.Combine(EditorApplication.applicationContentsPath, "Tools"));
        }

        public static string GetConfigDir()
        {
            string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            return Path.Combine(localAppData, "DXCB5Unity");
        }

        public static string GetConfigIniPath() => Path.Combine(GetConfigDir(), "config.ini");
        public static string GetStatsLogPath() => Path.Combine(GetConfigDir(), "stats.log");

        public static string GetNativePayloadDir()
        {
            var pkg = UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(Dxcb5Installer).Assembly);
            if (pkg != null && !string.IsNullOrEmpty(pkg.resolvedPath))
            {
                return Path.Combine(pkg.resolvedPath, "Editor", "Native", "win-x64");
            }
            return Path.GetFullPath("Packages/com.moepus.dxcb5/Editor/Native/win-x64");
        }

        public static bool HasNativePayloads(out string missingDetail)
        {
            string dir = GetNativePayloadDir();
            string proxy = Path.Combine(dir, ProxyPayloadName);
            string comp = Path.Combine(dir, CompilerPayloadName);
            if (!File.Exists(proxy) || !File.Exists(comp))
            {
                missingDetail = $"Missing native binaries under:\n{dir}\n" +
                                $"Expected: {ProxyPayloadName} and {CompilerPayloadName}";
                return false;
            }
            missingDetail = string.Empty;
            return true;
        }

        public static Dxcb5InstallState GetInstallState()
        {
            if (!IsWindowsEditor)
                return Dxcb5InstallState.UnsupportedPlatform;

            string tools = GetUnityToolsDir();
            string proxyPath = Path.Combine(tools, ProxyDllName);
            string backupPath = Path.Combine(tools, BackupDllName);
            string compPath = Path.Combine(tools, CompilerDllName);

            bool hasBackup = File.Exists(backupPath);
            bool hasComp = File.Exists(compPath);
            bool hasMain = File.Exists(proxyPath);

            if (!hasBackup && !hasComp)
                return Dxcb5InstallState.NotInstalled;
            if (hasBackup && hasComp && hasMain)
                return Dxcb5InstallState.Installed;
            return Dxcb5InstallState.PartialOrCorrupt;
        }

        public static bool InstallProxy(out string message)
        {
            if (!IsWindowsEditor)
            {
                message = "DXCB5 proxy is only supported on Windows x64 Unity Editor.";
                return false;
            }
            if (!HasNativePayloads(out message))
                return false;

            string tools = GetUnityToolsDir();
            string payloadDir = GetNativePayloadDir();
            string srcProxy = Path.Combine(payloadDir, ProxyPayloadName);
            string srcComp = Path.Combine(payloadDir, CompilerPayloadName);
            string dstProxy = Path.Combine(tools, ProxyDllName);
            string dstBackup = Path.Combine(tools, BackupDllName);
            string dstComp = Path.Combine(tools, CompilerDllName);

            if (!File.Exists(dstProxy) && !File.Exists(dstBackup))
            {
                message = $"Original {ProxyDllName} was not found in {tools}.";
                return false;
            }

            var sb = new StringBuilder();
            sb.AppendLine("$ErrorActionPreference = 'Stop'");
            sb.AppendLine($"$tools = '{EscapePs(tools)}'");
            sb.AppendLine($"$srcProxy = '{EscapePs(srcProxy)}'");
            sb.AppendLine($"$srcComp = '{EscapePs(srcComp)}'");
            sb.AppendLine($"$dstProxy = '{EscapePs(dstProxy)}'");
            sb.AppendLine($"$dstBackup = '{EscapePs(dstBackup)}'");
            sb.AppendLine($"$dstComp = '{EscapePs(dstComp)}'");
            sb.AppendLine("Get-Process -Name 'UnityShaderCompiler' -ErrorAction SilentlyContinue | ForEach-Object {");
            sb.AppendLine("  try { if ($_.Path -and $_.Path.StartsWith($tools, [System.StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $_.Id -Force } } catch {}");
            sb.AppendLine("}");
            sb.AppendLine("Start-Sleep -Milliseconds 250");
            sb.AppendLine("if (-not (Test-Path -LiteralPath $dstBackup)) {");
            sb.AppendLine("  Copy-Item -LiteralPath $dstProxy -Destination $dstBackup -Force");
            sb.AppendLine("}");
            sb.AppendLine("Copy-Item -LiteralPath $srcComp -Destination $dstComp -Force");
            sb.AppendLine("Copy-Item -LiteralPath $srcProxy -Destination $dstProxy -Force");
            sb.AppendLine("if (-not (Test-Path -LiteralPath $dstBackup)) { throw 'FXC backup verification failed.' }");

            bool ok = RunPowerShell(sb.ToString(), NeedsElevation(tools), out message);
            if (ok)
            {
                Dxcb5Config cfg = LoadConfig();
                SaveConfig(cfg);
                WriteStandaloneRestoreScript(tools);
                message = $"DXCB5 proxy installed to:\n{tools}";
                Debug.Log("[DXCB5] " + message);
            }
            return ok;
        }

        public static bool UninstallProxy(out string message)
        {
            if (!IsWindowsEditor)
            {
                message = "Unsupported platform.";
                return false;
            }
            string tools = GetUnityToolsDir();
            string dstProxy = Path.Combine(tools, ProxyDllName);
            string dstBackup = Path.Combine(tools, BackupDllName);
            string dstComp = Path.Combine(tools, CompilerDllName);

            var sb = new StringBuilder();
            sb.AppendLine("$ErrorActionPreference = 'Stop'");
            sb.AppendLine($"$tools = '{EscapePs(tools)}'");
            sb.AppendLine($"$dstProxy = '{EscapePs(dstProxy)}'");
            sb.AppendLine($"$dstBackup = '{EscapePs(dstBackup)}'");
            sb.AppendLine($"$dstComp = '{EscapePs(dstComp)}'");
            sb.AppendLine("Get-Process -Name 'UnityShaderCompiler' -ErrorAction SilentlyContinue | ForEach-Object {");
            sb.AppendLine("  try { if ($_.Path -and $_.Path.StartsWith($tools, [System.StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $_.Id -Force } } catch {}");
            sb.AppendLine("}");
            sb.AppendLine("Start-Sleep -Milliseconds 250");
            sb.AppendLine("if (Test-Path -LiteralPath $dstBackup) {");
            sb.AppendLine("  Copy-Item -LiteralPath $dstBackup -Destination $dstProxy -Force");
            sb.AppendLine("  Remove-Item -LiteralPath $dstBackup -Force");
            sb.AppendLine("}");
            sb.AppendLine("if (Test-Path -LiteralPath $dstComp) {");
            sb.AppendLine("  Remove-Item -LiteralPath $dstComp -Force");
            sb.AppendLine("}");

            bool ok = RunPowerShell(sb.ToString(), NeedsElevation(tools), out message);
            if (ok)
            {
                try
                {
                    string restoreCmd = Path.Combine(GetConfigDir(), "Restore_FXC.cmd");
                    if (File.Exists(restoreCmd)) File.Delete(restoreCmd);
                }
                catch { }
                message = $"Original FXC restored in:\n{tools}";
                Debug.Log("[DXCB5] " + message);
            }
            return ok;
        }

        private static void WriteStandaloneRestoreScript(string tools)
        {
            try
            {
                Directory.CreateDirectory(GetConfigDir());
                string restoreCmd = Path.Combine(GetConfigDir(), "Restore_FXC.cmd");
                string psOneLiner =
                    $"$t='{EscapePs(tools)}'; " +
                    "Get-Process -Name 'UnityShaderCompiler' -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($t,[System.StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force; " +
                    "Start-Sleep -Milliseconds 250; " +
                    "$b=Join-Path $t 'D3DCompiler_47_fxc.dll'; $p=Join-Path $t 'D3DCompiler_47.dll'; $c=Join-Path $t 'dxcb5_dxcompiler.dll'; " +
                    "if (Test-Path -LiteralPath $b) { Copy-Item -LiteralPath $b -Destination $p -Force; Remove-Item -LiteralPath $b -Force }; " +
                    "if (Test-Path -LiteralPath $c) { Remove-Item -LiteralPath $c -Force }";
                string cmdText =
                    "@echo off\r\n" +
                    "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile -ExecutionPolicy Bypass -Command', \\\"" + psOneLiner + "\\\"\"\r\n" +
                    "echo Restored original FXC D3DCompiler_47.dll.\r\n" +
                    "pause\r\n";
                File.WriteAllText(restoreCmd, cmdText, Encoding.ASCII);
            }
            catch { }
        }

        public static Dxcb5Config LoadConfig()
        {
            var cfg = new Dxcb5Config { Enabled = true, NanSafe = false, LogStats = true, RequirePackage = true };
            string path = GetConfigIniPath();
            if (!File.Exists(path))
                return cfg;

            foreach (string raw in File.ReadAllLines(path))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("#") || line.StartsWith(";") || line.StartsWith("["))
                    continue;
                int eq = line.IndexOf('=');
                if (eq <= 0)
                    continue;
                string key = line.Substring(0, eq).Trim().ToLowerInvariant();
                string val = line.Substring(eq + 1).Trim();
                bool flag = val != "0" && !val.Equals("false", StringComparison.OrdinalIgnoreCase);
                if (key == "enabled") cfg.Enabled = flag;
                else if (key == "nan_safe") cfg.NanSafe = flag;
                else if (key == "log_stats") cfg.LogStats = flag;
                else if (key == "require_package") cfg.RequirePackage = flag;
            }
            return cfg;
        }

        public static void SaveConfig(Dxcb5Config cfg)
        {
            string dir = GetConfigDir();
            Directory.CreateDirectory(dir);
            string content =
                "[DXCB5]\r\n" +
                $"enabled={(cfg.Enabled ? 1 : 0)}\r\n" +
                $"nan_safe={(cfg.NanSafe ? 1 : 0)}\r\n" +
                $"log_stats={(cfg.LogStats ? 1 : 0)}\r\n" +
                $"require_package={(cfg.RequirePackage ? 1 : 0)}\r\n";
            File.WriteAllText(GetConfigIniPath(), content, Encoding.ASCII);
        }

        public static Dxcb5StatsSummary LoadStats(int maxRecentFallbacks = 20)
        {
            var summary = new Dxcb5StatsSummary { RecentFallbacks = new List<Dxcb5FallbackEntry>() };
            string path = GetStatsLogPath();
            if (!File.Exists(path))
                return summary;

            double totalMs = 0.0;
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var sr = new StreamReader(fs, Encoding.UTF8))
                {
                    string line;
                    while ((line = sr.ReadLine()) != null)
                    {
                        string[] parts = line.Split('\t');
                        if (parts.Length < 4)
                            continue;
                        if (parts[0] == "HIT")
                        {
                            summary.Hits++;
                            if (double.TryParse(parts[3], NumberStyles.Float, CultureInfo.InvariantCulture, out double ms))
                                totalMs += ms;
                        }
                        else if (parts[0] == "FALLBACK")
                        {
                            summary.Fallbacks++;
                            summary.RecentFallbacks.Add(new Dxcb5FallbackEntry
                            {
                                Target = parts[1],
                                Entrypoint = parts[2],
                                Reason = parts[3]
                            });
                            if (summary.RecentFallbacks.Count > maxRecentFallbacks)
                                summary.RecentFallbacks.RemoveAt(0);
                        }
                    }
                }
            }
            catch (IOException)
            {
            }

            if (summary.Hits > 0)
                summary.AvgHitMs = totalMs / summary.Hits;
            return summary;
        }

        public static void ClearStatsLog()
        {
            string path = GetStatsLogPath();
            if (File.Exists(path))
            {
                try { File.Delete(path); } catch (IOException) { }
            }
        }

        public static bool ClearProjectShaderCache(out string message)
        {
            string cacheDir = Path.GetFullPath(Path.Combine(Application.dataPath, "..", "Library", "ShaderCache"));
            if (!Directory.Exists(cacheDir))
            {
                message = "Library/ShaderCache does not exist (already clean).";
                return true;
            }
            try
            {
                Directory.Delete(cacheDir, true);
                message = "Deleted Library/ShaderCache. Reimport shaders or enter Play Mode to recompile with DXCB5.";
                return true;
            }
            catch (Exception ex)
            {
                message = "Failed to delete Library/ShaderCache: " + ex.Message;
                return false;
            }
        }

        private static bool NeedsElevation(string dir)
        {
            try
            {
                string probe = Path.Combine(dir, ".dxcb5_write_probe_" + Guid.NewGuid().ToString("N"));
                File.WriteAllText(probe, "ok");
                File.Delete(probe);
                return false;
            }
            catch
            {
                return true;
            }
        }

        private static string EscapePs(string s) => s.Replace("'", "''");

        private static bool RunPowerShell(string scriptBody, bool elevate, out string error)
        {
            string tempScript = Path.Combine(Path.GetTempPath(), "dxcb5_unity_" + Guid.NewGuid().ToString("N") + ".ps1");
            try
            {
                File.WriteAllText(tempScript, scriptBody, new UTF8Encoding(false));
                var psi = new ProcessStartInfo
                {
                    FileName = "powershell.exe",
                    Arguments = $"-NoProfile -ExecutionPolicy Bypass -File \"{tempScript}\"",
                    UseShellExecute = elevate,
                    CreateNoWindow = !elevate,
                    WindowStyle = ProcessWindowStyle.Hidden
                };
                if (elevate)
                {
                    psi.Verb = "runas";
                }
                else
                {
                    psi.RedirectStandardError = true;
                    psi.RedirectStandardOutput = true;
                }

                using (Process proc = Process.Start(psi))
                {
                    if (proc == null)
                    {
                        error = "Failed to start powershell.exe.";
                        return false;
                    }
                    string stderr = elevate ? string.Empty : proc.StandardError.ReadToEnd();
                    proc.WaitForExit();
                    if (proc.ExitCode != 0)
                    {
                        error = string.IsNullOrEmpty(stderr)
                            ? $"Installer exited with code {proc.ExitCode}."
                            : stderr.Trim();
                        return false;
                    }
                }
                error = string.Empty;
                return true;
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
            finally
            {
                try { if (File.Exists(tempScript)) File.Delete(tempScript); } catch { }
            }
        }
    }
}
