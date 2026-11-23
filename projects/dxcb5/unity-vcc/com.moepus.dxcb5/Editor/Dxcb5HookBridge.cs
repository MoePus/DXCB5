// Distributed under the University of Illinois Open Source License.
#if UNITY_EDITOR_WIN
using System;
using System.IO;
using System.Runtime.InteropServices;
using UnityEditor;
using UnityEditor.PackageManager;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace MoePus.Dxcb5.Editor
{
    /// <summary>
    /// Native Unity Editor integration bridge for DXCB5.
    /// Loads dxcb5_unity_hook.dll in-process inside Unity.exe to:
    ///   1. Route UnityShaderCompiler.exe plugin discovery to dxcb5_cgbatch_plugin.dll;
    ///   2. Cache & deduplicate identical D3D11 shader variants in Unity.exe RAM;
    ///   3. Pass per-shader material specialization manifests to UnityShaderCompiler.exe.
    /// </summary>
    [InitializeOnLoad]
    public static class Dxcb5HookBridge
    {
        const string PackageName = "com.moepus.dxcb5";
        const string NativeDll = "dxcb5_unity_hook";
        const string PrefEnabled = "DXCB5.Enabled";
        const string PrefNanSafe = "DXCB5.NanSafe";
        const string PrefSnippetCache = "DXCB5.SnippetRamCache";

        const uint ExpectedHookAbiVersion = 1u;

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern uint dxcb5_hook_get_abi_version();

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        static extern int dxcb5_hook_install(string pluginDir, string tempDir, int enableSnippetCache);

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern void dxcb5_hook_uninstall();

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern int dxcb5_hook_is_installed();

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern void dxcb5_hook_set_enabled(int enabled);

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern void dxcb5_hook_set_nan_safe(int nanSafe);

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        static extern void dxcb5_hook_register_manifest(string shaderAssetPath, string manifestJson, string pairEntries);

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern void dxcb5_hook_clear_manifests();

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern void dxcb5_hook_clear_cache();

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern void dxcb5_hook_reload_workers();

        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        static extern void dxcb5_hook_get_stats(out ulong ramHits, out ulong ipcCalls, out ulong workersHooked, out ulong snippetHookActive);

        public struct HookStats
        {
            public ulong RamCacheHits;
            public ulong IpcCompiles;
            public ulong WorkersHooked;
            public bool SnippetHookActive;
        }

        static Dxcb5HookBridge()
        {
            string projectRoot = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
            Environment.SetEnvironmentVariable("DXCB5_UNITY_PROJECT_DIR", projectRoot);

            // Keep native hooks alive across C# Domain Reloads (dxcb5_unity_hook.dll
            // is pinned in Unity.exe); uninstall cleanly on Editor exit or package removal.
            EditorApplication.quitting += SafeUninstall;
            Events.registeringPackages += OnRegisteringPackages;

            if (!IsInstalled)
                TryInstall(SnippetCacheEnabled);
            else
                ApplySavedPreferences();
        }

        static void OnRegisteringPackages(PackageRegistrationEventArgs args)
        {
            if (args?.removed == null)
                return;
            foreach (var pkg in args.removed)
            {
                if (pkg != null && pkg.name == PackageName)
                {
                    Environment.SetEnvironmentVariable("DXCB5_UNITY_PROJECT_DIR", null);
                    SafeUninstall();
                    break;
                }
            }
        }

        public static bool Enabled
        {
            get => EditorPrefs.GetBool(PrefEnabled, true);
            set
            {
                EditorPrefs.SetBool(PrefEnabled, value);
                SetEnabled(value);
            }
        }

        public static bool NanSafe
        {
            get => EditorPrefs.GetBool(PrefNanSafe, false);
            set
            {
                EditorPrefs.SetBool(PrefNanSafe, value);
                SetNanSafe(value);
            }
        }

        public static bool SnippetCacheEnabled
        {
            get => EditorPrefs.GetBool(PrefSnippetCache, true);
            set
            {
                EditorPrefs.SetBool(PrefSnippetCache, value);
                if (IsInstalled)
                    TryInstall(value);
            }
        }

        static bool s_WarnedNativeError;

        static void LogNativeWarning(string op, Exception ex)
        {
            if (s_WarnedNativeError)
                return;
            s_WarnedNativeError = true;
            Debug.LogWarning($"[DXCB5] {op} failed ({ex.GetType().Name}): {ex.Message}");
        }

        public static bool IsInstalled
        {
            get
            {
                try { return dxcb5_hook_is_installed() != 0; }
                catch (Exception ex) { LogNativeWarning("dxcb5_hook_is_installed", ex); return false; }
            }
        }

        public static bool TryInstall(bool enableSnippetCache = true)
        {
            try
            {
                uint abi = dxcb5_hook_get_abi_version();
                if (abi != ExpectedHookAbiVersion)
                {
                    Debug.LogWarning($"[DXCB5] dxcb5_unity_hook ABI version mismatch: expected {ExpectedHookAbiVersion}, got {abi}.");
                    return false;
                }
                string pluginDir = ResolveNativePluginDir();
                if (string.IsNullOrEmpty(pluginDir))
                {
                    Debug.LogWarning("[DXCB5] Native plugin directory not found.");
                    return false;
                }
                string projectRoot = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
                string tempDir = Path.Combine(projectRoot, "Temp");
                int rc = dxcb5_hook_install(pluginDir, tempDir, enableSnippetCache ? 1 : 0);
                if (rc != 0)
                {
                    Debug.LogWarning($"[DXCB5] dxcb5_hook_install returned {rc}.");
                    return false;
                }
                ApplySavedPreferences();
                return true;
            }
            catch (Exception ex)
            {
                Debug.LogWarning($"[DXCB5] Failed to install Unity hook: {ex.Message}");
                return false;
            }
        }

        static void ApplySavedPreferences()
        {
            SetEnabled(Enabled);
            SetNanSafe(NanSafe);
        }

        static void Call(string op, Action fn)
        {
            try { fn(); }
            catch (Exception ex) { LogNativeWarning(op, ex); }
        }

        public static void SafeUninstall() => Call("dxcb5_hook_uninstall", dxcb5_hook_uninstall);
        public static void SetEnabled(bool enabled) => Call("dxcb5_hook_set_enabled", () => dxcb5_hook_set_enabled(enabled ? 1 : 0));
        public static void SetNanSafe(bool nanSafe) => Call("dxcb5_hook_set_nan_safe", () => dxcb5_hook_set_nan_safe(nanSafe ? 1 : 0));
        public static void RegisterShaderManifest(string shaderAssetPath, string manifestJson, string pairEntries = "") =>
            Call("dxcb5_hook_register_manifest", () => dxcb5_hook_register_manifest(shaderAssetPath, manifestJson, pairEntries ?? ""));
        public static void ClearManifests() => Call("dxcb5_hook_clear_manifests", dxcb5_hook_clear_manifests);
        public static void ClearCache() => Call("dxcb5_hook_clear_cache", dxcb5_hook_clear_cache);
        public static void ReloadWorkers() => Call("dxcb5_hook_reload_workers", dxcb5_hook_reload_workers);

        public static HookStats GetStats()
        {
            try
            {
                dxcb5_hook_get_stats(out ulong h, out ulong i, out ulong w, out ulong s);
                return new HookStats { RamCacheHits = h, IpcCompiles = i, WorkersHooked = w, SnippetHookActive = s != 0 };
            }
            catch (Exception ex)
            {
                LogNativeWarning("dxcb5_hook_get_stats", ex);
                return default;
            }
        }

        public static bool HasNativePayloads(out string missingDetail)
        {
            string dir = ResolveNativePluginDir();
            if (!string.IsNullOrEmpty(dir))
            {
                missingDetail = string.Empty;
                return true;
            }
            missingDetail = "Missing native DXCB5 binaries (dxcb5_cgbatch_plugin.dll[.bin] and dxcompiler.dll / dxcb5_dxcompiler.dll.bin).";
            return false;
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
                ClearCache();
                message = "Deleted Library/ShaderCache and cleared RAM variant cache.";
                return true;
            }
            catch (Exception ex)
            {
                message = "Failed to delete Library/ShaderCache: " + ex.Message;
                return false;
            }
        }

        static bool HasCgBatchPayloads(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return false;
            bool hasPlugin = File.Exists(Path.Combine(dir, "dxcb5_cgbatch_plugin.dll")) ||
                             File.Exists(Path.Combine(dir, "dxcb5_cgbatch_plugin.dll.bin"));
            bool hasDxc = File.Exists(Path.Combine(dir, "dxcompiler.dll")) ||
                          File.Exists(Path.Combine(dir, "dxcb5_dxcompiler.dll.bin"));
            return hasPlugin && hasDxc;
        }

        public static string ResolveNativePluginDir()
        {
            var pkg = UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(Dxcb5HookBridge).Assembly);
            if (pkg != null && !string.IsNullOrEmpty(pkg.resolvedPath))
            {
                string pkgNative = Path.Combine(pkg.resolvedPath, "Editor", "Native", "win-x64");
                if (HasCgBatchPayloads(pkgNative))
                    return pkgNative;
            }
            string projectRoot = Path.GetFullPath(Path.Combine(Application.dataPath, ".."));
            string[] candidates =
            {
                Path.Combine(projectRoot, "Packages", "com.moepus.dxcb5", "Editor", "Native", "win-x64"),
                Path.Combine(projectRoot, "Assets", "Plugins", "DXCB5", "Editor", "win-x64"),
            };
            foreach (string dir in candidates)
            {
                if (HasCgBatchPayloads(dir))
                    return dir;
            }
            return null;
        }
    }
}
#endif
